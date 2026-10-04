#ifndef MICH64_LIMITS_H
#define MICH64_LIMITS_H

/* The path bound the kernel ABI carries (VFS_PATH_MAX), so a userspace
   buffer of this size holds anything the profile can resolve. */
#define PATH_MAX 256

/* Bound on symlink expansions inside one resolution, the classic
   SYMLOOP_MAX guard against link loops. */
#define SYMLOOP_MAX 40

#endif
