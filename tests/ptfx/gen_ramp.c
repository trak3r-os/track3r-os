/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `gen_ramp`: a generator writing a waveform into out[0], ignoring in. */
#include "ptfx_plugin_abi.h"

static uint8_t s_wave = 0;

void plugin_init(void* scratch, uint32_t scratch_size, const PtfxTestHostApi* host) {
  (void)scratch;
  (void)scratch_size;
  (void)host;
  s_wave = 0;
}

void plugin_set_param(uint8_t index, float value) {
  if (index == 0) {
    s_wave = (uint8_t)value;
  }
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
  (void)in;
  (void)channels;
  int16_t* dst = out[0];
  if (dst == 0) {
    return (int32_t)samples;
  }
  for (uint32_t i = 0; i < samples; ++i) {
    int32_t v;
    switch (s_wave) {
      case 1:  v = (int32_t)((i & 255u) << 8) - 32768; break;   /* saw */
      case 2:  v = ((i & 1u) != 0u) ? 32767 : -32768; break;     /* square */
      default: v = (int32_t)(i & 0xFFFFu) - 32768; break;        /* ramp */
    }
    dst[i] = ptfx_test_sat16(v);
  }
  return (int32_t)samples;
}
