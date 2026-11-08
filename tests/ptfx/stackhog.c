/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `stackhog`: recurses deeper than the plugin stack (the gate must report a stack
   overflow or a fault, not hang). */
#include "ptfx_plugin_abi.h"

static volatile int32_t s_sink;

static int32_t recurse(int32_t n) {
  if (n <= 0) {
    return 0;
  }
  s_sink += n;
  return recurse(n - 1) + 1;
}

void plugin_init(void* scratch, uint32_t scratch_size, const PtfxTestHostApi* host) {
  (void)scratch;
  (void)scratch_size;
  (void)host;
}

void plugin_set_param(uint8_t index, float value) {
  (void)index;
  (void)value;
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
  (void)in;
  (void)out;
  (void)samples;
  (void)channels;
  return recurse(100000);
}
