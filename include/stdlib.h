#ifndef MICH64_STDLIB_H
#define MICH64_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void free(void *pointer);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);

// Resolve a path to its canonical absolute form, expanding symlinks and
// folding "." and ".." against the directories they pass through. The
// buffer, when the caller provides one, must hold PATH_MAX bytes; a NULL
// buffer makes the call allocate it. Returns the buffer or NULL with
// errno set.
char *realpath(const char *path, char *resolved);

#endif
