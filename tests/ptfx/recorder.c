/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `recorder`: logs every process_block call into the scratch (8 bytes a call: samples u32,
   channels u8, NULL in-entries u8, pad u16; the first 4 bytes of the scratch hold the call count). */
#include "ptfx_plugin_abi.h"

static const PtfxTestHostApi* s_host;
static uint8_t* s_scratch;
static uint32_t s_scratch_size;

typedef struct {
  uint32_t samples;
  uint8_t channels;
  uint8_t nulls;
  uint16_t pad;
} Rec;

void plugin_init(void* scratch, uint32_t scratch_size, const PtfxTestHostApi* host) {
  s_host = host;
  s_scratch = (uint8_t*)scratch;
  s_scratch_size = scratch_size;
  if (s_scratch != 0 && s_scratch_size >= 4) {
    *(uint32_t*)s_scratch = 0;
  }
}

void plugin_set_param(uint8_t index, float value) {
  (void)index;
  (void)value;
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
  (void)s_host;
  (void)out;
  if (s_scratch != 0 && s_scratch_size >= 4 + sizeof(Rec)) {
    uint32_t n = *(uint32_t*)s_scratch;
    if (4 + (n + 1) * sizeof(Rec) <= s_scratch_size) {
      Rec* r = (Rec*)(s_scratch + 4 + n * sizeof(Rec));
      uint8_t nulls = 0;
      for (uint8_t c = 0; c < channels; ++c) {
        if (in[c] == 0) {
          ++nulls;
        }
      }
      r->samples = samples;
      r->channels = channels;
      r->nulls = nulls;
      r->pad = 0;
      *(uint32_t*)s_scratch = n + 1;
    }
  }
  return (int32_t)samples;
}
