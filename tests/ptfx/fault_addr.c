/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `fault_addr`: writes to an unmapped address (0x50000000 is mapped on neither the MK66
   nor the QEMU mps2-an386 test target). The gate must contain it. */
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
  (void)in;
  (void)out;
  (void)channels;
  *(volatile uint32_t*)0x50000000u = samples;
  return (int32_t)samples;
}
