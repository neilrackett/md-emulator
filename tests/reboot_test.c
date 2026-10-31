/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: reboot_test.c
 * Description: Checks that a reboot and a power cycle start the firmware
 *              from fresh RAM, as on an RP2040, while flash survives both
 *              and watchdog scratch survives the reboot only. Loads
 *              tests/reboot as Hatari loads a .mdfw; VERBOSE=1 shows the
 *              log.
 *
 *   reboot_test tests/reboot/build/reboot.mdfw
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "emumd_plugin.h"

static int s_fails;
#define CHECK(c)                                                   \
  do {                                                             \
    if (!(c)) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      s_fails++;                                                   \
    }                                                              \
  } while (0)

/* tests/reboot/reboot.c */
#define REBOOT_RUNS 0
#define REBOOT_DATA 1
#define REBOOT_SCRATCH 2
#define REBOOT_FLASH 3
#define REBOOT_CAUSED 4
#define REBOOT_ASK 0x1234

static const emumd_plugin_t *s_plugin;
static uint64_t s_now = 1000;
static int s_verbose;

static void host_log(const char *line) {
  if (s_verbose) fprintf(stderr, "%s\n", line);
}

static unsigned word(unsigned n) { return s_plugin->rom4_read(n * 2u, s_now); }

/* What the firmware found at its last power-on. */
static void expect(unsigned runs, unsigned data, unsigned scratch, unsigned flash,
                   unsigned caused) {
  CHECK(word(REBOOT_RUNS) == runs);
  CHECK(word(REBOOT_DATA) == data);
  CHECK(word(REBOOT_SCRATCH) == scratch);
  CHECK(word(REBOOT_FLASH) == flash);
  CHECK(word(REBOOT_CAUSED) == caused);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: reboot_test REBOOT.mdfw\n");
    return 2;
  }
  s_verbose = getenv("VERBOSE") != NULL;
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    fprintf(stderr, "%s\n", dlerror());
    return 1;
  }
  const emumd_plugin_entry_t entry = (emumd_plugin_entry_t)dlsym(lib, EMUMD_PLUGIN_ENTRY);
  s_plugin = entry ? entry() : NULL;
  if (!s_plugin || s_plugin->abi != EMUMD_PLUGIN_ABI) {
    fprintf(stderr, "%s: not an EmuMD plugin\n", argv[1]);
    return 1;
  }
  emumd_host_t host = {EMUMD_PLUGIN_ABI, "/tmp", "", s_verbose, host_log};

  CHECK(s_plugin->power_on(&host, s_now) == 0);
  expect(1, 7, 1, 1, 0);

  /* A reboot: fresh RAM; scratch and flash carry on. */
  s_now += 1000;
  s_plugin->rom3_read(REBOOT_ASK, s_now);
  expect(1, 7, 2, 2, 1);
  s_now += 1000;
  s_plugin->rom3_read(REBOOT_ASK, s_now);
  expect(1, 7, 3, 3, 1);

  /* A power cycle: fresh RAM and scratch; flash carries on. */
  s_plugin->power_off();
  s_now += 1000;
  CHECK(s_plugin->power_on(&host, s_now) == 0);
  expect(1, 7, 1, 4, 0);
  s_plugin->power_off();

  if (s_fails) return 1;
  printf("reboot_test: OK\n");
  return 0;
}
