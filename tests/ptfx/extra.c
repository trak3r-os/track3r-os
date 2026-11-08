/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `extra`: prepend and append samples through host->set_extra_samples, from init and from
   every set_param (A10: it counts during the init phase). The audio itself passes through unchanged. */
#include "ptfx_plugin_abi.h"

static const PtfxTestHostApi* s_host;
static uint32_t s_pre = 100;
static uint32_t s_post = 200;

static void latch(void) {
  if (s_host != 0 && s_host->set_extra_samples != 0) {
    s_host->set_extra_samples(s_pre, s_post);
  }
}

void plugin_init(void* scratch, uint32_t scratch_size, const PtfxTestHostApi* host) {
  (void)scratch;
  (void)scratch_size;
  s_host = host;
  s_pre = 100;
  s_post = 200;
  latch();
}

void plugin_set_param(uint8_t index, float value) {
  if (index == 0) {
    s_pre = (uint32_t)value;
  } else if (index == 1) {
    s_post = (uint32_t)value;
  }
  latch();
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
  for (uint8_t c = 0; c < channels; ++c) {
    const int16_t* src = in[c];
    int16_t* dst = out[c];
    if (src == 0 || dst == 0) {
      continue;
    }
    for (uint32_t i = 0; i < samples; ++i) {
      dst[i] = src[i];
    }
  }
  return (int32_t)samples;
}
