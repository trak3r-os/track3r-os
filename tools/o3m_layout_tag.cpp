// SPDX-License-Identifier: GPL-3.0-or-later
// OS-FLASHDIET I1/I2: prints model::kImportLayoutTag (model/import_layout.h), the importer's layout tag, so the build can hand it to
// tools/o3m_pack.py and tools/o3m_gen.py (--layout-tag-file) instead of mirroring the hash in Python. Host tool only: the tag only contains
// host/target-stable layouts, so the number printed here is the one the cross build computes too (ctest o3m_layout_tag_pin checks the pins).
#include <o3os/model/import_layout.h>

#include <cstdio>

int main() {
  std::printf("0x%08X\n", o3os::model::kImportLayoutTag);
  return 0;
}
