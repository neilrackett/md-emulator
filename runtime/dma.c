/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * File: dma.c
 * Description: The RP2040's DMA, as far as a host can have it
 *              (hardware/dma.h): channels to claim, and memory-to-memory
 *              transfers, done at once when started.
 */

#include "hardware/dma.h"

#include <pthread.h>

#include "runtime.h"

static dma_hw_t s_dma_hw;
dma_hw_t *dma_hw = &s_dma_hw;

static struct {
  dma_channel_config config;
  volatile void *write;
  const volatile void *read;
  uint32_t count;
} s_ch[NUM_DMA_CHANNELS];

static uint32_t s_claimed;
static pthread_mutex_t s_claim_lock MDFW_KEEP = PTHREAD_MUTEX_INITIALIZER;

int dma_claim_unused_channel(bool required) {
  if (!required) return -1; /* the caller has another way: take it */
  int found = -1;
  pthread_mutex_lock(&s_claim_lock);
  for (uint i = 0; i < NUM_DMA_CHANNELS && found < 0; i++) {
    if (!(s_claimed & (1u << i))) {
      s_claimed |= 1u << i;
      found = (int)i;
    }
  }
  pthread_mutex_unlock(&s_claim_lock);
  if (found < 0) panic("no DMA channel free");
  return found;
}

void dma_channel_claim(uint channel) {
  pthread_mutex_lock(&s_claim_lock);
  s_claimed |= 1u << channel;
  pthread_mutex_unlock(&s_claim_lock);
}

void dma_channel_unclaim(uint channel) {
  pthread_mutex_lock(&s_claim_lock);
  s_claimed &= ~(1u << channel);
  pthread_mutex_unlock(&s_claim_lock);
}

bool dma_channel_is_claimed(uint channel) { return (s_claimed >> channel) & 1u; }

/* A transfer paced by a peripheral (the PIO's, say) does not happen. */
static void run(uint channel) {
  if (channel >= NUM_DMA_CHANNELS) return;
  const dma_channel_config *c = &s_ch[channel].config;
  if (!c->enable || c->dreq != DREQ_FORCE) return;
  const size_t size = 1u << c->size;
  const uint8_t *r = (const uint8_t *)s_ch[channel].read;
  uint8_t *w = (uint8_t *)s_ch[channel].write;
  for (uint32_t n = 0; n < s_ch[channel].count; n++) {
    uint8_t v[4];
    memcpy(v, r, size);
    if (c->bswap) {
      for (size_t i = 0; i < size / 2; i++) {
        const uint8_t t = v[i];
        v[i] = v[size - 1 - i];
        v[size - 1 - i] = t;
      }
    }
    memcpy(w, v, size);
    if (c->read_increment) r += size;
    if (c->write_increment) w += size;
  }
  if (c->chain_to != channel) run(c->chain_to);
}

void dma_channel_set_config(uint channel, const dma_channel_config *config, bool trigger) {
  s_ch[channel].config = *config;
  if (trigger) run(channel);
}

void dma_channel_set_read_addr(uint channel, const volatile void *read_addr, bool trigger) {
  s_ch[channel].read = read_addr;
  if (trigger) run(channel);
}

void dma_channel_set_write_addr(uint channel, volatile void *write_addr, bool trigger) {
  s_ch[channel].write = write_addr;
  if (trigger) run(channel);
}

void dma_channel_set_trans_count(uint channel, uint32_t count, bool trigger) {
  s_ch[channel].count = count;
  if (trigger) run(channel);
}

void dma_channel_configure(uint channel, const dma_channel_config *config,
                           volatile void *write_addr, const volatile void *read_addr,
                           uint transfer_count, bool trigger) {
  s_ch[channel].config = *config;
  s_ch[channel].write = write_addr;
  s_ch[channel].read = read_addr;
  s_ch[channel].count = transfer_count;
  if (trigger) run(channel);
}

void dma_channel_start(uint channel) { run(channel); }

void dma_start_channel_mask(uint32_t mask) {
  for (uint i = 0; i < NUM_DMA_CHANNELS; i++) {
    if (mask & (1u << i)) run(i);
  }
}
