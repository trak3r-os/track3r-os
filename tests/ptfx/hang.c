/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `hang`: never returns from process_block (the gate must time it out). */
#include "ptfx_plugin_abi.h"

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
  volatile uint32_t sink = 0;
  (void)in;
  (void)out;
  (void)samples;
  (void)channels;
  for (;;) {
    ++sink;
  }
}
