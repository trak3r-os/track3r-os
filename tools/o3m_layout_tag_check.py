#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ctest o3m_layout_tag_pin: model/import_layout_tag.txt (the importer layout tag the packer and o3m_gen.py read by default) equals
model::kImportLayoutTag as printed by the host-compiled printer (tools/o3m_layout_tag.cpp).

  o3m_layout_tag_check.py <printed tag file>

When a model layout the importer writes changes, kImportLayoutTag moves: rebuild, run this check, and put the printed number into
model/import_layout_tag.txt (tests/module_importer_test.cpp pins the C++ side and also checks the built tracker.tmod's word 44).
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TAG_FILE = ROOT / "model" / "import_layout_tag.txt"


def main(argv):
    if len(argv) != 2:
        print("usage: o3m_layout_tag_check.py <printed tag file>", file=sys.stderr)
        return 2
    printed = int(Path(argv[1]).read_text().strip(), 0)
    pinned = int(TAG_FILE.read_text().strip(), 0)
    if printed != pinned:
        print("o3m_layout_tag_pin: FAILED: model/import_layout_tag.txt says %#010x, model::kImportLayoutTag is %#010x: "
              "put the printed number into model/import_layout_tag.txt" % (pinned, printed), file=sys.stderr)
        return 1
    print("o3m_layout_tag_pin: ok (the tag is %#010x)" % printed)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
