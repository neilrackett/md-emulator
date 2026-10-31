/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: flash.c
 * Description: The RP2040's 2 MB of flash as a host array, with the Pico
 *              SDK's erase/program calls and their real rules: erase sets
 *              whole 4 KB sectors to 0xFF, programming whole 256-byte
 *              pages can only clear bits. Anything else stops the
 *              emulator, as it would misbehave on the real part.
 */

#include "hardware/flash.h"

#include "pico.h"
#include "runtime.h"

/* mdfw_flash itself is in memory.c, with the template's regions marked. */
static bool s_flash_ready MDFW_KEEP;

static const char *flash_file(void) { return mdfw_option("flash"); }

void mdfw_runtime_flash_power_on(void) {
  if (s_flash_ready) return; /* flash keeps its contents across power-offs */
  memset(mdfw_flash, 0xFF, sizeof(mdfw_flash));
  const char *path = flash_file();
  if (path && *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
      const size_t n = fread(mdfw_flash, 1, sizeof(mdfw_flash), f);
      fclose(f);
      mdfw_debug("flash: %zu bytes from %s\n", n, path);
    }
  }
  s_flash_ready = true;
}

void mdfw_runtime_flash_power_off(void) {
  const char *path = flash_file();
  if (!path || !*path) return;
  FILE *f = fopen(path, "wb");
  if (!f) {
    mdfw_log("flash: cannot write %s", path);
    return;
  }
  fwrite(mdfw_flash, 1, sizeof(mdfw_flash), f);
  fclose(f);
}

void flash_range_erase(uint32_t flash_offs, size_t count) {
  if ((flash_offs | count) & (FLASH_SECTOR_SIZE - 1u)) {
    panic("flash_range_erase(0x%x, 0x%zx): not whole 4 KB sectors", flash_offs,
          count);
  }
  if (flash_offs + count > MDFW_FLASH_BYTES) {
    panic("flash_range_erase(0x%x, 0x%zx): past the end of flash", flash_offs,
          count);
  }
  memset(mdfw_flash + flash_offs, 0xFF, count);
}

void flash_range_program(uint32_t flash_offs, const uint8_t *data,
                         size_t count) {
  if ((flash_offs | count) & (FLASH_PAGE_SIZE - 1u)) {
    panic("flash_range_program(0x%x, 0x%zx): not whole 256-byte pages",
          flash_offs, count);
  }
  if (flash_offs + count > MDFW_FLASH_BYTES) {
    panic("flash_range_program(0x%x, 0x%zx): past the end of flash",
          flash_offs, count);
  }
  for (size_t i = 0; i < count; i++) mdfw_flash[flash_offs + i] &= data[i];
}

void flash_get_unique_id(uint8_t *id_out) {
  static const uint8_t id[FLASH_UNIQUE_ID_SIZE_BYTES] = {'m', 'd', 'e', 'm',
                                                         'u', 0, 0, 1};
  memcpy(id_out, id, sizeof(id));
}
