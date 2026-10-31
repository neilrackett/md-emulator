/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: reboot.c
 * Description: The firmware reboot_test loads. At each power-on it puts
 *              what it finds of its last run in ROM4: its RAM (a counter
 *              and an initialised variable it changes), watchdog scratch
 *              0 and a count kept in flash. A ROM3 read of REBOOT_ASK
 *              makes it reboot through the watchdog.
 */
#include <string.h>

#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "mdfw.h"

/* ROM4 words, as reboot_test reads them. */
#define REBOOT_RUNS 0    /* power-ons this RAM has seen */
#define REBOOT_DATA 1    /* an initialised variable, as found */
#define REBOOT_SCRATCH 2 /* watchdog scratch 0, counting power-ons */
#define REBOOT_FLASH 3   /* a count of power-ons kept in flash */
#define REBOOT_CAUSED 4  /* watchdog_caused_reboot() */
#define REBOOT_ASK 0x1234

#define FLASH_AT 0x100000u

static unsigned s_runs;
static unsigned s_data = 7;
static bool s_reboot;

static void rom3_irq(void) {
  uint16_t sample;
  while (mdfw_rom3_pop(&sample)) {
    if (sample == REBOOT_ASK) s_reboot = true;
  }
}

static int app_init(void) {
  uint16_t *rom4 = mdfw_rom4();
  rom4[REBOOT_RUNS] = (uint16_t)++s_runs;
  rom4[REBOOT_DATA] = (uint16_t)s_data;
  s_data = 99;
  rom4[REBOOT_SCRATCH] = (uint16_t)++watchdog_hw->scratch[0];

  uint8_t page[FLASH_PAGE_SIZE];
  memcpy(page, mdfw_flash + FLASH_AT, sizeof(page));
  page[0] = page[0] == 0xFF ? 1 : (uint8_t)(page[0] + 1);
  flash_range_erase(FLASH_AT, FLASH_SECTOR_SIZE);
  flash_range_program(FLASH_AT, page, sizeof(page));
  rom4[REBOOT_FLASH] = page[0];

  rom4[REBOOT_CAUSED] = watchdog_caused_reboot();
  mdfw_rom3_set_irq(rom3_irq);
  return 0;
}

static bool app_poll(void) {
  if (!s_reboot) return false;
  s_reboot = false;
  watchdog_reboot(0, 0, 0);
  return false;
}

const mdfw_app_t mdfw_app = {
    .name = MDFW_NAME,
    .version = MDFW_VERSION,
    .init = app_init,
    .poll = app_poll,
};
