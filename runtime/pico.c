/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: pico.c
 * Description: Host stand-ins for the Pico SDK calls declared in shim/:
 *              time and sleeping (emulated time), clocks, reboots, the
 *              board ID and random numbers.
 */

#include <sys/random.h>

#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico.h"
#include "pico/rand.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "runtime.h"

uint32_t time_us_32(void) { return (uint32_t)mdfw_time_us(); }
uint64_t time_us_64(void) { return mdfw_time_us(); }

/* Sleeping on the emulator's thread moves emulated time on, so a firmware
 * that waits for a moment gets there without the emulator stalling; on the
 * firmware's own threads it waits for the emulator (runtime.c). */
void sleep_us(uint64_t us) { mdfw_runtime_sleep(us); }
void sleep_ms(uint32_t ms) { mdfw_runtime_sleep((uint64_t)ms * 1000u); }
void sleep_until(absolute_time_t t) {
  if (t != at_the_end_of_time) mdfw_runtime_sleep_until(t);
}
void busy_wait_us(uint64_t us) { sleep_us(us); }
void busy_wait_us_32(uint32_t us) { sleep_us(us); }
void busy_wait_ms(uint32_t ms) { sleep_ms(ms); }
void busy_wait_until(absolute_time_t t) { sleep_until(t); }

/* What each GPIO reads (hardware/gpio.h): high, unless EmuMD says
 * otherwise (a SidecarTridge template's SELECT button reads low). */
uint32_t mdfw_gpio_inputs = 0xffffffffu;

static uint32_t s_sys_hz = 125000000u;

bool set_sys_clock_khz(uint32_t freq_khz, bool required) {
  (void)required;
  s_sys_hz = freq_khz * 1000u;
  return true;
}

uint32_t clock_get_hz(enum clock_index clk_index) {
  switch (clk_index) {
    case clk_sys:
      return s_sys_hz;
    case clk_peri:
      return s_sys_hz;
    case clk_usb:
    case clk_adc:
      return 48000000u;
    case clk_ref:
      return 12000000u;
    case clk_rtc:
      return 46875u;
    default:
      return 0;
  }
}

/* Its scratch registers survive a reboot (MDFW_KEEP), not a power cycle. */
static watchdog_hw_t s_watchdog MDFW_KEEP;
watchdog_hw_t *watchdog_hw = &s_watchdog;
static bool s_watchdog_rebooted MDFW_KEEP;

void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) {
  (void)pc;
  (void)sp;
  (void)delay_ms;
  s_watchdog_rebooted = true;
  mdfw_runtime_reboot();
}

bool watchdog_caused_reboot(void) { return s_watchdog_rebooted; }

/* A power cycle, rather than the reboot that follows watchdog_reboot. */
void mdfw_runtime_watchdog_power_on(bool cold) {
  if (!cold) return;
  memset(&s_watchdog, 0, sizeof(s_watchdog));
  s_watchdog_rebooted = false;
}

void pico_get_unique_board_id(pico_unique_board_id_t *id_out) {
  static const uint8_t def[PICO_UNIQUE_BOARD_ID_SIZE_BYTES] = {
      0xE6, 0x61, 0x41, 0x03, 0xE7, 0x4D, 0x44, 0x01};
  memcpy(id_out->id, def, sizeof(def));
  const char *hex = mdfw_option("board_id");
  if (!hex) return;
  for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES && hex[2 * i] && hex[2 * i + 1];
       i++) {
    const char pair[3] = {hex[2 * i], hex[2 * i + 1], 0};
    id_out->id[i] = (uint8_t)strtoul(pair, NULL, 16);
  }
}

void pico_get_unique_board_id_string(char *id_out, uint len) {
  pico_unique_board_id_t id;
  pico_get_unique_board_id(&id);
  char hex[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
  for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
    snprintf(hex + 2 * i, 3, "%02X", id.id[i]);
  }
  if (len) snprintf(id_out, len, "%s", hex);
}

static void random_bytes(void *buf, size_t len) {
  if (getentropy(buf, len) != 0) panic("getentropy failed");
}

uint32_t get_rand_32(void) {
  uint32_t v;
  random_bytes(&v, sizeof(v));
  return v;
}

uint64_t get_rand_64(void) {
  uint64_t v;
  random_bytes(&v, sizeof(v));
  return v;
}

void get_rand_128(rng_128_t *rand128) { random_bytes(rand128->r, sizeof(rand128->r)); }
