/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `fault_scs`: reads SCB->CPUID (0xE000ED00) from unprivileged mode, which is a BusFault
   (ARMv7-M: the System Control Space is privileged). The gate must contain it. */
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
  volatile uint32_t cpuid = *(volatile uint32_t*)0xE000ED00u;
  (void)in;
  (void)out;
  (void)channels;
  return (int32_t)samples + (int32_t)cpuid;
}
