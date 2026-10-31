/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: ram.c
 * Description: The firmware's RAM across a reboot. An RP2040 starts each
 *              boot from cleared RAM, but here a reboot or a power cycle
 *              runs the same loaded .mdfw again, whose variables still
 *              hold what the last run left in them. So the first
 *              power-on saves the .mdfw's writable data, and every one
 *              after puts it back: all of it but flash, which the real
 *              part keeps too, and what is marked MDFW_KEEP (the
 *              watchdog's scratch registers, the runtime's clock and its
 *              locks).
 *
 *              Only in a .mdfw, loaded as Hatari loads it. A test built
 *              as one program has its own variables in the same image,
 *              and they would be put back with the firmware's.
 */
#ifndef __APPLE__
#define _GNU_SOURCE /* dl_iterate_phdr */
#endif

#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#endif

#include "mdfw.h"
#include "runtime.h"

#define MAX_RANGES 16

typedef struct {
  uint8_t *at;
  size_t len;
  uint8_t *saved;
} range_t;

/* This file's own state is the one thing that must not go back. */
static range_t s_ranges[MAX_RANGES] MDFW_KEEP;
static int s_count MDFW_KEEP;
static bool s_saved MDFW_KEEP;

/* What is never put back: flash, and (on Linux, where it sits among the
 * rest) the MDFW_KEEP section. */
typedef struct {
  uint8_t *at, *end;
} span_t;

#define MAX_SKIPS 3
static span_t s_skip[MAX_SKIPS];
static int s_nskip;

static void skip(uint8_t *at, uint8_t *end) {
  if (at && end > at && s_nskip < MAX_SKIPS) s_skip[s_nskip++] = (span_t){at, end};
}

/* Adds [at, end) less whatever overlaps it in s_skip. */
static void add(uint8_t *at, uint8_t *end) {
  if (end <= at) return;
  for (int i = 0; i < s_nskip; i++) {
    if (s_skip[i].end <= at || s_skip[i].at >= end) continue;
    add(at, s_skip[i].at);
    add(s_skip[i].end, end);
    return;
  }
  if (s_count == MAX_RANGES) {
    mdfw_log("ram: too many pieces; a reboot keeps some variables");
    return;
  }
  s_ranges[s_count++] = (range_t){at, (size_t)(end - at), NULL};
}

#ifdef __APPLE__

/* The .mdfw's __data, __bss and __common, but not its thread-locals
 * (__thread_*), which belong to each thread rather than to the image, nor
 * __mdfw_keep, a section of its own. */
static void find_ranges(void) {
  Dl_info info;
  if (!dladdr((const void *)&mdfw_runtime_ram_power_on, &info)) return;
  const struct mach_header_64 *mh = info.dli_fbase;
  if (mh->filetype == MH_EXECUTE) return;
  static const char *const segs[] = {"__DATA", "__DATA_DIRTY"};
  static const char *const sects[] = {"__data", "__bss", "__common"};
  for (size_t g = 0; g < 2; g++) {
    for (size_t s = 0; s < 3; s++) {
      unsigned long len;
      uint8_t *at = getsectiondata(mh, segs[g], sects[s], &len);
      if (at) add(at, at + len);
    }
  }
}

#else

extern char __start_mdfw_keep[] __attribute__((weak, visibility("hidden")));
extern char __stop_mdfw_keep[] __attribute__((weak, visibility("hidden")));

typedef struct {
  uintptr_t self;
  bool found, exe;
  struct dl_phdr_info info;
} find_t;

static int find_image(struct dl_phdr_info *info, size_t size, void *arg) {
  (void)size;
  find_t *f = arg;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *p = &info->dlpi_phdr[i];
    const uintptr_t at = info->dlpi_addr + p->p_vaddr;
    if (p->p_type == PT_LOAD && f->self >= at && f->self < at + p->p_memsz) {
      f->found = true;
      f->exe = !info->dlpi_name || !info->dlpi_name[0]; /* the program itself */
      f->info = *info;
      return 1;
    }
  }
  return 0;
}

/* The .mdfw's writable segments, less RELRO: the GOT and the like, which
 * the loader has made read-only by now. */
static void find_ranges(void) {
  find_t f = {.self = (uintptr_t)&s_saved};
  dl_iterate_phdr(find_image, &f);
  if (!f.found || f.exe) return;
  skip((uint8_t *)__start_mdfw_keep, (uint8_t *)__stop_mdfw_keep);
  for (int i = 0; i < f.info.dlpi_phnum; i++) {
    const ElfW(Phdr) *p = &f.info.dlpi_phdr[i];
    if (p->p_type == PT_GNU_RELRO) {
      uint8_t *at = (uint8_t *)(f.info.dlpi_addr + p->p_vaddr);
      skip(at, at + p->p_memsz);
    }
  }
  for (int i = 0; i < f.info.dlpi_phnum; i++) {
    const ElfW(Phdr) *p = &f.info.dlpi_phdr[i];
    if (p->p_type == PT_LOAD && (p->p_flags & PF_W)) {
      uint8_t *at = (uint8_t *)(f.info.dlpi_addr + p->p_vaddr);
      add(at, at + p->p_memsz);
    }
  }
}

#endif

void mdfw_runtime_ram_power_on(void) {
  if (s_saved) {
    for (int i = 0; i < s_count; i++) {
      memcpy(s_ranges[i].at, s_ranges[i].saved, s_ranges[i].len);
    }
    return;
  }
  /* Before the emulator's log is set up: anything said here goes to
   * stderr. */
  s_saved = true;
  skip(mdfw_flash, mdfw_flash + MDFW_FLASH_BYTES);
  find_ranges();
  for (int i = 0; i < s_count; i++) {
    s_ranges[i].saved = malloc(s_ranges[i].len);
    if (!s_ranges[i].saved) {
      mdfw_log("ram: out of memory; a reboot keeps some variables");
      s_count = i;
      break;
    }
    memcpy(s_ranges[i].saved, s_ranges[i].at, s_ranges[i].len);
  }
}
