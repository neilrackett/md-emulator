/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: async_context.c
 * Description: The Pico SDK's async_context (pico/async_context.h), which
 *              cyw43_arch and lwIP run on. Every context shares one lock,
 *              which is also the network's. A polled context's work is
 *              done when the firmware polls it; a background one's while
 *              a firmware thread waits for work, and on the emulator's
 *              thread at every turn of the main loop, where the RP2040
 *              would do it in an interrupt.
 */

#include <pthread.h>

#include "pico/async_context_poll.h"
#include "pico/async_context_threadsafe_background.h"
#include "runtime.h"
#include "wifi.h"

/* ------------------------------------------------------------------ */
/* The lock                                                             */
/* ------------------------------------------------------------------ */

static pthread_mutex_t s_lock MDFW_KEEP;
static pthread_once_t s_lock_once MDFW_KEEP = PTHREAD_ONCE_INIT;
static __thread unsigned s_depth;
static __thread int s_cancel_state;

static void lock_init(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&s_lock, &attr);
  pthread_mutexattr_destroy(&attr);
}

/* A firmware thread stopped at power-off while it held the lock would
 * keep it for good, so it is not stopped until it lets go. */
void mdfw_wifi_lock(void) {
  int state;
  pthread_once(&s_lock_once, lock_init);
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state);
  pthread_mutex_lock(&s_lock);
  if (s_depth++ == 0) s_cancel_state = state;
}

bool mdfw_wifi_trylock(void) {
  int state;
  pthread_once(&s_lock_once, lock_init);
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state);
  if (pthread_mutex_trylock(&s_lock) != 0) {
    pthread_setcancelstate(state, NULL);
    return false;
  }
  if (s_depth++ == 0) s_cancel_state = state;
  return true;
}

void mdfw_wifi_unlock(void) {
  const bool last = --s_depth == 0;
  pthread_mutex_unlock(&s_lock);
  if (last) pthread_setcancelstate(s_cancel_state, NULL);
}

void async_context_acquire_lock_blocking(async_context_t *context) {
  (void)context;
  mdfw_wifi_lock();
}

void async_context_release_lock(async_context_t *context) {
  (void)context;
  mdfw_wifi_unlock();
}

void async_context_lock_check(async_context_t *context) { (void)context; }

uint32_t async_context_execute_sync(async_context_t *context,
                                    uint32_t (*func)(void *param), void *param) {
  (void)context;
  mdfw_wifi_lock();
  const uint32_t r = func(param);
  mdfw_wifi_unlock();
  return r;
}

/* ------------------------------------------------------------------ */
/* Contexts                                                             */
/* ------------------------------------------------------------------ */

/* The background ones, which the emulator's thread does the work of. */
#define MAX_BACKGROUND 4
static async_context_t *s_background[MAX_BACKGROUND];

static void init_context(async_context_t *c, uint16_t flags) {
  memset(c, 0, sizeof(*c));
  c->flags = flags;
  c->core_num = (uint8_t)get_core_num();
  c->next_time = at_the_end_of_time;
}

bool async_context_poll_init_with_defaults(async_context_poll_t *self) {
  init_context(&self->core,
               ASYNC_CONTEXT_FLAG_POLLED | ASYNC_CONTEXT_FLAG_CALLBACK_FROM_NON_IRQ);
  return true;
}

async_context_threadsafe_background_config_t
async_context_threadsafe_background_default_config(void) {
  const async_context_threadsafe_background_config_t config = {0, NULL};
  return config;
}

bool async_context_threadsafe_background_init(
    async_context_threadsafe_background_t *self,
    async_context_threadsafe_background_config_t *config) {
  (void)config;
  init_context(&self->core, ASYNC_CONTEXT_FLAG_CALLBACK_FROM_IRQ);
  bool ok = false;
  mdfw_wifi_lock();
  for (int i = 0; i < MAX_BACKGROUND && !ok; i++) {
    if (!s_background[i]) {
      s_background[i] = &self->core;
      ok = true;
    }
  }
  mdfw_wifi_unlock();
  return ok;
}

void async_context_deinit(async_context_t *c) {
  mdfw_wifi_lock();
  for (int i = 0; i < MAX_BACKGROUND; i++) {
    if (s_background[i] == c) s_background[i] = NULL;
  }
  c->when_pending_list = NULL;
  c->at_time_list = NULL;
  c->next_time = at_the_end_of_time;
  mdfw_wifi_unlock();
}

/* ------------------------------------------------------------------ */
/* Workers                                                              */
/* ------------------------------------------------------------------ */

static void update_next_time(async_context_t *c) {
  c->next_time = c->at_time_list ? c->at_time_list->next_time : at_the_end_of_time;
}

static bool unlink_at_time(async_context_t *c, async_at_time_worker_t *w) {
  for (async_at_time_worker_t **p = &c->at_time_list; *p; p = &(*p)->next) {
    if (*p == w) {
      *p = w->next;
      w->next = NULL;
      return true;
    }
  }
  return false;
}

bool async_context_add_at_time_worker(async_context_t *c, async_at_time_worker_t *w) {
  mdfw_wifi_lock();
  unlink_at_time(c, w);
  async_at_time_worker_t **p = &c->at_time_list;
  while (*p && (*p)->next_time <= w->next_time) p = &(*p)->next;
  w->next = *p;
  *p = w;
  update_next_time(c);
  mdfw_wifi_unlock();
  return true;
}

bool async_context_remove_at_time_worker(async_context_t *c, async_at_time_worker_t *w) {
  mdfw_wifi_lock();
  const bool found = unlink_at_time(c, w);
  update_next_time(c);
  mdfw_wifi_unlock();
  return found;
}

bool async_context_add_when_pending_worker(async_context_t *c,
                                           async_when_pending_worker_t *w) {
  mdfw_wifi_lock();
  bool there = false;
  for (async_when_pending_worker_t *x = c->when_pending_list; x; x = x->next) {
    if (x == w) there = true;
  }
  if (!there) {
    w->next = c->when_pending_list;
    c->when_pending_list = w;
  }
  mdfw_wifi_unlock();
  return true;
}

bool async_context_remove_when_pending_worker(async_context_t *c,
                                              async_when_pending_worker_t *w) {
  bool found = false;
  mdfw_wifi_lock();
  for (async_when_pending_worker_t **p = &c->when_pending_list; *p; p = &(*p)->next) {
    if (*p == w) {
      *p = w->next;
      w->next = NULL;
      found = true;
      break;
    }
  }
  mdfw_wifi_unlock();
  return found;
}

void async_context_set_work_pending(async_context_t *c, async_when_pending_worker_t *w) {
  (void)c;
  __atomic_store_n(&w->work_pending, true, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* Doing the work (with the lock held)                                  */
/* ------------------------------------------------------------------ */

static bool work_ready(async_context_t *c) {
  mdfw_wifi_check(c);
  for (async_when_pending_worker_t *w = c->when_pending_list; w; w = w->next) {
    if (__atomic_load_n(&w->work_pending, __ATOMIC_ACQUIRE)) return true;
  }
  return time_reached(c->next_time);
}

/* How many times work has been done, by any thread: a background
 * context's waiters wake when the emulator's thread has done some. */
static unsigned s_work_done;

/* True if any worker ran. */
static bool run_work(async_context_t *c) {
  bool did = false;
  mdfw_wifi_check(c);
  async_when_pending_worker_t *next;
  for (async_when_pending_worker_t *w = c->when_pending_list; w; w = next) {
    next = w->next;
    if (__atomic_exchange_n(&w->work_pending, false, __ATOMIC_ACQ_REL)) {
      w->do_work(c, w);
      did = true;
    }
  }
  /* The timeouts due now: one that adds itself again for now waits for
   * the next turn. */
  const absolute_time_t now = get_absolute_time();
  async_at_time_worker_t *due = NULL, **tail = &due, *t, *next_t;
  while ((t = c->at_time_list) && t->next_time <= now) {
    c->at_time_list = t->next;
    t->next = NULL;
    *tail = t;
    tail = &t->next;
  }
  update_next_time(c);
  for (t = due; t; t = next_t) {
    next_t = t->next;
    t->next = NULL;
    t->do_work(c, t);
    did = true;
  }
  if (did) __atomic_add_fetch(&s_work_done, 1, __ATOMIC_RELEASE);
  return did;
}

void async_context_poll(async_context_t *c) {
  mdfw_wifi_lock();
  run_work(c);
  mdfw_wifi_unlock();
}

void async_context_wait_until(async_context_t *c, absolute_time_t until) {
  (void)c;
  sleep_until(until);
}

void async_context_wait_for_work_until(async_context_t *c, absolute_time_t until) {
  const bool own = mdfw_runtime_own_thread();
  const unsigned done = __atomic_load_n(&s_work_done, __ATOMIC_ACQUIRE);
  for (;;) {
    /* Where a firmware thread is stopped at power-off. */
    if (own) pthread_testcancel();
    mdfw_wifi_lock();
    const bool ready = (c->flags & ASYNC_CONTEXT_FLAG_POLLED)
                           ? work_ready(c)
                           : run_work(c) || __atomic_load_n(&s_work_done,
                                                            __ATOMIC_ACQUIRE) != done;
    absolute_time_t next = until;
    if (c->next_time < next) next = c->next_time;
    const absolute_time_t net = mdfw_wifi_next_time(c);
    if (net < next) next = net;
    mdfw_wifi_unlock();
    if (ready || time_reached(next)) return;
    if (!own) {
      /* The emulator's thread: move emulated time on, as sleeping does. */
      sleep_until(next);
    } else {
      mdfw_net_wait(1000);
    }
  }
}

/* The emulator's thread, at every turn of the firmware's main loop: the
 * background contexts' work, unless a firmware thread has the lock (then
 * it waits for the next turn, as an interrupt would). */
void mdfw_runtime_async_turn(void) {
  bool any = false;
  for (int i = 0; i < MAX_BACKGROUND; i++) {
    if (__atomic_load_n(&s_background[i], __ATOMIC_ACQUIRE)) any = true;
  }
  if (!any || !mdfw_wifi_trylock()) return;
  for (int i = 0; i < MAX_BACKGROUND; i++) {
    if (s_background[i]) run_work(s_background[i]);
  }
  mdfw_wifi_unlock();
}
