# Provenance of the vendored CHIP engine (OS-CECHIP C1)

Taken from Polyend's published firmware source, `polyend/tracker-ce`, as an approved MIT component (maintainer, 2026-11-05; see `THIRD_PARTY.md`).
Nothing else of tracker-ce is vendored, and `synth/lib/*.a` (proprietary) is never used.

- Repository: https://github.com/polyend/tracker-ce
- Commit: `7fd6c2c3002a8f690b5341c486ba69606077b513` (committed 2026-11-04)
- Fetched: 2026-11-06, unmodified, with `git show 7fd6c2c:<path>`

| File here | Path in tracker-ce at that commit | Bytes | SHA-256 |
|---|---|---|---|
| `ChiptuneSynthVoice.cpp` | `core/synth/engines/ChiptuneSynthVoice.cpp` | 37051 | `d0558823a3cc30d41ea0c98efffdf322d7d3809f2222112149fc0f37f78c2298` |
| `LICENSE` | `LICENSE` (the whole file: Parts 1, 2 and 3) | 6087 | `92ea2a7264d71826a45e0095badad93c6b22b1c72dc8cf704f43eee1484bf3bb` |

Both files are byte for byte the originals: ctest `cechip_oracle_sha` (`check_sha256.cmake`) fails on any difference. The engine file carries no copyright
header of its own; the copyright line and the permission notice (with Part 2's conditions) are those of `LICENSE`, which travels with it.

Ours, in this directory: `CMakeLists.txt` (the host-only static library `cechip_oracle`), `check_sha256.cmake` and this file. The headers the engine includes
(`config.h`, `ChiptuneSynthVoice.h`, `PolySynth/sdk/SynthVoiceSDK.h`) are ours, in `tests/support/cechip_shim/`; the wrapper is `tests/support/cechip_ref.{h,cpp}`.
