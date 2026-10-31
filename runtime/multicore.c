/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: multicore.c
 * Description: The RP2040's second core as a host thread: launching it,
 *              the two 8-deep inter-core FIFOs, the 32 spin locks,
 *              critical sections, mutexes, semaphores and SEV/WFE.
 *              Core 0 is the emulator's own thread, or the firmware's
 *              main thread (mdfw_app.main).
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "hardware/sync.h"
#include "pico.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#include "pico/sync.h"
#include "runtime.h"

static __thread uint s_core; /* 0, or 1 on core 1's thread */
uint get_core_num(void) { return s_core; }

/* ------------------------------------------------------------------ */
/* Events (SEV / WFE)                                                   */
/* ------------------------------------------------------------------ */

static pthread_mutex_t s_event_lock MDFW_KEEP = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_event MDFW_KEEP = PTHREAD_COND_INITIALIZER;
static unsigned s_event_seq;

/* A thread stopped in a condition wait gets the mutex back before it
 * goes, and must not keep it. */
static void unlock_mutex(void *m) { pthread_mutex_unlock((pthread_mutex_t *)m); }

static void wait_event_ms(unsigned ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += (long)ms * 1000000L;
  ts.tv_sec += ts.tv_nsec / 1000000000L;
  ts.tv_nsec %= 1000000000L;
  pthread_mutex_lock(&s_event_lock);
  pthread_cleanup_push(unlock_mutex, &s_event_lock);
  const unsigned seq = s_event_seq;
  while (seq == s_event_seq) {
    if (pthread_cond_timedwait(&s_event, &s_event_lock, &ts) == ETIMEDOUT) break;
  }
  pthread_cleanup_pop(1);
}

void mdfw_sev(void) {
  pthread_mutex_lock(&s_event_lock);
  s_event_seq++;
  pthread_cond_broadcast(&s_event);
  pthread_mutex_unlock(&s_event_lock);
}
void mdfw_wfe(void) { wait_event_ms(1); }
void mdfw_wfi(void) { wait_event_ms(1); }

/* ------------------------------------------------------------------ */
/* FIFOs: fifo[n] carries words from core n to the other core           */
/* ------------------------------------------------------------------ */

#define FIFO_DEPTH 8

typedef struct {
  uint32_t buf[FIFO_DEPTH];
  unsigned head, count;
  pthread_mutex_t lock;
  pthread_cond_t changed;
} fifo_t;

/* MDFW_KEEP for the locks; the words go at power-off, as the SIO's would. */
static fifo_t s_fifo[2] MDFW_KEEP = {
    {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER},
    {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER},
};

static bool fifo_push(fifo_t *f, uint32_t v, int64_t timeout_us) {
  struct timespec ts;
  if (timeout_us >= 0) {
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
    ts.tv_sec += (time_t)(timeout_us / 1000000) + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
  }
  bool ok = true;
  pthread_mutex_lock(&f->lock);
  pthread_cleanup_push(unlock_mutex, &f->lock);
  while (f->count == FIFO_DEPTH) {
    if (timeout_us < 0) {
      pthread_cond_wait(&f->changed, &f->lock);
    } else if (pthread_cond_timedwait(&f->changed, &f->lock, &ts) == ETIMEDOUT) {
      ok = false;
      break;
    }
  }
  if (ok) {
    f->buf[(f->head + f->count) % FIFO_DEPTH] = v;
    f->count++;
    pthread_cond_broadcast(&f->changed);
  }
  pthread_cleanup_pop(1);
  if (ok) __sev();
  return ok;
}

static bool fifo_pop(fifo_t *f, uint32_t *v, int64_t timeout_us) {
  struct timespec ts;
  if (timeout_us >= 0) {
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
    ts.tv_sec += (time_t)(timeout_us / 1000000) + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
  }
  bool ok = true;
  pthread_mutex_lock(&f->lock);
  pthread_cleanup_push(unlock_mutex, &f->lock);
  while (f->count == 0) {
    if (timeout_us < 0) {
      pthread_cond_wait(&f->changed, &f->lock);
    } else if (pthread_cond_timedwait(&f->changed, &f->lock, &ts) == ETIMEDOUT) {
      ok = false;
      break;
    }
  }
  if (ok) {
    *v = f->buf[f->head];
    f->head = (f->head + 1) % FIFO_DEPTH;
    f->count--;
    pthread_cond_broadcast(&f->changed);
  }
  pthread_cleanup_pop(1);
  return ok;
}

static fifo_t *fifo_out(void) { return &s_fifo[s_core]; }
static fifo_t *fifo_in(void) { return &s_fifo[s_core ^ 1u]; }

bool multicore_fifo_rvalid(void) {
  fifo_t *f = fifo_in();
  pthread_mutex_lock(&f->lock);
  const bool r = f->count > 0;
  pthread_mutex_unlock(&f->lock);
  return r;
}

bool multicore_fifo_wready(void) {
  fifo_t *f = fifo_out();
  pthread_mutex_lock(&f->lock);
  const bool r = f->count < FIFO_DEPTH;
  pthread_mutex_unlock(&f->lock);
  return r;
}

void multicore_fifo_push_blocking(uint32_t data) { fifo_push(fifo_out(), data, -1); }
bool multicore_fifo_push_timeout_us(uint32_t data, uint64_t timeout_us) {
  return fifo_push(fifo_out(), data, (int64_t)timeout_us);
}
uint32_t multicore_fifo_pop_blocking(void) {
  uint32_t v = 0;
  fifo_pop(fifo_in(), &v, -1);
  return v;
}
bool multicore_fifo_pop_timeout_us(uint64_t timeout_us, uint32_t *out) {
  return fifo_pop(fifo_in(), out, (int64_t)timeout_us);
}
void multicore_fifo_drain(void) {
  fifo_t *f = fifo_in();
  pthread_mutex_lock(&f->lock);
  f->count = 0;
  pthread_cond_broadcast(&f->changed);
  pthread_mutex_unlock(&f->lock);
}

/* ------------------------------------------------------------------ */
/* Core 1                                                               */
/* ------------------------------------------------------------------ */

static pthread_t s_core1;
static bool s_core1_running;

static void *core1_main(void *arg) {
  void (*entry)(void) = (void (*)(void))arg;
  s_core = 1;
  mdfw_runtime_enter_thread();
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
  entry();
  return NULL;
}

void multicore_launch_core1(void (*entry)(void)) {
  if (s_core1_running) multicore_reset_core1();
  for (int i = 0; i < 2; i++) {
    s_fifo[i].head = s_fifo[i].count = 0;
  }
  s_core1_running = pthread_create(&s_core1, NULL, core1_main, (void *)entry) == 0;
  if (!s_core1_running) panic("cannot start core 1's thread");
}

/* Stops core 1 at its next wait (a FIFO, WFE, sleep or tight loop). */
void multicore_reset_core1(void) {
  if (!s_core1_running) return;
  pthread_cancel(s_core1);
  __sev();
  pthread_join(s_core1, NULL);
  s_core1_running = false;
}

void mdfw_runtime_multicore_stop(void) {
  multicore_reset_core1();
  for (int i = 0; i < 2; i++) s_fifo[i].head = s_fifo[i].count = 0;
}

/* tight_loop_contents(): let the other threads run, and let the
 * firmware's own threads be stopped. */
void mdfw_tight_loop(void) {
  static __thread unsigned n;
  if (mdfw_runtime_own_thread()) pthread_testcancel();
  if ((++n & 63u) == 0) sched_yield();
}

/* ------------------------------------------------------------------ */
/* Spin locks, critical sections, mutexes                               */
/* ------------------------------------------------------------------ */

static spin_lock_t s_spin_regs[32];
static pthread_mutex_t s_spin_mutex[32] MDFW_KEEP;
static pthread_once_t s_spin_once MDFW_KEEP = PTHREAD_ONCE_INIT;
static uint32_t s_spin_claimed;

static void spin_init_all(void) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  for (int i = 0; i < 32; i++) pthread_mutex_init(&s_spin_mutex[i], &a);
}

spin_lock_t *spin_lock_instance(uint lock_num) { return &s_spin_regs[lock_num & 31u]; }
uint spin_lock_get_num(spin_lock_t *lock) { return (uint)(lock - s_spin_regs); }
spin_lock_t *spin_lock_init(uint lock_num) { return spin_lock_instance(lock_num); }
int spin_lock_claim_unused(bool required) {
  for (int i = 16; i < 32; i++) {
    if (!(s_spin_claimed & (1u << i))) {
      s_spin_claimed |= 1u << i;
      return i;
    }
  }
  if (required) panic("no spin locks left");
  return -1;
}
void spin_lock_claim(uint lock_num) { s_spin_claimed |= 1u << (lock_num & 31u); }
void spin_lock_unclaim(uint lock_num) { s_spin_claimed &= ~(1u << (lock_num & 31u)); }
void spin_lock_unsafe_blocking(spin_lock_t *lock) {
  pthread_once(&s_spin_once, spin_init_all);
  pthread_mutex_lock(&s_spin_mutex[spin_lock_get_num(lock) & 31u]);
}
void spin_unlock_unsafe(spin_lock_t *lock) {
  pthread_mutex_unlock(&s_spin_mutex[spin_lock_get_num(lock) & 31u]);
}
uint32_t spin_lock_blocking(spin_lock_t *lock) {
  spin_lock_unsafe_blocking(lock);
  return 0;
}
void spin_unlock(spin_lock_t *lock, uint32_t saved_irq) {
  (void)saved_irq;
  spin_unlock_unsafe(lock);
}

static pthread_mutex_t *new_recursive_mutex(void) {
  pthread_mutex_t *m = malloc(sizeof(*m));
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(m, &a);
  return m;
}

void critical_section_init(critical_section_t *cs) { cs->impl = new_recursive_mutex(); }
void critical_section_init_with_lock_num(critical_section_t *cs, uint lock_num) {
  (void)lock_num;
  critical_section_init(cs);
}
void critical_section_enter_blocking(critical_section_t *cs) {
  pthread_mutex_lock((pthread_mutex_t *)cs->impl);
}
void critical_section_exit(critical_section_t *cs) {
  pthread_mutex_unlock((pthread_mutex_t *)cs->impl);
}
void critical_section_deinit(critical_section_t *cs) {
  pthread_mutex_destroy((pthread_mutex_t *)cs->impl);
  free(cs->impl);
  cs->impl = NULL;
}

void mutex_init(mutex_t *m) { m->impl = new_recursive_mutex(); }
void mutex_enter_blocking(mutex_t *m) { pthread_mutex_lock((pthread_mutex_t *)m->impl); }
bool mutex_try_enter(mutex_t *m, uint32_t *owner_out) {
  if (owner_out) *owner_out = 0;
  return pthread_mutex_trylock((pthread_mutex_t *)m->impl) == 0;
}
void mutex_exit(mutex_t *m) { pthread_mutex_unlock((pthread_mutex_t *)m->impl); }

/* ------------------------------------------------------------------ */
/* Semaphores                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t changed;
} sem_impl_t;

void sem_init(semaphore_t *sem, int16_t initial_permits, int16_t max_permits) {
  sem_impl_t *s = sem->impl;
  if (!s) {
    s = malloc(sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->changed, NULL);
    sem->impl = s;
  }
  pthread_mutex_lock(&s->lock);
  sem->permits = initial_permits;
  sem->max_permits = max_permits;
  pthread_mutex_unlock(&s->lock);
}

int sem_available(semaphore_t *sem) { return sem->permits; }

bool sem_release(semaphore_t *sem) {
  sem_impl_t *s = sem->impl;
  pthread_mutex_lock(&s->lock);
  const bool rc = sem->permits < sem->max_permits;
  if (rc) {
    sem->permits++;
    pthread_cond_broadcast(&s->changed);
  }
  pthread_mutex_unlock(&s->lock);
  if (rc) __sev();
  return rc;
}

void sem_reset(semaphore_t *sem, int16_t permits) {
  sem_impl_t *s = sem->impl;
  pthread_mutex_lock(&s->lock);
  sem->permits = permits;
  pthread_cond_broadcast(&s->changed);
  pthread_mutex_unlock(&s->lock);
}

bool sem_try_acquire(semaphore_t *sem) {
  sem_impl_t *s = sem->impl;
  pthread_mutex_lock(&s->lock);
  const bool rc = sem->permits > 0;
  if (rc) sem->permits--;
  pthread_mutex_unlock(&s->lock);
  return rc;
}

/* Waits in steps of a millisecond of real time, so an emulated deadline
 * is noticed. On the emulator's thread nothing else moves emulated time
 * on, so each step does. */
bool sem_acquire_block_until(semaphore_t *sem, absolute_time_t until) {
  sem_impl_t *s = sem->impl;
  bool rc = true;
  pthread_mutex_lock(&s->lock);
  pthread_cleanup_push(unlock_mutex, &s->lock);
  while (sem->permits <= 0) {
    if (until != at_the_end_of_time && time_us_64() >= until) {
      rc = false;
      break;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&s->changed, &s->lock, &ts);
    if (!mdfw_runtime_own_thread() && until != at_the_end_of_time) {
      mdfw_runtime_sleep(1000);
    }
  }
  if (rc) sem->permits--;
  pthread_cleanup_pop(1);
  return rc;
}

void sem_acquire_blocking(semaphore_t *sem) { sem_acquire_block_until(sem, at_the_end_of_time); }
