#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The whole release kit (OS-RELGHA), the one script that both a person and .github/workflows/release.yml run:
#
#   ARM_GNU_ROOT=<Arm GNU toolchain> tools/release/make_kit.sh <tag> <outdir> [<ptf>]
#
# <tag> is v<major>.<minor>.<patch>[-suffix]; <outdir> is a new or empty directory (below build*/ when it is inside the repository). The .ptf is the
# gated image: <ptf> when given, else <outdir>/track3r-os-<tag>.ptf must already exist. The script builds the modules (host build with the Arm
# compiler, the o3m_<name>_tmod targets of the starter manifest, and o3_ptfx), makes the modules and examples zips with make_starter.py --split
# (third-party downloads and the Mod Archive licence pages are fetched and checked: any failed fetch or mismatch fails the run), and writes
# <outdir>/SHA256SUMS over the three release files:
#   track3r-os-<tag>.ptf  track3r-os-<tag>-modules.zip  track3r-os-<tag>-examples.zip
# Intermediate files stay in <outdir>/work/. Jobs: O3OS_KIT_JOBS (default 4). Nothing is published or uploaded.
set -euo pipefail

[ $# -ge 2 ] && [ $# -le 3 ] || { sed -n '5,9p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
TAG=$1
OUT=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.-]+)?$ ]] || { echo "make_kit: tag $TAG is not v<major>.<minor>.<patch>[-suffix]" >&2; exit 2; }
VERSION=${TAG#v}
PTF_NAME="track3r-os-${TAG}.ptf"
: "${ARM_GNU_ROOT:?make_kit: set ARM_GNU_ROOT to the Arm GNU toolchain (docs/toolchain.md)}"
JOBS=${O3OS_KIT_JOBS:-4}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
if [ $# -eq 3 ]; then
  [ "$(realpath "$3")" = "$OUT/$PTF_NAME" ] || cp "$3" "$OUT/$PTF_NAME"
fi
[ -s "$OUT/$PTF_NAME" ] || { echo "make_kit: $OUT/$PTF_NAME is missing: pass the .ptf as the third argument" >&2; exit 2; }
rm -f "$OUT/SHA256SUMS" "$OUT/track3r-os-${TAG}-modules.zip" "$OUT/track3r-os-${TAG}-examples.zip"

WORK="$OUT/work"
mkdir -p "$WORK"

# The targets: every module of the manifest (the same list make_starter.py packs).
mapfile -t TARGETS < <(python3 -I -c '
import json, sys
m = json.load(open(sys.argv[1]))["modules"]
for kind in sorted(m):
    for name in m[kind]:
        print("o3m_%s_tmod" % name)
' "$ROOT/tools/release/starter_manifest.json")
[ "${#TARGETS[@]}" -gt 0 ] || { echo "make_kit: no module targets in the manifest" >&2; exit 1; }

cmake -S "$ROOT" -B "$WORK/mods" -DO3OS_RATE_44100=ON "-DARM_GNU_ROOT=$ARM_GNU_ROOT" > "$WORK/mods-cfg.log" 2>&1 || { tail -30 "$WORK/mods-cfg.log"; exit 1; }
cmake --build "$WORK/mods" -j"$JOBS" --target "${TARGETS[@]}" o3_ptfx > "$WORK/mods-build.log" 2>&1 || { tail -40 "$WORK/mods-build.log"; exit 1; }
# The same rule as the image build: a warning stops the release.
if grep -nEi 'warning[:(]|CMake (Deprecation )?Warning' "$WORK/mods-cfg.log" "$WORK/mods-build.log"; then echo "make_kit: warnings in the modules build" >&2; exit 1; fi

python3 -I "$ROOT/tools/release/make_starter.py" --split --version "$VERSION" --ptf "$OUT/$PTF_NAME" \
  --modules-dir "$WORK/mods/modules" --ptfx-dir "$WORK/mods/modules/ptfx" --out "$WORK/starter" --cache "${O3OS_KIT_CACHE:-$WORK/cache}"
for k in modules examples; do
  cp "$WORK/starter/track3r-os-${TAG}-$k.zip" "$OUT/"
done

(cd "$OUT" && sha256sum "$PTF_NAME" "track3r-os-${TAG}-modules.zip" "track3r-os-${TAG}-examples.zip" > SHA256SUMS && sha256sum -c SHA256SUMS)
echo "make_kit: $TAG kit in $OUT"
