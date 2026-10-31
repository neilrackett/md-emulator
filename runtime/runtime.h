/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: runtime.h
 * Description: Internal calls between the EmuMD runtime's parts.
 */
#ifndef MDFW_RUNTIME_H
#define MDFW_RUNTIME_H
#include <stdint.h>

#include <stdbool.h>

/* memory.c: ROM4 (the first MDFW_ROM4_WORDS), then ROM3's 64 KB. */
extern uint16_t mdfw_rom_in_ram[2 * 0x8000];

void mdfw_runtime_sleep(uint64_t us);
void mdfw_runtime_sleep_until(uint64_t target_us);
void mdfw_runtime_reboot(void);

/* The firmware's own threads (main and core 1) mark themselves. */
void mdfw_runtime_enter_thread(void);
bool mdfw_runtime_own_thread(void);

/* Pico SDK timers and alarms, fired on the emulator's thread. */
void mdfw_runtime_run_timers(uint64_t now_us);
void mdfw_runtime_timers_reset(void);

/* ram.c: the .mdfw's variables as they were before the first power-on,
 * put back at every power-on after it, as an RP2040 boots from cleared
 * RAM. A variable marked MDFW_KEEP (after its name) is left as it is:
 * what survives on the real part, and the runtime's locks, clock and
 * whatever else has to outlive a reboot of the firmware. */
#ifdef __APPLE__
#define MDFW_KEEP __attribute__((section("__DATA,__mdfw_keep")))
#else
#define MDFW_KEEP __attribute__((section("mdfw_keep")))
#endif
void mdfw_runtime_ram_power_on(void);

void mdfw_runtime_watchdog_power_on(bool cold);
void mdfw_runtime_flash_power_on(void);
void mdfw_runtime_flash_power_off(void);
void mdfw_runtime_fatfs_reset(void);
void mdfw_runtime_multicore_stop(void);

/* Wi-Fi (runtime/wifi, in a firmware built with it; no-ops otherwise):
 * background work on the emulator's thread, holding a fast-forwarding
 * emulator back to the network's pace (once a frame), and switching the
 * chip off. */
void mdfw_runtime_async_turn(void);
void mdfw_runtime_wifi_pace(uint64_t now_us);
void mdfw_runtime_wifi_power_off(void);

/* Bluetooth (runtime/bluepad32, in a firmware built with it; no-ops
 * otherwise): the scripted pads' turn, from cyw43_arch_poll(), and
 * switching the controller off. */
void mdfw_runtime_bt_poll(void);
void mdfw_runtime_bt_power_off(void);

#include <stddef.h>
void *mdfw_hostdir_open(const char *path);
void mdfw_hostdir_close(void *d);
void mdfw_hostdir_rewind(void *d);
const char *mdfw_hostdir_next(void *d);
bool mdfw_hostdir_find(const char *dir, const char *name, char *found,
                       size_t foundlen);

#endif
