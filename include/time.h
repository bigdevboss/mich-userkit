#ifndef MICH64_TIME_H
#define MICH64_TIME_H

/* Seconds since the epoch and the nanoseconds inside the current second,
   the pair utimensat and futimens hand to the kernel. */
typedef long long time_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

/* The two reserved tv_nsec spellings: stamp the current time, or leave the
   field alone. tv_sec is ignored whenever either one rides along. */
#define UTIME_NOW 0x3ffffffeL
#define UTIME_OMIT 0x3fffffffL

#endif
