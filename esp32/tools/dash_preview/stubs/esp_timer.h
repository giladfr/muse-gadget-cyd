#pragma once
#include <stdint.h>
extern int64_t g_fake_us;
static inline int64_t esp_timer_get_time(void){return g_fake_us;}

// One-shot timers (dashboard.c's activation fallback): a detached thread per
// start that fires unless the timer was stopped or restarted meanwhile.
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
typedef struct {
    void (*callback)(void *arg);
    void *arg;
    int dispatch_method;
    const char *name;
    int skip_unhandled_events;
} esp_timer_create_args_t;
typedef struct host_timer {
    esp_timer_create_args_t a;
    volatile unsigned gen;
} *esp_timer_handle_t;
typedef struct { esp_timer_handle_t t; unsigned gen; uint64_t us; } host_timer_shot_t;
static inline void *host_timer_run(void *p) {
    host_timer_shot_t *s = p;
    usleep((useconds_t)s->us);
    if (s->t->gen == s->gen) s->t->a.callback(s->t->a.arg);
    free(s);
    return NULL;
}
static inline int esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out) {
    *out = calloc(1, sizeof(**out));
    if (!*out) return -1;
    (*out)->a = *a;
    return 0;
}
static inline int esp_timer_stop(esp_timer_handle_t t) { t->gen++; return 0; }
static inline int esp_timer_start_once(esp_timer_handle_t t, uint64_t us) {
    host_timer_shot_t *s = malloc(sizeof(*s));
    pthread_t th;
    if (!s) return -1;
    s->t = t;
    s->gen = ++t->gen;
    s->us = us;
    if (pthread_create(&th, NULL, host_timer_run, s) != 0) { free(s); return -1; }
    pthread_detach(th);
    return 0;
}
