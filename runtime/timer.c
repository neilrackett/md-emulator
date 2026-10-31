/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: timer.c
 * Description: The Pico SDK's alarms and repeating timers (pico/time.h)
 *              in emulated time. They fire on the emulator's thread
 *              whenever it passes their time, the way the timer
 *              interrupt would, in step with what the ST is doing. Every
 *              alarm pool is the same one list.
 */

#include <pthread.h>

#include "pico.h"
#include "pico/time.h"
#include "runtime.h"

#define MAX_TIMERS 32

typedef struct {
  alarm_id_t id; /* 0: free */
  uint64_t due_us;
  alarm_callback_t alarm;         /* one of these two */
  repeating_timer_t *repeating;
  void *user_data;
} timer_slot_t;

struct alarm_pool {
  int unused;
};

static alarm_pool_t s_pool;
static timer_slot_t s_timers[MAX_TIMERS];
static alarm_id_t s_next_id = 1;
static uint64_t s_next_due = UINT64_MAX; /* earliest due_us, for a quick no */

/* Recursive: a callback may add or cancel timers. Held while a callback
 * runs, so once a cancel returns that timer's callback is not running. */
static pthread_mutex_t s_lock MDFW_KEEP;
static pthread_once_t s_lock_once MDFW_KEEP = PTHREAD_ONCE_INIT;

static void init_lock(void) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&s_lock, &a);
}

static void lock(void) {
  pthread_once(&s_lock_once, init_lock);
  pthread_mutex_lock(&s_lock);
}

static void unlock(void) { pthread_mutex_unlock(&s_lock); }

static void update_next_due(void) {
  uint64_t next = UINT64_MAX;
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (s_timers[i].id && s_timers[i].due_us < next) next = s_timers[i].due_us;
  }
  __atomic_store_n(&s_next_due, next, __ATOMIC_RELEASE);
}

static alarm_id_t add(uint64_t due_us, alarm_callback_t alarm, repeating_timer_t *rt,
                      void *user_data) {
  lock();
  alarm_id_t id = -1;
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (!s_timers[i].id) {
      id = s_next_id++;
      if (s_next_id <= 0) s_next_id = 1;
      s_timers[i] = (timer_slot_t){id, due_us, alarm, rt, user_data};
      break;
    }
  }
  update_next_due();
  unlock();
  if (id < 0) mdfw_log("no timers left (%d in use)", MAX_TIMERS);
  return id;
}

static bool cancel(alarm_id_t id) {
  bool found = false;
  lock();
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (s_timers[i].id == id) {
      s_timers[i].id = 0;
      found = true;
    }
  }
  update_next_due();
  unlock();
  return found;
}

/* ------------------------------------------------------------------ */
/* Alarm pools                                                          */
/* ------------------------------------------------------------------ */

alarm_pool_t *alarm_pool_get_default(void) { return &s_pool; }
alarm_pool_t *alarm_pool_create(uint hardware_alarm_num, uint max_timers) {
  (void)hardware_alarm_num;
  (void)max_timers;
  return &s_pool;
}
alarm_pool_t *alarm_pool_create_with_unused_hardware_alarm(uint max_timers) {
  (void)max_timers;
  return &s_pool;
}
void alarm_pool_destroy(alarm_pool_t *pool) { (void)pool; }

/* ------------------------------------------------------------------ */
/* Alarms                                                               */
/* ------------------------------------------------------------------ */

alarm_id_t alarm_pool_add_alarm_at(alarm_pool_t *pool, absolute_time_t time,
                                   alarm_callback_t callback, void *user_data,
                                   bool fire_if_past) {
  (void)pool;
  if (time <= time_us_64()) {
    if (!fire_if_past) return 0;
    /* The SDK calls it now; what it returns may reschedule it. */
    const int64_t again = callback(0, user_data);
    if (again == 0) return 0;
    time = again > 0 ? time_us_64() + (uint64_t)again : time - (uint64_t)again;
  }
  return add(time, callback, NULL, user_data);
}

alarm_id_t alarm_pool_add_alarm_in_us(alarm_pool_t *pool, uint64_t us,
                                      alarm_callback_t callback, void *user_data,
                                      bool fire_if_past) {
  return alarm_pool_add_alarm_at(pool, time_us_64() + us, callback, user_data,
                                 fire_if_past);
}

bool alarm_pool_cancel_alarm(alarm_pool_t *pool, alarm_id_t alarm_id) {
  (void)pool;
  return cancel(alarm_id);
}

/* ------------------------------------------------------------------ */
/* Repeating timers                                                     */
/* ------------------------------------------------------------------ */

bool alarm_pool_add_repeating_timer_us(alarm_pool_t *pool, int64_t delay_us,
                                       repeating_timer_callback_t callback,
                                       void *user_data, repeating_timer_t *out) {
  if (!delay_us) delay_us = 1;
  out->pool = pool;
  out->callback = callback;
  out->delay_us = delay_us;
  out->user_data = user_data;
  const uint64_t period = (uint64_t)(delay_us < 0 ? -delay_us : delay_us);
  out->alarm_id = add(time_us_64() + period, NULL, out, user_data);
  return out->alarm_id > 0;
}

bool cancel_repeating_timer(repeating_timer_t *timer) {
  const alarm_id_t id = timer->alarm_id;
  timer->alarm_id = 0;
  return id > 0 && cancel(id);
}

/* ------------------------------------------------------------------ */
/* Firing them                                                          */
/* ------------------------------------------------------------------ */

/* Everything due by now_us, each at most once per call: an interrupt
 * that is late is late once, not a burst of catch-ups. Emulated time
 * only moves on between calls, so a repeating timer whose callback is
 * slow is not a problem here as it would be on the RP2040. */
void mdfw_runtime_run_timers(uint64_t now_us) {
  if (now_us < __atomic_load_n(&s_next_due, __ATOMIC_ACQUIRE)) return;
  lock();
  for (int i = 0; i < MAX_TIMERS; i++) {
    timer_slot_t t = s_timers[i];
    if (!t.id || t.due_us > now_us) continue;
    if (t.repeating) {
      repeating_timer_t *rt = t.repeating;
      const uint64_t period = (uint64_t)(rt->delay_us < 0 ? -rt->delay_us : rt->delay_us);
      const bool again = rt->callback(rt);
      if (s_timers[i].id != t.id) continue; /* cancelled by the callback */
      if (!again) {
        s_timers[i].id = 0;
        rt->alarm_id = 0;
        continue;
      }
      /* Negative: from when it was due; positive: from when it ran. */
      uint64_t next = (rt->delay_us < 0 ? t.due_us : now_us) + period;
      if (next <= now_us) next = now_us + period;
      s_timers[i].due_us = next;
    } else {
      const int64_t again = t.alarm(t.id, t.user_data);
      if (s_timers[i].id != t.id) continue;
      if (again == 0) {
        s_timers[i].id = 0;
      } else {
        /* Negative: from when it was due; positive: from now. */
        uint64_t next = again < 0 ? t.due_us - (uint64_t)again : now_us + (uint64_t)again;
        if (next <= now_us) next = now_us + 1u;
        s_timers[i].due_us = next;
      }
    }
  }
  update_next_due();
  unlock();
}

void mdfw_runtime_timers_reset(void) {
  lock();
  memset(s_timers, 0, sizeof(s_timers));
  update_next_due();
  unlock();
}
