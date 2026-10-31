/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: runtime.c
 * Description: The plugin side of EmuMD: the emumd_plugin_v1 entry
 *              point (emumd_plugin.h) wrapped around a firmware's
 *              mdfw_app (mdfw.h). Holds the ROM4 window and the ROM3
 *              capture ring, and runs the firmware's main loop whenever
 *              the ST touches the cartridge or a frame passes -- or, for
 *              a firmware whose main loop never returns, on a thread of
 *              its own alongside the emulator.
 */

#include <pthread.h>
#include <stdarg.h>
#include <time.h>

#include "emumd_plugin.h"
#include "mdfw.h"
#include "pico.h"
#include "runtime.h"

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static const emumd_host_t *s_host;
static bool s_on;

/* ROM4: the start of the RAM the cartridge is served from (memory.c). */
#define s_rom4 mdfw_rom_in_ram

/* Written by the emulator's thread only, read by any: head is published
 * after the sample it covers, tail after the sample it frees. */
#define RING_SIZE 4096u
static uint16_t s_ring[RING_SIZE];
static uint32_t s_ring_head, s_ring_tail;
static uint32_t s_ring_dropped;
static void (*s_rom3_irq)(void);

static char s_options[4096];
static char s_sd_root[1024];

/* Written by the emulator's thread only, read by any. */
/* The clock runs on across a reboot (MDFW_KEEP, ram.c). */
static uint64_t s_host_us MDFW_KEEP;  /* what the emulator says the time is */
static uint64_t s_slept_us MDFW_KEEP; /* what sleeps on the emulator's thread added */
static mdfw_timer_hw_t s_timer_hw;
mdfw_timer_hw_t *timer_hw = &s_timer_hw;

/* A main loop that always has work must not hang the emulator. */
#define MAX_POLLS_PER_TURN 4096

/* The firmware's own threads (its main() and core 1) as opposed to the
 * emulator's, where init, poll and the ROM handlers run. */
static __thread bool s_own_thread;

void mdfw_runtime_enter_thread(void) { s_own_thread = true; }
bool mdfw_runtime_own_thread(void) { return s_own_thread; }

/* ------------------------------------------------------------------ */
/* Time                                                                 */
/* ------------------------------------------------------------------ */

/* Firmware threads sleeping until emulated time reaches s_wake_at (the
 * earliest of their deadlines; changed with s_time_lock held). A sleeper
 * sets it before looking at the time and the emulator moves the time on
 * before looking at it, so one of the two always sees the other. */
static pthread_mutex_t s_time_lock MDFW_KEEP = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_time_moved MDFW_KEEP = PTHREAD_COND_INITIALIZER;
static uint64_t s_wake_at MDFW_KEEP = UINT64_MAX;

uint64_t mdfw_time_us(void) {
  return __atomic_load_n(&s_host_us, __ATOMIC_SEQ_CST) +
         __atomic_load_n(&s_slept_us, __ATOMIC_SEQ_CST);
}

static void publish_time(void) {
  const uint64_t t = mdfw_time_us();
  s_timer_hw.timerawl = (uint32_t)t;
  s_timer_hw.timerawh = (uint32_t)(t >> 32);
  if (t >= __atomic_load_n(&s_wake_at, __ATOMIC_SEQ_CST)) {
    pthread_mutex_lock(&s_time_lock);
    __atomic_store_n(&s_wake_at, UINT64_MAX, __ATOMIC_SEQ_CST);
    pthread_cond_broadcast(&s_time_moved);
    pthread_mutex_unlock(&s_time_lock);
  }
}

static void set_host_time(uint64_t now_us) {
  if (now_us > s_host_us) __atomic_store_n(&s_host_us, now_us, __ATOMIC_SEQ_CST);
  publish_time();
}

static void unlock_time(void *arg) {
  (void)arg;
  pthread_mutex_unlock(&s_time_lock);
}

/* A firmware thread waits for the emulator to get there, as a core that
 * sleeps waits for the rest of the machine; the timed wait is a safety
 * net, and where a thread being stopped (power off) gets out. */
static void wait_until(uint64_t target) {
  pthread_mutex_lock(&s_time_lock);
  pthread_cleanup_push(unlock_time, NULL);
  for (;;) {
    if (target < __atomic_load_n(&s_wake_at, __ATOMIC_SEQ_CST)) {
      __atomic_store_n(&s_wake_at, target, __ATOMIC_SEQ_CST);
    }
    if (mdfw_time_us() >= target) break;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 10 * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&s_time_moved, &s_time_lock, &ts);
  }
  pthread_cleanup_pop(1);
}

/* On the emulator's thread nothing else can move time on while the
 * firmware runs, so a sleep moves it on itself. */
void mdfw_runtime_sleep_until(uint64_t target) {
  if (s_own_thread) {
    wait_until(target);
    return;
  }
  const uint64_t now = mdfw_time_us();
  if (target <= now) return;
  __atomic_store_n(&s_slept_us, s_slept_us + (target - now), __ATOMIC_SEQ_CST);
  publish_time();
}

void mdfw_runtime_sleep(uint64_t us) { mdfw_runtime_sleep_until(mdfw_time_us() + us); }

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

static pthread_mutex_t s_log_lock MDFW_KEEP = PTHREAD_MUTEX_INITIALIZER;
static char s_line[1024];
static size_t s_line_len;

/* Collect text into whole lines: DPRINTF output often arrives in pieces.
 * A firmware thread being stopped must not go while holding the lock. */
static void log_text(const char *text) {
  int cancel_state;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
  pthread_mutex_lock(&s_log_lock);
  for (const char *p = text; *p; p++) {
    if (*p == '\n' || s_line_len == sizeof(s_line) - 1) {
      s_line[s_line_len] = 0;
      if (s_host && s_host->log) {
        char out[1100];
        snprintf(out, sizeof(out), "%s: %s", mdfw_app.name ? mdfw_app.name : "MD",
                 s_line);
        s_host->log(out);
      } else {
        fprintf(stderr, "%s\n", s_line);
      }
      s_line_len = 0;
      if (*p == '\n') continue;
    }
    if (*p != '\r') s_line[s_line_len++] = *p;
  }
  pthread_mutex_unlock(&s_log_lock);
  pthread_setcancelstate(cancel_state, NULL);
}

static void log_v(const char *fmt, va_list ap) {
  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  log_text(buf);
}

void mdfw_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_v(fmt, ap);
  va_end(ap);
  /* mdfw_log is for whole messages; end the line if the caller did not. */
  if (fmt[0] && fmt[strlen(fmt) - 1] != '\n') log_text("\n");
}

bool mdfw_verbose(void) { return s_host && s_host->verbose; }

void mdfw_debug(const char *fmt, ...) {
  if (!mdfw_verbose()) return;
  va_list ap;
  va_start(ap, fmt);
  log_v(fmt, ap);
  va_end(ap);
}

void panic(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_text("panic: ");
  log_v(fmt, ap);
  log_text("\n");
  va_end(ap);
  abort();
}

/* ------------------------------------------------------------------ */
/* Options                                                              */
/* ------------------------------------------------------------------ */

const char *mdfw_option(const char *key) {
  static char value[1024];
  const size_t klen = strlen(key);
  const char *p = s_options;
  /* The last one given wins; a bare "key" means "1". */
  const char *found = NULL;
  size_t found_len = 0;
  while (*p) {
    const char *eol = strchr(p, '\n');
    const size_t len = eol ? (size_t)(eol - p) : strlen(p);
    if (len > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
      found = p + klen + 1;
      found_len = len - klen - 1;
    } else if (len == klen && strncmp(p, key, klen) == 0) {
      found = "1";
      found_len = 1;
    }
    p += len + (eol ? 1 : 0);
  }
  if (!found) return NULL;
  if (found_len >= sizeof(value)) found_len = sizeof(value) - 1;
  memcpy(value, found, found_len);
  value[found_len] = 0;
  return value;
}

int mdfw_option_int(const char *key, int def) {
  const char *v = mdfw_option(key);
  if (!v || !*v) return def;
  char *end = NULL;
  const long n = strtol(v, &end, 0);
  return (end && *end == 0) ? (int)n : def;
}

const char *mdfw_sd_root(void) { return s_sd_root; }

/* ------------------------------------------------------------------ */
/* The cartridge port                                                   */
/* ------------------------------------------------------------------ */

uint16_t *mdfw_rom4(void) { return s_rom4; }

void mdfw_rom4_load(const uint16_t *words, size_t count) {
  if (count > MDFW_ROM4_WORDS) count = MDFW_ROM4_WORDS;
  memcpy(s_rom4, words, count * sizeof(uint16_t));
}

void mdfw_rom3_set_irq(void (*handler)(void)) { s_rom3_irq = handler; }

bool mdfw_rom3_pop(uint16_t *sample) {
  const uint32_t tail = s_ring_tail;
  if (tail == __atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE)) return false;
  *sample = s_ring[tail % RING_SIZE];
  __atomic_store_n(&s_ring_tail, tail + 1u, __ATOMIC_RELEASE);
  return true;
}

bool mdfw_rom3_peek(uint32_t *cursor, uint16_t *sample) {
  for (;;) {
    const uint32_t head = __atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE);
    if (*cursor == head) return false;
    if (head - *cursor > RING_SIZE) *cursor = head - RING_SIZE;
    const uint16_t v = s_ring[*cursor % RING_SIZE];
    /* Still there, or already written over by a newer sample? */
    if (__atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE) - *cursor <= RING_SIZE) {
      *sample = v;
      (*cursor)++;
      return true;
    }
  }
}

uint32_t mdfw_rom3_dropped(void) { return s_ring_dropped; }

/* ------------------------------------------------------------------ */
/* The main loop                                                        */
/* ------------------------------------------------------------------ */

static bool s_in_loop;
static bool s_reboot_pending;
static void reboot_now(void);

/* Replaced by runtime/wifi in a firmware built with Wi-Fi. */
__attribute__((weak)) void mdfw_runtime_async_turn(void) {}
__attribute__((weak)) void mdfw_runtime_wifi_pace(uint64_t now_us) { (void)now_us; }
__attribute__((weak)) void mdfw_runtime_wifi_power_off(void) {}
/* And by runtime/bluepad32 in one built with Bluetooth. */
__attribute__((weak)) void mdfw_runtime_bt_poll(void) {}
__attribute__((weak)) void mdfw_runtime_bt_power_off(void) {}

static void run_main_loop(void) {
  if (!s_on || s_in_loop) return;
  s_in_loop = true;
  mdfw_runtime_run_timers(mdfw_time_us());
  mdfw_runtime_async_turn();
  if (mdfw_app.poll) {
    for (int i = 0;
         i < MAX_POLLS_PER_TURN && !s_reboot_pending && mdfw_app.poll(); i++) {
      if (mdfw_app.after_poll) mdfw_app.after_poll();
    }
  }
  s_in_loop = false;
  if (__atomic_load_n(&s_reboot_pending, __ATOMIC_ACQUIRE)) reboot_now();
}

/* A firmware whose main loop never returns runs it here, as core 0,
 * alongside the emulator. */
static pthread_t s_main_thread;
static bool s_main_running;

static void *main_thread(void *arg) {
  (void)arg;
  mdfw_runtime_enter_thread();
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
  mdfw_app.main();
  mdfw_log("the firmware's main() returned");
  return NULL;
}

static void start_main(void) {
  s_main_running = pthread_create(&s_main_thread, NULL, main_thread, NULL) == 0;
  if (!s_main_running) panic("cannot start the firmware's main thread");
}

/* Stops it at its next wait (a sleep, FIFO, WFE, tight loop or empty
 * ROM3 ring). */
static void stop_main(void) {
  if (!s_main_running) return;
  pthread_cancel(s_main_thread);
  __sev();
  pthread_join(s_main_thread, NULL);
  s_main_running = false;
}

/* ------------------------------------------------------------------ */
/* The plugin interface                                                 */
/* ------------------------------------------------------------------ */

/* Cold: the emulator powers the device on; otherwise the firmware asked
 * for a reboot. */
static int power_on(const emumd_host_t *host, uint64_t now_us, bool cold) {
  if (!host || host->abi != EMUMD_PLUGIN_ABI) return -1;
  mdfw_runtime_ram_power_on(); /* first, before anything is set */
  s_host = host;
  snprintf(s_options, sizeof(s_options), "%s", host->options ? host->options : "");
  snprintf(s_sd_root, sizeof(s_sd_root), "%s", host->sd_dir ? host->sd_dir : ".");
  /* No trailing slash: firmware paths start with one. */
  size_t n = strlen(s_sd_root);
  while (n > 1 && s_sd_root[n - 1] == '/') s_sd_root[--n] = 0;

  set_host_time(now_us);
  memset(mdfw_rom_in_ram, 0, sizeof(mdfw_rom_in_ram));
  s_ring_head = s_ring_tail = s_ring_dropped = 0;
  s_rom3_irq = NULL;
  mdfw_runtime_watchdog_power_on(cold);
  mdfw_runtime_flash_power_on();
  mdfw_runtime_fatfs_reset();

  s_on = true;
  const int rc = mdfw_app.init ? mdfw_app.init() : 0;
  if (rc != 0) {
    mdfw_log("the firmware did not start (%d)", rc);
    s_on = false;
    return rc;
  }
  if (mdfw_app.main) start_main();
  run_main_loop();
  return 0;
}

static int plugin_power_on(const emumd_host_t *host, uint64_t now_us) {
  return power_on(host, now_us, true);
}

static void plugin_power_off(void) {
  if (!s_on) return;
  if (mdfw_app.shutdown) mdfw_app.shutdown();
  s_on = false;
  stop_main();
  mdfw_runtime_multicore_stop();
  mdfw_runtime_bt_power_off();
  mdfw_runtime_wifi_power_off();
  mdfw_runtime_timers_reset();
  mdfw_runtime_fatfs_reset();
  mdfw_runtime_flash_power_off();
}

static uint16_t plugin_rom4_read(uint32_t offset, uint64_t now_us) {
  static unsigned reads;
  set_host_time(now_us);
  /* The real main loop never stops; here it also gets a turn while the
   * ST polls ROM4 (waiting for the firmware, say). Timers are due far
   * more often than that. */
  if ((++reads & 255u) == 0) {
    run_main_loop();
  } else if (s_on && !s_in_loop) {
    mdfw_runtime_run_timers(mdfw_time_us());
  }
  return s_rom4[(offset & 0xFFFFu) >> 1];
}

static void plugin_rom3_read(uint32_t offset, uint64_t now_us) {
  set_host_time(now_us);
  const uint32_t head = s_ring_head;
  if (head - __atomic_load_n(&s_ring_tail, __ATOMIC_ACQUIRE) >= RING_SIZE) {
    s_ring_dropped++;
  } else {
    s_ring[head % RING_SIZE] = (uint16_t)offset;
    __atomic_store_n(&s_ring_head, head + 1u, __ATOMIC_RELEASE);
  }
  if (s_on && s_rom3_irq) s_rom3_irq();
  run_main_loop();
}

static void plugin_tick(uint64_t now_us) {
  set_host_time(now_us);
  run_main_loop();
  if (s_on) mdfw_runtime_wifi_pace(now_us);
}

/* watchdog_reboot() and the like: power-cycle on the emulator's thread,
 * once the current turn of the main loop is over. A firmware thread that
 * asks is stopped then, at its next wait. */
void mdfw_runtime_reboot(void) {
  mdfw_log("the firmware asked for a reboot");
  __atomic_store_n(&s_reboot_pending, true, __ATOMIC_RELEASE);
}

static void reboot_now(void) {
  const emumd_host_t *host = s_host;
  s_reboot_pending = false;
  plugin_power_off();
  power_on(host, s_host_us, false);
}

__attribute__((visibility("default"))) const emumd_plugin_t *emumd_plugin_v1(
    void) {
  static emumd_plugin_t plugin = {
      .abi = EMUMD_PLUGIN_ABI,
      .power_on = plugin_power_on,
      .power_off = plugin_power_off,
      .rom4_read = plugin_rom4_read,
      .rom3_read = plugin_rom3_read,
      .tick = plugin_tick,
  };
  plugin.name = mdfw_app.name ? mdfw_app.name : "unnamed firmware";
  plugin.version = mdfw_app.version ? mdfw_app.version : "";
  return &plugin;
}
