#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mich/syscall.h>

/* Page-mapped heap: blocks live in the arena between the heap base and the
   program break, walked in address order, so coalescing only needs the two
   physical neighbors of a freed block. */
#define MALLOC_ALIGN 16
#define MALLOC_MIN_PAYLOAD 16
#define MALLOC_GROW 65536

struct malloc_block {
    size_t size;
    int used;
};

static size_t heap_base;
static size_t heap_break;
static int heap_ready;

static int heap_init(void) {
    if (heap_ready) return 0;
    unsigned long base =
        (unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0);
    if (!base) return -1;
    heap_base = base;
    heap_break = base;
    heap_ready = 1;
    return 0;
}

static struct malloc_block *block_after(struct malloc_block *block) {
    return (struct malloc_block *)((char *)block + sizeof(*block) +
                                   block->size);
}

static void heap_split(struct malloc_block *block, size_t payload) {
    if (block->size >= payload + sizeof(struct malloc_block) +
                       MALLOC_MIN_PAYLOAD) {
        struct malloc_block *tail =
            (struct malloc_block *)((char *)block + sizeof(*block) + payload);
        tail->size = block->size - payload - sizeof(struct malloc_block);
        tail->used = 0;
        block->size = payload;
    }
}

static struct malloc_block *heap_extend(size_t payload) {
    size_t total = payload + sizeof(struct malloc_block);
    size_t chunk = total < MALLOC_GROW ? MALLOC_GROW : total;
    /* The break syscall rejects growth past the bounded heap window by
       returning the unchanged break, which this compare turns into failure. */
    if (mich_syscall1(MICH_SYS_POSIX_BRK, (unsigned long)(heap_break + chunk)) !=
        (long)(heap_break + chunk))
        return 0;
    struct malloc_block *block = (struct malloc_block *)heap_break;
    heap_break += chunk;
    block->size = chunk - sizeof(struct malloc_block);
    block->used = 0;
    return block;
}

static void heap_coalesce(struct malloc_block *block,
                          struct malloc_block *previous) {
    struct malloc_block *next = block_after(block);
    if ((size_t)next < heap_break && !next->used)
        block->size += sizeof(struct malloc_block) + next->size;
    if (previous && !previous->used)
        previous->size += sizeof(struct malloc_block) + block->size;
}

void *malloc(size_t size) {
    if (!size || heap_init()) {
        errno = ENOMEM;
        return 0;
    }
    size = (size + MALLOC_ALIGN - 1) & ~(size_t)(MALLOC_ALIGN - 1);
    struct malloc_block *found = 0;
    for (struct malloc_block *block = (struct malloc_block *)heap_base;
         (size_t)block < heap_break;
         block = block_after(block)) {
        if (!block->used && block->size >= size) {
            found = block;
            break;
        }
    }
    if (!found) {
        found = heap_extend(size);
        if (!found) {
            errno = ENOMEM;
            return 0;
        }
    }
    heap_split(found, size);
    found->used = 1;
    return (char *)found + sizeof(struct malloc_block);
}

void free(void *pointer) {
    if (!pointer || heap_init()) return;
    struct malloc_block *previous = 0;
    for (struct malloc_block *block = (struct malloc_block *)heap_base;
         (size_t)block < heap_break;
         previous = block, block = block_after(block)) {
        if ((char *)block + sizeof(struct malloc_block) == pointer) {
            /* Releasing an already free block would corrupt its neighbors,
               so a double free is ignored instead of merged twice. */
            if (block->used) {
                block->used = 0;
                heap_coalesce(block, previous);
            }
            return;
        }
    }
}

void *calloc(size_t count, size_t size) {
    if (count && size > (size_t)-1 / count) {
        errno = ENOMEM;
        return 0;
    }
    void *pointer = malloc(count * size);
    if (pointer) memset(pointer, 0, count * size);
    return pointer;
}

void *realloc(void *pointer, size_t size) {
    if (!pointer) return malloc(size);
    if (!size) {
        free(pointer);
        return 0;
    }
    if (heap_init()) return 0;
    size_t payload = (size + MALLOC_ALIGN - 1) & ~(size_t)(MALLOC_ALIGN - 1);
    if ((size_t)pointer < heap_base + sizeof(struct malloc_block) ||
        (size_t)pointer > heap_break) {
        errno = EINVAL;
        return 0;
    }
    struct malloc_block *block =
        (struct malloc_block *)((char *)pointer - sizeof(struct malloc_block));
    if (block->size >= payload) {
        heap_split(block, payload);
        return pointer;
    }
    struct malloc_block *next = block_after(block);
    if ((size_t)next < heap_break && !next->used &&
        block->size + sizeof(struct malloc_block) + next->size >= payload) {
        block->size += sizeof(struct malloc_block) + next->size;
        return pointer;
    }
    void *fresh = malloc(size);
    if (!fresh) return 0;
    memcpy(fresh, pointer, block->size);
    free(pointer);
    return fresh;
}

// Trim the canonical prefix back to its parent directory. The root is its
// own parent, the same way the kernel resolves ".." there.
static void realpath_parent(char *resolved) {
    size_t cut = 0;
    for (size_t index = 0; resolved[index]; index++)
        if (resolved[index] == '/' && index) cut = index;
    if (!cut) {
        resolved[0] = '/';
        resolved[1] = 0;
        return;
    }
    resolved[cut] = 0;
}

static int realpath_push(char *resolved, const char *component, size_t length) {
    size_t base = (size_t)strlen(resolved);
    size_t separator = base > 1 ? 1 : 0;
    if (base + separator + length + 1 > PATH_MAX) return -1;
    size_t index = base;
    if (separator) resolved[index++] = '/';
    for (size_t part = 0; part < length; part++) resolved[index++] = component[part];
    resolved[index] = 0;
    return 0;
}

char *realpath(const char *path, char *resolved) {
    if (!path || !path[0]) {
        errno = ENOENT;
        return 0;
    }
    if (!resolved) {
        resolved = malloc(PATH_MAX);
        if (!resolved) {
            errno = ENOMEM;
            return 0;
        }
    }
    // An input longer than the path bound can never resolve: a symlink
    // substitution only ever lengthens what is left to walk.
    size_t input = strlen(path);
    if (input >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return 0;
    }
    char remaining[PATH_MAX];
    char target[PATH_MAX];
    struct stat info;
    if (path[0] == '/') {
        resolved[0] = '/';
        resolved[1] = 0;
    } else if (!getcwd(resolved, PATH_MAX)) {
        return 0;
    }
    for (size_t index = 0; index <= input; index++) remaining[index] = path[index];
    size_t links = 0;
    size_t cursor = 0;
    while (remaining[cursor]) {
        while (remaining[cursor] == '/') cursor++;
        if (!remaining[cursor]) break;
        const char *component = remaining + cursor;
        size_t length = 0;
        while (remaining[cursor] && remaining[cursor] != '/') {
            cursor++;
            length++;
        }
        if (length == 1u && component[0] == '.') continue;
        if (length == 2u && component[0] == '.' && component[1] == '.') {
            realpath_parent(resolved);
            continue;
        }
        // A component joins the answer only after the kernel vouches for
        // it, so the resolution cannot drift from what stat itself sees.
        if (realpath_push(resolved, component, length)) {
            errno = ENAMETOOLONG;
            return 0;
        }
        if (lstat(resolved, &info)) return 0;
        if (!S_ISLNK(info.st_mode)) continue;
        if (++links > SYMLOOP_MAX) {
            errno = ELOOP;
            return 0;
        }
        ssize_t got = readlink(resolved, target, PATH_MAX - 1);
        if (got < 0) return 0;
        target[got] = 0;
        realpath_parent(resolved);
        // The target takes the link's place at the head of what is left:
        // a relative one continues from the link's directory, an absolute
        // one restarts the walk from the root. The tail moves first so
        // the copy never runs over bytes it still needs.
        size_t tail = 0;
        while (remaining[cursor + tail]) tail++;
        if ((size_t)got + 1 + tail + 1 > PATH_MAX) {
            errno = ENAMETOOLONG;
            return 0;
        }
        for (size_t index = tail + 1; index-- > 0;)
            remaining[(size_t)got + 1 + index] = remaining[cursor + index];
        for (size_t index = 0; index < (size_t)got; index++)
            remaining[index] = target[index];
        remaining[got] = '/';
        cursor = 0;
        if (target[0] == '/') {
            resolved[0] = '/';
            resolved[1] = 0;
        }
    }
    return resolved;
}
