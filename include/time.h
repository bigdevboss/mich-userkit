#ifndef MICH64_TIME_H
#define MICH64_TIME_H

/* Seconds since the epoch and the nanoseconds inside the current second,
   the pair utimensat and futimens hand to the kernel. */
typedef long long time_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

/* Seconds and microseconds, the pair gettimeofday reports. */
struct timeval {
    time_t tv_sec;
    long tv_usec;
};

/* The two reserved tv_nsec spellings: stamp the current time, or leave the
   field alone. tv_sec is ignored whenever either one rides along. */
#define UTIME_NOW 0x3ffffffeL
#define UTIME_OMIT 0x3fffffffL

/* Clock selector for clock_gettime and clock_getres. The kernel backs
   realtime with the wall clock anchor and monotonic with the tick counter,
   so both move in whole timer ticks. */
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

int clock_gettime(clockid_t clock, struct timespec *out);
int clock_getres(clockid_t clock, struct timespec *out);
int nanosleep(const struct timespec *requested, struct timespec *remaining);
time_t time(time_t *out);
int gettimeofday(struct timeval *out, void *timezone);

#endif
