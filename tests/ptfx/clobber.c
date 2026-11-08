/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `clobber`: overwrites the host's callee-saved registers (r4-r11, s16-s31) and FPSCR and
   returns normally. The gate must leave them as they were. */
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
  __asm volatile(
      "mov r4, #0x11\n"
      "mov r5, #0x22\n"
      "mov r6, #0x33\n"
      "mov r7, #0x44\n"
      "mov r8, #0x55\n"
      "mov r9, #0x66\n"
      "mov r10, #0x77\n"
      "mov r11, #0x88\n"
      "vmov.f32 s16, #16.0\n"
      "vmov.f32 s17, #17.0\n"
      "vmov.f32 s18, #18.0\n"
      "vmov.f32 s19, #19.0\n"
      "vmov.f32 s20, #20.0\n"
      "vmov.f32 s21, #21.0\n"
      "vmov.f32 s22, #22.0\n"
      "vmov.f32 s23, #23.0\n"
      "vmov.f32 s24, #24.0\n"
      "vmov.f32 s25, #25.0\n"
      "vmov.f32 s26, #26.0\n"
      "vmov.f32 s27, #27.0\n"
      "vmov.f32 s28, #28.0\n"
      "vmov.f32 s29, #29.0\n"
      "vmov.f32 s30, #30.0\n"
      "vmov.f32 s31, #31.0\n"
      "vmrs r0, fpscr\n"
      "orr r0, r0, #0x00000F00\n"
      "vmsr fpscr, r0\n"
      :
      :
      : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r0",
        "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
        "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31", "memory");
  return (int32_t)samples * (int32_t)channels;
}
