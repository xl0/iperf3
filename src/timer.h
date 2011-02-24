#ifndef __TIMER_H
#define __TIMER_H

#include <time.h>
#include <sys/time.h>

struct timer {
    struct timeval begin;
    struct timeval end;
    int (*expired)(struct timer *timer);
};

struct timer *new_timer(time_t sec, suseconds_t usec);

int delay(int64_t ns);

double timeval_to_double(struct timeval *tv);

double timeval_diff(struct timeval *tv0, struct timeval *tv1);

int update_timer(struct timer *tp, time_t sec, suseconds_t usec);

int64_t timer_remaining(struct timer *tp);

void free_timer(struct timer *tp);

int timer_expired(struct timer *);

void cpu_util(double *);

#endif
