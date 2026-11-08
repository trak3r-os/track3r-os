/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `gain`: an integer gain, exact (int16 * int with saturation). Effect, mono. */
#include "ptfx_plugin_abi.h"

static const PtfxTestHostApi* s_host;
static int32_t s_gain = 2;

void plugin_init(void* scratch, uint32_t scratch_size, const PtfxTestHostApi* host) {
  (void)scratch;
  (void)scratch_size;
  s_host = host;
  s_gain = 2;
}

void plugin_set_param(uint8_t index, float value) {
  if (index == 0) {
    s_gain = (int32_t)value;
  }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
  (void)s_host;
  for (uint8_t c = 0; c < channels; ++c) {
    const int16_t* src = in[c];
    int16_t* dst = out[c];
    if (src == 0 || dst == 0) {
      continue;
    }
    for (uint32_t i = 0; i < samples; ++i) {
      dst[i] = ptfx_test_sat16((int32_t)src[i] * s_gain);
    }
  }
  return (int32_t)samples;
}
