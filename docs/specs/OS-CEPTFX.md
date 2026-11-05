# OS-CEPTFX: run tracker-ce `.ptfx` Sample Editor plugins

Status: spec, 2026-11-05; the maintainer's decisions taken 2026-11-05 (below the table). Item approved by the maintainer 2026-11-05 ("I want to be able to use tracker-ce's effects: load them and use them"). Level C: a foreign binary ABI runs on the unit. Docs only; nothing in the tree was changed. Our facts come from the tree at `docs/OS-CEPTFX-spec` (from dev 742effc2). The tracker-ce facts come from `tracker-ce@7fd6c2c` (Polyend's published firmware source, cited as facts; no code is copied). The six SDK examples were built once with our Arm GNU Toolchain 15.3.Rel1 in an untracked scratch folder to check the binary layout; nothing of that build is committed.

## Maintainer decisions

| # | Question | Options | Recommended default | Why |
|---|---|---|---|---|
| Q1 | Who may run a `.ptfx` | (a) Any `.ptfx` in `/Effects` is listed and can run, like a `.tmod` (ADR 0002: trusted community code, no signing). (b) Off by default behind a setting. | (a) | It is ADR 0002's open-modules decision. The containment of Q2 and Q3 limits the damage. (b) adds a field to the settings file: a persistent-format bump. |
| Q2 | A plugin that faults (bad address, bad instruction, privileged access) | (a) Contain it: abort the run, quarantine the plugin for this boot, keep the OS running and say `PLUGIN CRASHED: SAVE, THEN RESTART`. Reset only when the fault cannot be pinned on the plugin. (b) ADR 0002's default: crash record, reset, quarantine at the next boot. | (a) | With (b), a bad plugin in an offline edit costs the user every unsaved change. Nothing fences RAM from the plugin (see Facts), so (a) still tells the user to save and restart. |
| Q3 | A plugin that never returns | (a) A 2 s limit per call. The SysTick handler aborts the call. Plugins run unprivileged, so they cannot mask it. (b) No limit until the watchdog lands (it is off today): a hang needs a power cycle. | (a) | One check per millisecond in an existing ISR. Without it, one bad file freezes the unit. |
| Q4 | Stereo samples | (a) An effect plugin sees L and R (`channels = 2`, as tracker-ce gives on Tracker+ / Mini). A `CONVERT_TO_MONO` plugin makes the whole sample mono. Slice S5. (b) Refuse stereo samples (`STEREO: MAKE MONO FIRST`), as the original Tracker only ever passes one channel. | (a) | 4 of the 6 SDK examples declare `STEREO`, and Stereo to Mono is only useful this way. About 0.6 KiB of flash. |
| Q5 | Preview | (a) As our other effects: APPLY renders and commits one step of the sample's history, F7 plays it and F8 undoes it. (b) tracker-ce's flow: PREVIEW renders a pending result and plays it, then APPLY commits it. | (a) | One protocol for every effect, and no new editor state. The difference is one key press (F8). |
| Q6 | tracker-ce's 16 built-in effects (section 6) | (a) None now; reconsider after S6. (b) Later: rebuild the ones we lack as `.ptfx` from tracker-ce's MIT components (sources in our repo with the notice, files built as release assets, 0 flash). (c) Port the ones we lack into our Sample Editor (about 10 to 15 KiB of flash). | (a) now, then (b) | Flash is about 94 % used. A `.ptfx` costs no flash and also runs on tracker-ce. (b) and (c) need the maintainer's approval for each named component, with its notice in `THIRD_PARTY.md`. S0 to S6 need no licence approval (assumption A1). |

**Decided by the maintainer, 2026-11-05:** Q1 (a), Q2 (a), Q5 (a), Q6 (a) now, then (b) ("lo demás ok"). Q4 (a), worded by the maintainer as "si son stereo, hacerlos con nuestro estéreo": a stereo sample stays stereo in our own format (the bank's M/S of OS-STEREO, converted to L/R for the plugin and back with the WAV importer's rounding, section 3.6); a plugin without `STEREO` runs as two mono passes (A13). Q3 (a), 2 s per call ("lo que tú dices, 2 s"; a named constant, `kPtfxCallTimeoutMs`; S6 measures the slowest call and it is raised only if a real plugin comes near).

## Assumptions (decided here; technical, reversible)

- **A1. No tracker-ce code in the repository.**
  - Our loader, our header constants and our packer are written from the format facts below, with citations.
  - The SDK files (`tracker_plugin.h`, `ptfx_builder.py`, the six examples; MIT, LICENSE part 1, use of the SDK expressly permitted by part 2 1(b)) are fetched by `tools/testdata/testdata.py`, pinned by SHA-256, into the untracked `build-fixtures/`. They are only test fixtures; no `.ptfx` binary is ever committed.
- **A2. Validation is strict where safety depends on it and tolerant where only the UI does** (section 3.1). The UI side is tracker-ce's own tolerance (verified parity).
- **A3. The plugin workspace is a new SDRAM region `ptfx` of 768 KiB.**
  - It is carved from three regions nothing uses today: `file_job` (512 KiB) and `lut` (192 KiB), which are contiguous, plus the first 64 KiB of `ui_cache`. `ui_cache` keeps 448 KiB, which still holds the bench's 16 KiB scratch.
  - Every core access to the region goes through `core_view()` (the ICode alias), so plugin code runs cached (section 3.2).
- **A4. Code is loaded only when a run starts.** Browsing the list and editing parameters need only the header and the list strings, which are read from the file. Nothing executable stays resident between runs.
- **A5. The call gate.**
  - The plugin runs in unprivileged thread mode, on its own stack (PSP) in the `ptfx` region, and returns through `SVC`.
  - The host's callee-saved registers (r4-r11, s16-s31), FPSCR and MSP are saved and restored around every call.
  - SVCall, unused today (`platform/mk66/include/o3os/platform/mk66/irq_map.h:72-76`), gets the role `PluginGate`.
- **A6. Time limit.** 2,000 ms per call (init, set_param, process_block), checked in the SysTick handler.
- **A7. Quarantine.**
  - It lasts for the boot and holds up to 16 plugins, each by an id: `0x8000 |` a 15-bit FNV-1a hash of `"ptfx:" + file name`. This namespace is distinct from the `.tmod` stems.
  - The id is stamped with `set_module()` while a call runs (`platform/mk66/include/o3os/platform/mk66/platform.h:59`). A crash record that names it quarantines it at the next boot.
- **A8. Host table.**
  - Math is our own float code in `core/fast_math` (section 3.4), not newlib's libm (about 4 KB for sinf/cosf alone, `audio/src/module_voices.cpp:49-52`).
  - `rand()` keeps newlib's range (0..0x7FFFFFFF). It is deterministic and reseeded at every run, so a run is repeatable.
  - `sample_rate` is the instrument's rate (0 means 44,100). A generator on an empty slot gets 44,100.
- **A9. The calls are tracker-ce's sequence exactly** (section 3.5). The output of a plugin that depends on block size is therefore the same as on tracker-ce.
- **A10. `set_extra_samples`.** It counts when called from `plugin_init` or from the parameter replay that follows it. It is ignored during `process_block`. The total must fit the edit pool.
- **A11. Return values.**
  - A `process_block` that returns 0 or less ends the run (verified parity).
  - One that returns more than it was asked for fails the run with `PLUGIN ERROR` (new). tracker-ce would advance its output pointer past the buffer.
- **A12. Generators.** They always run with `channels = 1` (verified parity with the original Tracker). The result is mono, even on a stereo slot (undo brings the stereo sample back).
- **A13. An effect plugin without `STEREO` on a stereo sample runs twice, as two mono passes over L and then R.** It is re-initialised in between. A plugin written for the original Tracker (always one channel) may write `out[0]` only. Both passes must give the same length, or the run fails. A `STEREO` plugin runs once with `channels = 2`.
- **A14. The catalogue.**
  - It is scanned on the first visit of the PLUGINS page in a boot, and again on F2 RESCAN or a card change.
  - It holds up to 128 files (verified parity), sorted by display name without case.
  - A file name over 47 characters is skipped and counted. tracker-ce truncates it to 41 and then fails to open it.
  - `/Effects` is never created. tracker-ce creates it; we write nothing to the card while browsing.
- **A15. Parameter values** are kept per plugin (by file name, for the last 8 plugins) while the OS runs. They are not saved, so no format changes.
- **A16. Simulator.** It lists plugins (the headers parse anywhere). Running one needs a native stand-in registered by a test. Otherwise the sim says `PLUGINS RUN ON THE TRACKER`.
- **A17. One plugin call per main-loop pass.** A call can take up to 8,192 frames. The job tells the runner its steps are not divisible (`core::Job` gains a `long_step()` hint), so the overrun counter does not count them.

## 1. Goal

A `.ptfx` file built with tracker-ce's `editor-fx-sdk` and copied to `/Effects` on the card shows up in our Sample Editor. The user sets its parameters and applies it to the selection, or to the whole sample. It behaves as on tracker-ce:
- the same calls, block sizes and limits;
- generators and wavetable generators;
- extra samples;
- stereo as on Tracker+.

Our own guarantees come on top: one undo step per apply, a time limit, containment of a crash, quarantine, and no tracker-ce code in our tree. A plugin that runs on tracker-ce on the original Tracker (not `IMX_ONLY`) runs here.

## 2. Facts

### 2.1 The `.ptfx` format and the SDK (tracker-ce)

| Fact | Evidence |
|---|---|
| File: a 472-byte header, then the code payload at `code_offset` (472 from the builder). The payload is `.text` + `.rodata`, `.data`, `.ptfx_strings`, `code_size` bytes. `.bss` (`bss_size`) is zeroed in RAM after it. Little endian. | `tracker-ce@7fd6c2c:editor-fx-sdk/tracker_plugin.h:44-61`, `ptfx_builder.py:18,333-349,384-386` |
| Header: magic `0x58465450` ("PTFX") at +0, `format_version` u16 = 1 at +4, `flags` u16 at +6, `display_name[32]` at +8, `author[16]` at +40, `num_params` u8 at +56, `scratch_size` at +60, `code_offset` +64, `code_size` +68, `bss_size` +72, `init_offset` +76, `set_param_offset` +80, `process_block_offset` +84, `params[12]` of 32 bytes at +88. | `tracker_plugin.h:11,45-61` |
| Flags: GENERATOR 0x01, STEREO 0x02, WAVETABLE 0x04, CONVERT_TO_MONO 0x08, IMX_ONLY 0x10 ("code > 32 KB or requires Cortex-M7"). EFFECT is 0. | `tracker_plugin.h:14-21` |
| A parameter: `label[8]`, then the floats `min`, `max`, `default`, `step` at +8..+20, `unit[6]` at +24, `display_type` `'f'` / `'d'` / `'l'` at +30. For `'l'`, `unit[0..1]` is a u16 offset into the payload and `unit[2]` an item count of NUL-terminated strings (up to 16 items of up to 10 characters, from the builder). | `tracker_plugin.h:29-42`, `ptfx_builder.py:107,355-363` |
| ABI: `void init(void* scratch, uint32_t scratch_size, const PtfxHostApi*)`, `void set_param(uint8_t, float)`, `int32_t process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels)`. AAPCS hard-float (the float in s0). | `tracker_plugin.h:88-90`, `ptfx_builder.py:237-253` |
| Host table, in this order: `sample_rate` (u32), `sinf`, `cosf`, `tanf`, `tanhf`, `powf`, `expf`, `sqrtf`, `logf`, `log10f`, `fminf`, `fmaxf`, `fabsf`, `memcpy`, `memset`, `rand`, `set_extra_samples(prepend, append)`. tracker-ce fills it with libm and libc and 44,100. | `tracker_plugin.h:64-85`, `core/effector/ptfx_loader.cpp:20-38` |
| Build: `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -O3 -fno-builtin -fPIC -mpic-data-is-text-relative -mlong-calls -nostdlib`. Linked at address 0 by a script that puts `.text`/`.rodata`, then `.data`, `.ptfx_strings`, then `.bss` (each 4-aligned). No relocation table is emitted. The entry offsets come from `nm`. | `ptfx_builder.py:31-56,237-305` |
| The SDK's `author` field is always written as zeros. The builder's docstring says "344-byte" header; the code writes 472. | `ptfx_builder.py:5,18,330` |
| Our build of the six examples (GCC 15.3, the builder's flags): `.text` holds only `R_ARM_REL32` relocations (PC-relative). The only `R_ARM_ABS32` are in `.data.rel.ro.local` (the `PLUGIN_PARAMS` table's label and unit pointers, which the plugin code never reads). Sizes: Add Silence 678 B code + 5 B bss; Analog Kick 732 + 12; Chiptune 2,458 + 32; Multi-Mode Filter 1,728 + 44; Stereo Delay 2,345 + 12 (scratch 176,404 B); Stereo to Mono 952 + 5. | Measured here, 2026-11-05 (`readelf -r`, `nm` on the objects; untracked scratch build) |
| `.bss` starts at `align4(code_size)`. tracker-ce zeroes `[code_size, code_size + bss_size)`, which misses up to 3 bytes at the end of `.bss` when `code_size` is not a multiple of 4 (Chiptune: payload ends at 2,458, `.bss` is 2,460..2,492). | `ptfx_builder.py:39-54`, `core/effector/ptfx_loader.cpp:254`; the Chiptune layout measured here |
| Examples: Add Silence (EFFECT, STEREO; prepend or append via `set_extra_samples`), Analog Kick Gen and Chiptune Voice Gen (GENERATOR), Multi-Mode Filter (EFFECT, STEREO), Stereo Delay (EFFECT, STEREO, 176,404 B scratch, tail), Stereo to Mono (STEREO, CONVERT_TO_MONO). | `tracker-ce@7fd6c2c:docs/modular_effects_system.md` section 2 |

### 2.2 How tracker-ce loads and runs them

All of these are **verified parity** targets of this design unless the design section says otherwise.

| Fact | Evidence |
|---|---|
| Scan: `/Effects`, files ending `.ptfx` (case-insensitive), up to 128. A file whose header is refused (magic, version ≠ 1, IMX_ONLY on MK66) is not listed. An empty display name falls back to the file name. tracker-ce creates `/Effects` when it is missing. | `core/effector/ptfx_manager.h:9`, `ptfx_manager.cpp:28-125` (`:58` mkdir, `:75` filter, `:92` IMX_ONLY, `:109` fallback) |
| Load on MK66: code + bss at most 32 KiB, in SDRAM (`externalRamScratchBuff`). Read in chunks of 256 bytes. No relocation. Entry = base + offset, with bit 0 set. Then the loader **switches the SYSMPU off** (`MPU_CESR = 0`) and invalidates the LMEM code cache. The SD player is stopped first, because it shares that buffer. | `core/effector/ptfx_loader.cpp:57-64,126-135,162-267` (`:226` stop, `:254` bss, `:262-264` entries) |
| Scratch: requested size clamped to 640 KiB (655,360 B). Zeroed before every `init`. | `ptfx_loader.h:7-9`, `ptfx_loader.cpp:66-75`, `mtEffectModular.cpp:27,105` |
| A run (`startProcess`): zero scratch, `init`, then `set_param(i, v)` for **all 12** indices (0.0 past `num_params`); extra samples reset to 0 before `init`; the run's length is computed after the replays. | `core/effector/mtEffectModular.cpp:92-138` |
| Blocks: at most 8,192 frames per call, split at the prepend / body / append boundaries. `in` is an array of channel pointers whose entries are NULL outside the body and for a generator (the array itself is never NULL). `channels` is 1 on MK66 and 2 on Tracker+ / Mini, whatever the STEREO flag. A return ≤ 0 ends the run; the frames produced so far are the result. One call per `update()` of the main loop. | `mtEffectModular.cpp:140-225` (`:147`, `:173`, `:179-186`), `mtEffect.h:14-18`, `mtEffect.cpp:401-411` |
| Generators: target 88,200 frames (2 s). They replace the whole sample (no copy of the old audio). Applied on an inactive slot, they make the instrument active, named `Gen NN`. WAVETABLE sets the instrument's play mode to wavetable. | `mtEffectModular.cpp:13-72` (`:22`, `:33`), `:227-246` (`:236`, `:243`) |
| Apply swaps the processed and confirmed buffers; one undo level in the editor. | `core/effector/mtEffect.cpp:197-232` |
| Parameter UI: count clamped to 12. List strings read within `code_size`, at most 16. A list with no valid string is shown as an integer. Any display type other than `'d'` / `'l'` is shown as a float. | `core/interface/sampleEditor/fSampleEdit.cpp:2186-2262`, `sampleEditor.h:9` |
| CONVERT_TO_MONO on Tracker+ keeps the left channel when the edit is saved back. | `docs/modular_effects_system.md` section 4, flag table |

### 2.3 Ours

| Fact | Evidence |
|---|---|
| The SDRAM answers at 0x7000_0000 (system bus, uncached) and at the ICode alias 0x0800_0000 (code bus, held by the 8 KiB LMEM code cache, write-through, no allocate on a write miss). The single-alias rule: a region is accessed by every core access through one view only. The sample arena, the project and the module region already use the alias through `core_view()`. | `platform/mk66/include/o3os/platform/mk66/mem_map.h:44-50`, `sdram_view.h:4-36`, `lmem.h:4-26` |
| The module loader runs cold module code from SDRAM through the alias. Module inserts ran on the unit (OS-INSFX S1, S2, unit PASS 2026-11-01). The speed of code from the alias is not measured. | `module/sdk/README.md` "What is real" and section 10; `platform/mk66/src/app_main.cpp:790-797` |
| `file_job` (512 KiB), `lut` (192 KiB) and `ui_cache` (512 KiB) are contiguous and used by nothing in the image but the bench's scratch in `ui_cache`. | `mem_map.h` SDRAM table; no user outside `mem_map.h`, `bench.cpp` and the tests (grep, 2026-11-05) |
| SRAM_L's 14 KiB module slab holds the hot code of loaded instruments. SRAM_U keeps its 32 KiB minimum free. Neither has room for a 32 KiB plugin. | `mem_map.h:52-80` |
| Faults: a naked stub moves to a fault stack, records a crash record naming the stamped module, and resets. The watchdog is off (its re-enable point is marked). | `platform/mk66/src/startup.cpp:63-89`, `crash_record.h:27-65`, `platform/mk66/src/main.cpp:240-244` |
| The FPU is enabled for privileged and unprivileged code (CPACR CP10/CP11 full access). | `platform/mk66/src/startup.cpp:114`, `platform/mk66/src/regs.h:140` |
| The Sample Editor's edit protocol: a `core::Job` on the main loop writes into a spare buffer of the bank's edit pool. `sample.commit` swaps it in as one undo unit and one step of the sample's history (three steps). A stereo sample is M and S blocks (S = L - M) in one pool buffer. | `app/include/o3os/app/screens/sample_edit_screen.h:28-34`, `sample_edit_job.h:24-27`, `sample_bank.h:385-405` |
| The Sample Editor has an edit page and an FX page (F1 FILTER, F2 COMPRESS, F3-F6 free). The instrument keeps its own sample rate (0 means 44,100). Play mode 6 is Wavetable. | `sample_edit_screen.h:9-14`, `model/include/o3os/model/instrument.h:120-123`, `app/include/o3os/app/play_mode.h:14` |
| ARMv7-M: in unprivileged thread mode, `CPS` and `MSR` to PRIMASK, BASEPRI, FAULTMASK or CONTROL have no effect, and an access to the System Control Space (SCB, NVIC, SysTick, DWT) is a BusFault. | ARMv7-M Architecture Reference Manual (DDI 0403): the CPS and MSR descriptions, and "System Control Space" |

### 2.4 Open research questions

- **R1.** Is the Cortex-M4 of the MK66 built without a core MPU? We believe so (NXP's Kinetis K headers declare none, and our tree uses none). The design does not need one.
- **R2.** Can the K66 SYSMPU deny the core access to a region while RGD0 grants it everything (ADR 0002 validation 5)? tracker-ce switches the SYSMPU off before running a plugin. We do not, and module code already runs from the alias. The design does not rely on the SYSMPU.
- **R3.** Are the peripherals supervisor-only for the core by default (AIPS PACR)? If so, an unprivileged plugin that touches one faults and is contained. If not, it can reach peripherals. The SCS is protected either way.
- **R4.** How close is our output to tracker-ce's on real plugins? We use our own math (A8), and the compiler may fuse multiply-adds differently. Expected: within a few LSB. Measured on the host against glibc's libm (S2) and on the unit (S6). No tracker-ce output file is available as a reference.
- **R5.** How fast is plugin code from the SDRAM alias (DWT cycles per frame of Multi-Mode Filter, S6)?

## 3. Design

### 3.1 The loader and its checks

`module/include/o3os/module/ptfx_format.h` (pure, host-tested) parses the 472 header bytes with explicit little-endian reads (no packed struct) into a `PtfxInfo`. Each check below is **new Community OS behaviour** unless marked verified parity.

**Refused** (listed greyed with the reason; never loaded):

| Check | Message |
|---|---|
| File shorter than 472 B; magic not `PTFX` | `NOT A PTFX PLUGIN` |
| `format_version` ≠ 1 (verified parity) | `PTFX VERSION n NOT SUPPORTED` |
| `IMX_ONLY` (verified parity) | `NOT FOR THIS TRACKER` |
| A flag bit above 0x10 | `NEEDS NEWER OS` |
| `code_offset` < 472; `code_offset + code_size` beyond the file; `code_size` 0 | `PLUGIN FILE DAMAGED` |
| `align4(code_size) + bss_size` > 32 KiB (the MK66 limit, verified parity) | `PLUGIN TOO BIG` |
| An entry offset (bit 0 masked) not inside `[0, code_size)`, or two entry offsets equal (the builder writes 0 for a missing function) | `PLUGIN FILE DAMAGED` |

**Tolerated** (verified parity with tracker-ce's UI, `fSampleEdit.cpp:2186-2262`):
- `num_params` is clamped to 12.
- A list's items are read only inside the payload and capped at 16. A list with no valid item is shown as an integer.
- Any display type other than `'d'` / `'l'` is shown as a float.
- `scratch_size` is clamped to 640 KiB (verified parity, `mtEffectModular.cpp:27`).

**New sanitising:**
- A non-finite or reversed range is shown as an integer 0..0, and the parameter is not editable.
- A non-printable character in a name, label or unit shows as `?`.
- A default outside the range is clamped.

The list strings are read from the file (`code_offset + list_offset`) when a plugin is selected (A4). The catalogue keeps, per file:
- the file name (≤ 47 characters);
- the display name;
- the flags;
- a status (ok, or the refusal);
- the plugin's id (A7);
- whether it is quarantined.

That is 88 B a file, 11 KiB for 128 files, in the `ptfx` region.

The scan (`module/.../ptfx_catalog.h`) is a stepped object driven by a `core::Job`: one directory page or one 472-byte header read a step, with progress. A card error is not a refusal: the file is retried at the next scan.

### 3.2 Where the code runs, and why

The code runs **in SDRAM, through the ICode alias**:
- Flash cannot take a file.
- SRAM_L's slab belongs to the loaded instruments.
- SRAM_U keeps its free 32 KiB.
- Uncached SDRAM (0x7000_0000) would work but, by ADR 0002's estimate, 10 to 30 times slower.

The alias is the path the module loader already uses for cold code.

The image is position-independent: `.text` uses only PC-relative literals (2.1). So the image is **not relocated**: it runs at the address it is copied to, exactly as on tracker-ce. Because the PC is in the alias, its `.data` and `.bss` accesses go through the alias too, which obeys the single-alias rule. An absolute pointer in `.data` (`R_ARM_ABS32`) cannot be fixed: the file carries no relocation table. Such a pointer stays a link-time offset from 0 and points into our flash. The same plugin is broken on tracker-ce for the same reason (risk 2).

The `ptfx` region (A3), 768 KiB, accessed only through `core_view()`, joins the list in `sdram_view.h`:

| Offset | Size | Use |
|---|---|---|
| 0 | 32 KiB | The code image: the payload, then the zeroed tail from `code_size` to `align4(code_size) + bss_size` (this also zeroes tracker-ce's 1-3 byte gap, 2.1) |
| 32 KiB | 4 KiB | Stack guard, painted with a pattern and checked after every call |
| 36 KiB | 12 KiB | The plugin's stack (PSP starts at 48 KiB, 8-byte aligned). An overflow runs into the guard and then into the plugin's own image, never into the region below (the undo journal) unless it passes 48 KiB. |
| 48 KiB | 32 KiB | Stereo input staging: L and R, 8,192 frames each (3.6) |
| 80 KiB | 640 KiB | Scratch (655,360 B: tracker-ce's maximum) |
| 720 KiB | 16 KiB | The catalogue (11 KiB), the selected plugin's parameter info and list strings (≤ 2.6 KiB), the host table, the run job's state |
| 736 KiB | 32 KiB | Spare |

Loading (in steps of 4 KiB on the main loop):
1. Read the payload through the alias address.
2. Zero the tail.
3. Call the platform's code-sync hook over the image: the module loader's `SyncFn` (DSB, the LMEM line invalidate over the range, ISB).
4. Point the entries at `base + offset | 1`.

The SYSMPU is not touched (R2). tracker-ce has to stop its SD player because it shares the buffer; we share nothing, so playback continues.

### 3.3 The call gate and containment

A hal interface, `hal/include/o3os/hal/code_gate.h`:

```
enum class GateResult : uint8_t { Ok, Fault, Timeout, StackOverflow, Unavailable };
GateResult call(entry, r0..r3, s0, timeout_ms, int32_t* ret)
```

It has three implementations:
- **Host:** calls a native function pointer directly, for tests and stand-ins; no containment.
- **QEMU and MK66:** one Cortex-M4F source shared by both builds.

The QEMU / MK66 gate, in about 60 instructions of assembly plus two handler hooks:

1. **Enter** (main loop, privileged, MSP):
   - Save r4-r11, s16-s31, FPSCR, LR and MSP into the gate's context (SRAM_U, about 160 B).
   - Set the gate state: active, the plugin's id (`set_module`), the stack and image bounds, and the deadline (now + 2,000 ms). The main loop is the single writer; the fields are `volatile`, written before a DSB.
   - Set PSP to the stack top, LR to the return stub, and CONTROL to SPSEL = 1, nPRIV = 1; ISB.
   - Branch to the entry with r0-r3 and s0 loaded.
   - From here the plugin cannot mask interrupts or touch the SCS (2.3).
2. **Normal return:** the plugin returns into the stub, which executes `SVC`. The SVC handler (asm) builds a basic frame just below the saved MSP, with PC = the gate's resume point and r0 = the plugin's r0. It sets CONTROL.nPRIV = 0, clears FPCCR.LSPACT (the plugin's lazy FP state is dropped) and returns to thread mode on MSP. The resume point restores the context and FPSCR, checks the stack guard, clears the gate state and `set_module(0)`, and returns `Ok` or `StackOverflow`.
3. **Timeout:** the SysTick handler gets a check costing a few cycles each millisecond. If the gate is active, the deadline has passed and the interrupted context is thread mode on PSP, it rewrites the stacked PC to an abort stub (xPSR = Thumb only) and records `Timeout`. The abort stub executes `SVC`, which leads to step 2's path with the reason kept. A tick that interrupted another ISR does nothing; the next one acts.
4. **Fault:** the fault stub first asks the gate: is it active, did the exception come from thread mode on PSP, and does the frame lie inside the plugin stack, or is it a stacking error?
   - If yes, it records CFSR, the stacked PC, BFAR / MMFAR and the reason `Fault`, and clears CFSR / HFSR (write-1-to-clear). It does not leave PRIMASK set. It forges a fresh basic frame at the plugin stack's top with PC = the abort stub, sets PSP, and returns to thread mode, ending as in 3.
   - If no (a fault in handler mode, the gate inactive, a lockup), the existing path runs: crash record (with the plugin's id still stamped), then reset, then quarantine at the next boot.
5. **After any result other than `Ok`:**
   - The run is aborted (nothing is committed).
   - The plugin is quarantined (A7).
   - The strip says `PLUGIN TIMED OUT: QUARANTINED`, `PLUGIN CRASHED: SAVE, THEN RESTART` or `PLUGIN STACK OVERFLOW: SAVE, THEN RESTART`.
   - A diagnostic log line keeps CFSR, PC (as an offset in the image when inside it) and the plugin's name.

This is new Community OS behaviour (Q2, Q3). It is ISR and fault-path code: Level C, with QEMU tests of every path (section 5).

The cost to the audio:
- The render preempts the plugin as it preempts any main-loop code.
- The exception frame of the interrupted plugin goes to its stack in SDRAM through the alias: 8 words, or 26 with lazy FP. That is under a microsecond.
- The SDRAM traffic of a plugin competes with the voices' sample reads, as an edit job's does.

None of this is per block when no plugin runs. S6 measures the render's worst block during a run.

### 3.4 The host table

The table is built in the `ptfx` region for each run (68 B), in the field order of 2.1.

| Member | Ours |
|---|---|
| `sample_rate` | The instrument's rate (0 means 44,100); a generator on an empty slot gets 44,100 (A8). New: tracker-ce always gives 44,100; for a 44.1 kHz sample the two are the same. |
| `sinf`, `cosf` | The module host's own (`audio/src/module_voices.cpp:47-90`), moved to `core/fast_math` so both hosts share one copy |
| `tanf` | sin / cos |
| `expf`, `logf`, `log10f` | `exp2_fast` / `log2_fast` (`core/include/o3os/core/fast_math.h`) times a constant |
| `powf` | `exp2(y · log2 x)`, with libm's special cases: x = 0, y = 0, x < 0 with an integer y (sign), x < 0 with a fractional y (NaN), and infinities |
| `tanhf` | From `exp2_fast`, odd and saturating (±1 beyond \|x\| > 9) |
| `sqrtf` | `VSQRT` (already linked) |
| `fminf`, `fmaxf`, `fabsf` | Trivial, with libm's NaN rules |
| `memcpy`, `memset` | The OS's |
| `rand` | A 64-bit LCG with newlib's range (0..0x7FFFFFFF), reseeded at the start of every run (A8) |
| `set_extra_samples` | Latches (prepend, append) while the run is in its init phase, and ignores the call afterwards (A10) |

The functions run in the plugin's unprivileged context. They touch no peripheral and no shared state but the latch. Accuracy target: relative error under 2e-6 over the ranges audio code uses, tested against the host's libm (S2). The output parity this gives is the open question R4.

### 3.5 The run

`app/include/o3os/app/ptfx_job.h`, a `core::Job` beside `SampleEditJob`, started from the PLUGINS page's APPLY or from `diag ptfx run`. Its phases, one bounded step each:

1. **Load** (3.2), in 4 KiB steps.
2. **Init.**
   - Zero the scratch, `scratch_size` clamped, 64 KiB a step.
   - Call `init(scratch, scratch_size, host)`, then `set_param(i, v)` for i = 0..11 (0.0 past `num_params`). This is verified parity, `mtEffectModular.cpp:92-138`.
   - Read the latched (prepend, append).
3. **Size.**
   - An effect's result is `frames - (b - a) + prepend + (b - a) + append`.
   - A generator's is 88,200 (verified parity), trimmed at the end to what it produced.
   - `edit_begin(slot, result, stereo)`. When it fails: `NO ROOM FOR THE EDIT`.
4. **Copy before:** frames [0, a) of the source, as the existing job does. A generator copies nothing: it replaces the whole sample (verified parity).
5. **Process.** One call per step. The block is `min(8192, remaining in the current region)`, split at prepend / body / append. `in` is an array of channel pointers: the source at the body's offset, NULL in the prepend and append regions and for a generator. `out` is the result at the running offset.
   - A return ≤ 0 ends this phase: the frames produced are the result (verified parity).
   - A return above `samples` fails the run (A11).
   - Progress is the frames produced against the result's length.
6. **Copy after:** frames [b, frames) after what was produced.
7. **Done.** The App's existing end-of-edit path stages the result and runs `sample.commit`: one undo unit, one step of the sample's history, and the instrument's points remapped when the length changed, as crop does. The job keeps the existing checks: the sample changed meanwhile, another job running, cancel (F8 CANCEL), a project load.

**Generators:**
- On a slot with a sample, the result replaces it: one history step, F8 brings the old sample back.
- On an empty slot, the result goes in like a loaded sample (the bank's load commit, mono, 44,100 Hz). The instrument becomes a sample instrument named `Gen NN`, NN being the slot number on two digits (verified parity of the name, `mtEffectModular.cpp:236`). Like a load, this makes no history step.
- WAVETABLE sets `play_mode` to 6, Wavetable (verified parity, `:243`).
- A generator ignores the selection (verified parity).

`CONVERT_TO_MONO` on a mono sample is a normal run. On a stereo sample, see 3.6.

**The main loop.** One call takes what the plugin takes for up to 8,192 frames. That is 1 to 20 ms for the SDK examples at an estimated 30 to 400 cycles a frame (S6 measures). The screen updates between calls. Live pads and the UI wait for the call to end. Audio does not wait: it is interrupt-driven. The 2 s limit bounds the worst case. This is the same trade tracker-ce makes (one block per `update()`).

### 3.6 Stereo and mono samples (S5, if Q4 a)

The bank holds a stereo sample as M and S. During a run the result's M and S blocks hold **L and R**, and a last in-place pass converts the produced range to M/S. The rounding is the WAV importer's (OS-STEREO (b)), so a stereo sample processed by a plugin that changes nothing comes back as the importer made it. The input is converted per block: L = sat(M + S), R = sat(M - S), into the 32 KiB staging. So the extra memory is 32 KiB whatever the sample's length.

| Sample | Plugin | Run |
|---|---|---|
| Mono | any | `channels = 1` (verified parity with the original Tracker) |
| Stereo | `STEREO` | one pass, `channels = 2` (verified parity with Tracker+) |
| Stereo | effect without `STEREO` | two mono passes, L then R, re-initialised in between; both must produce the same length (A13, new) |
| Stereo | `CONVERT_TO_MONO` | `channels = 2` over the **whole sample** whatever the selection. The result is L (`out[0]`) only, committed as a mono sample (`edit_commit` with no side), which drops S; undo brings it back. New behaviour: tracker-ce converts the selection and keeps L at save time, but a sample that is part mono cannot exist here. |
| Stereo | generator | `channels = 1`, mono result (A12) |

Before S5 lands, an effect plugin on a stereo sample says `STEREO: MAKE MONO FIRST`, as Compressor does today (`app/src/app.cpp:2665-2670`).

### 3.7 The UI: a PLUGINS page in Sample Edit (S4)

The Sample Edit button cycles edit page → FX page → **PLUGINS** → edit page. The page is new Community OS behaviour.

**Layout:**
- **Top:** the waveform and its selection, at the FX page's height (`kLaneHFx`).
- **Left (about 300 px), the list:** one row per file. Each row has the display name and tags: `GEN`, `WT`, `ST`, `MONO`. A refused or quarantined file is greyed, with its short reason on the row when selected. The header line reads `PLUGINS n` and, when files were skipped, `(k SKIPPED)`.
- **Right, the selected plugin's parameters:** up to 12 rows in two columns of six. Each row has the label, then the value with its unit, or the list item.
- **Under them:** the author, when the field is not empty (the SDK writes zeros), and the scratch it asks for.

**Keys** (field context `Context::SampleEditPlug`):

| Key | Action |
|---|---|
| Up / Down | The row in the focused column |
| Left / Right | Focus the list ↔ the parameters (and the column inside them) |
| Encoder | List: the plugin. Parameter: its value by `step`; a list parameter by one item. |
| Shift + encoder | A parameter by 10 steps |
| Delete | The parameter's default |
| F1 APPLY | Run on the selection, or on all of the sample when there is none. A generator replaces the sample or fills an empty slot. |
| F2 RESCAN | Scan `/Effects` again |
| F3 DEFAULTS | Every parameter of this plugin back to its default |
| F4-F6 | `-` |
| F7 PREVIEW | As on the other pages |
| F8 | UNDO (CANCEL while running) |
| Shift row | The edit page's |

- **Value display:** `'f'` values show two decimals and `'d'` integers, then the unit. A list shows its item. Values are kept per A15.
- **No sample:** on an empty slot an effect plugin shows greyed with `NO SAMPLE`, and only a generator can be applied.
- **Messages:** `PLUGIN <NAME> nn%`, then `DONE`. Errors use the texts of 3.1, 3.3, 3.5 and 3.6. `PLUGINS RUN ON THE TRACKER` appears in the sim without a stand-in.

### 3.8 Can the `.tmod` loader be reused?

**As a whole, no.** A `.tmod` is a non-PIC image with an ABS32 relocation table, a CRC, a descriptor and its own ABI (`module/sdk/README.md` section 8). A `.ptfx` is a PIC image with neither table nor CRC, and its own header and ABI.

**Its parts, yes:**
- the code-sync hook (`SyncFn`) and its LMEM invalidate;
- the module id stamping (`set_module`) and the crash-record quarantine at boot;
- the stepped storage read pattern;
- `Arena`, for the region's sub-blocks;
- the float services, shared through `core/fast_math` (3.4).

The quarantine list is a separate small set, so a plugin and a `.tmod` of the same name never quarantine each other (A7). ADR 0002 gets amendment 6 for this second, foreign kind of SD code: placement, the gate, containment, and the trust model of Q1.

## 4. Flash, SRAM, SDRAM and CPU (estimates)

| Slice | Flash | SRAM |
|---|---|---|
| S1 format + catalogue | about 1.6 KiB | 0 |
| S2 loader + gate + host table + `diag ptfx` | about 2.8 KiB (gate and hooks 0.9, loader 0.7, table and math 1.0, diag 0.2) | about 200 B (gate context, quarantine) |
| S3 run job | about 2.0 KiB | about 0.1 KiB |
| S4 PLUGINS page | about 2.5 KiB | about 80 B (view state) |
| S5 stereo | about 0.6 KiB | 0 |
| **Total** | **about 9.5 KiB** | **about 0.4 KiB** |

- **SDRAM:** the 768 KiB `ptfx` region, from regions unused today (A3).
- **CPU per audio block:** 0 when no plugin runs, plus the SysTick check (a few cycles a millisecond). During a run: the plugin's SDRAM traffic and one exception frame on its stack per preemption, measured in S6.
- Each slice records its measured deltas in its commit.

## 5. Testing

- **Fixtures (S0).**
  - `tools/testdata/testdata_manifest.json` gains the eight SDK files of `tracker-ce@7fd6c2c` (`bundled: false`, MIT, raw URLs, SHA-256).
  - `tools/ptfx_fixtures.py` builds the six examples with the fetched official builder (run with `python3 -I`) and our toolchain into `build-fixtures/ptfx/`.
  - Our own test plugins (`tests/ptfx/*.c`, our code, against our own small ABI header written from 2.1) are built by our packer `tools/ptfx_pack.py` (flags as in 2.1, header from a JSON sidecar):
    - `gain` (integer, exact);
    - `gen_ramp` (generator);
    - `extra` (prepend and append);
    - `recorder` (logs every call's `samples`, NULL entries and `channels` into scratch);
    - `hang`;
    - `fault_scs` (reads SCB->CPUID unprivileged);
    - `fault_addr` (writes an unmapped address);
    - `cpsid` (masks interrupts, then loops);
    - `stackhog` (deep recursion);
    - `overcount`;
    - `clobber` (overwrites r4-r11, s16-s31, FPSCR and returns).
  - Nothing built is committed.
- **Host (doctest).**
  - `tests/ptfx_format_test.cpp`: synthetic headers built byte by byte, one per refusal, clamp and sanitising rule of 3.1; list strings at the payload's edge.
  - `tests/ptfx_catalog_test.cpp`: 0, 1, 128 and 129 files; case-insensitive extension; long names skipped and counted; a damaged file listed with its status; the sort order; a card error retried.
  - `tests/ptfx_host_math_test.cpp`: every table function against the host's libm over audio ranges and special values.
  - `tests/ptfx_job_test.cpp`, native stand-ins through the host gate:
    - the `recorder` call sequence equals the sequence of 2.2, written from the spec, not from tracker-ce code;
    - extra samples, early stop, overcount, generator trim, `Gen NN`, the wavetable play mode;
    - cancel; undo restores the bytes (`memcmp`);
    - the stereo cases of 3.6, including the L/R/M/S round trip.
  - `tests/sample_edit_screen_test.cpp`: the page, its keys and rows.
- **SDK examples as host oracles.** When the fixtures are fetched, the six sources are compiled natively against the fetched header and run through the same job with our table. Their checksums are recorded for the QEMU and unit comparisons. The run is skipped when the fixtures are absent; `O3_CI=1` makes that a failure, as for modules.
- **QEMU (`qemu_ptfx_*`, `tools/ci.sh qemu`).** On mps2-an386 (Cortex-M4F; no alias or LMEM there, so a plain RAM block stands in for the region), the real loader and gate run:
  - Our test plugins: `gain` and `recorder` match the host exactly.
  - The six SDK fixtures match the host oracle within 2 LSB.
  - `hang` and `cpsid` give `Timeout` within 2.1 s. `cpsid`'s mask has no effect unprivileged.
  - `fault_scs` and `fault_addr` give `Fault`, with CFSR and the PC recorded.
  - `stackhog` gives `StackOverflow` or `Fault`.
  - `overcount` fails the run.
  - `clobber` leaves the host's registers and FPSCR as they were.
  - After each, a canary in the test's own memory is intact and the next `gain` run is still exact.
- **Unit (S6, `platform/mk66/tools/hil/qa_ptfx.json`).** The six SDK fixtures and `hang` / `fault_scs` go on the card (the tester; `/Effects`). Then:
  - Each runs from the console (S3) and from the page (S4); the result checksums equal QEMU's.
  - `hang` times out within 3 s, and the unit stays responsive with playback running.
  - `fault_scs` is contained, with no reset (R3: `fault_periph`, reading SIM_SDID, tells whether peripherals fault unprivileged).
  - The render's worst block during a Stereo Delay run with a project playing (`diag cpu`).
  - DWT cycles per frame of Multi-Mode Filter from the alias (R5).
  - A host or QEMU test is not hardware verification. Until S6 passes, nothing of this is claimed to work on the Tracker.

## 6. tracker-ce's built-in effects (an option for the maintainer, not a slice unless chosen in Q6)

tracker-ce compiles 16 effect classes (17 list entries; time stretch has two modes) into its firmware: `tracker-ce@7fd6c2c:core/effector/mtSampleEditorEngine.h:5-28`. They are not `.ptfx` files. Ours already covers:

| tracker-ce | Ours |
|---|---|
| Normalizer | NORM |
| Reverse | REVERSE |
| Fade In / Fade Out | FADE IN / FADE OUT |
| Amplifier | AMP |
| Compressor | COMPRESS (our own) |
| EQ, in part | FILTER (one biquad; tracker-ce's EQ has three bands, 9 parameters) |

We also have CROP (keep the selection), where tracker-ce's Cut deletes it. We lack:
- Overdrive
- Delay
- Bitcrusher
- Chorus
- Flanger
- Limiter
- the three-band EQ
- Wavetable Smoother
- Time-stretch (beat, note)
- Cut (delete the range)

| Option | What | Flash | Licence | Notes |
|---|---|---|---|---|
| (a) None | — | 0 | — | Users can drop `.ptfx` equivalents if the community makes them |
| (b) Rebuild as `.ptfx` | Port each effect's DSP (MIT component) into one C file against the SDK header. Sources go in our repo, under `modules/ptfx/`, with the notice. CI builds the `.ptfx` files as release assets, and users copy them to `/Effects`. | 0 | Approval per named component, notice in `THIRD_PARTY.md` | Each about 0.5 to 3 KB on the card (estimated from the source sizes: 100 to 380 lines each). They also run on tracker-ce. Overdrive, Delay, Bitcrusher, Chorus, Flanger, Limiter: about 1 to 2 days together. Time-stretch is the largest (378 lines, it uses tracker-ce's `AudioStream`). |
| (c) Port into our Sample Editor | New `EditOp`s in `app/sample_dsp`, under our job protocol (mono and stereo blocks), float only, with rows on the FX page | About 0.5 to 2 KiB each plus about 0.3 KiB of UI each: about 10 to 15 KiB for the ten we lack | As (b) | Costs flash we do not have |
| (d) As `.tmod` | — | — | — | Not suitable: a kind-2 module is a live insert, and an offline kind would be a new ABI (Level C) |

## 7. Slices

Each slice is one branch and one commit per slice ID. The QA rows go to `docs/qa/catalogue/sample_edit.md`; their IDs are proposed and assigned in merge order, never reused.

**S0 `OS-CEPTFX-S0` fixtures and packer.** S, Level A (isolated tooling).
- **Files:** `tools/testdata/testdata_manifest.json` (eight entries), `tools/ptfx_fixtures.py`, `tools/ptfx_pack.py`, `tools/ptfx_inspect.py` (prints a header and the 3.1 checks, for people and the HIL), `tests/ptfx/` (sources and sidecars of our test plugins, and our ABI header).
- **Acceptance:** `testdata.py fetch` brings the eight files with matching hashes; the six examples and our plugins build into `build-fixtures/ptfx/`; `ptfx_inspect.py` reads all of them back.
- **Tests:** a ctest that inspects the built files (skipped without the fixtures or the cross compiler; `O3_CI=1` fails).
- **QA rows:** none. **Flash / SRAM:** 0.

**S1 `OS-CEPTFX-S1` format and catalogue.** S, Level B (a parser of untrusted files, no execution).
- **Files:** `module/include/o3os/module/ptfx_format.h`, `module/src/ptfx_format.cpp`, `module/include/o3os/module/ptfx_catalog.h`, `module/src/ptfx_catalog.cpp`, `module/CMakeLists.txt`, `tests/ptfx_format_test.cpp`, `tests/ptfx_catalog_test.cpp`.
- **Acceptance:** every rule of 3.1 and A14 has a test.
- **Tests:** host.
- **QA rows:** none yet.
- **Flash:** about 1.6 KiB once linked (S2 links it). **SRAM:** 0.

**S2 `OS-CEPTFX-S2` region, loader, gate, host table.** M, Level C.
- **Files:**
  - `platform/mk66/include/o3os/platform/mk66/mem_map.h` (the `ptfx` region), `sdram_view.h` (the list), `tests/mk66_*` (region tests);
  - `hal/include/o3os/hal/code_gate.h`;
  - the shared Cortex-M4F gate source and its hooks in the SVCall, SysTick and fault stubs (`platform/mk66/src/startup.cpp`, `clock.cpp`; the QEMU twins in `platform/qemu/src/`), `irq_map.h` (SVCall role);
  - `module/.../ptfx_loader.{h,cpp}` (stepped load, sync, entries);
  - `core/fast_math` (sin and cos moved from `audio/src/module_voices.cpp`, the new functions) and the host table;
  - the quarantine set;
  - `diag ptfx list | info <n> | quarantine` in the console;
  - `platform/qemu/tests/ptfx_gate_test.cpp`, `tests/ptfx_host_math_test.cpp`;
  - ADR 0002 amendment 6.
- **Acceptance:** every QEMU case of section 5 passes. The module host's sin and cos give the same results as before (the module tests unchanged). The region test checks contiguity and the alias view.
- **Tests:** host, `tools/ci.sh qemu`.
- **QA rows:** none user-visible (console only).
- **Flash:** about 2.8 KiB. **SRAM:** about 200 B. Needs a risk review plus an independent review.

**S3 `OS-CEPTFX-S3` the run.** M, Level C (foreign code writes sample data through the edit protocol).
- **Files:** `app/include/o3os/app/ptfx_job.h`, `app/src/ptfx_job.cpp`, the App's start and end of edit (`app/src/app.cpp`, beside the existing edit path), `core/include/o3os/core/job.h` (`long_step()`), `diag ptfx run <n>` (the instrument in view, the selection), `tests/ptfx_job_test.cpp`.
- **Acceptance:** 3.5 for mono samples. The sequence test equals 2.2. One undo step; F8 restores the bytes. A stereo sample with an effect plugin says `STEREO: MAKE MONO FIRST`.
- **Tests:** host, plus the SDK oracle when fetched, plus QEMU, which runs `gain` through the job.
- **QA rows:**
  - QA-SED-027: `diag ptfx run` applies a plugin to the selection.
  - QA-SED-028: a hung plugin times out and is quarantined; the unit stays alive.
  - QA-SED-029: a crashing plugin is contained with the warning.
- **Flash:** about 2.0 KiB. **SRAM:** about 0.1 KiB.

**S4 `OS-CEPTFX-S4` the PLUGINS page.** M, Level B.
- **Files:** `app/include/o3os/app/screens/sample_edit_screen.h`, `app/src/screens/sample_edit_screen.cpp`, `app/include/o3os/app/sample_edit_state.h`, `app/src/sample_edit_actions.cpp`, `app/src/bindings.cpp`, `tests/sample_edit_screen_test.cpp`, `sim/scenarios/qa/qa_sample_edit.txt` and goldens (the catalogue from synthetic header-only files made by the test; runs through registered stand-ins), `docs/ui/ui-map.md`, `docs/manual/sample_edit.md` (a "Plugins" section: what is new, the `/Effects` folder, the tags, the messages).
- **Acceptance:** 3.7.
- **Tests:** host, sim goldens.
- **QA rows:**
  - QA-SED-030: open the page; the list shows `/Effects` with tags.
  - QA-SED-031: a refused file is greyed with its reason.
  - QA-SED-032: float, integer and list parameters by encoder, Shift and Delete.
  - QA-SED-033: APPLY on the selection, F8 undoes.
  - QA-SED-034: APPLY with no selection acts on all of the sample.
  - QA-SED-035: a generator on an empty slot makes `Gen NN`.
  - QA-SED-036: a generator replaces a sample.
  - QA-SED-037: WAVETABLE sets the play mode.
  - QA-SED-038: Add Silence lengthens the sample, and the points follow.
  - QA-SED-039: CANCEL while running.
  - QA-SED-040: RESCAN finds a new file.
  - QA-SED-041: an `IMX_ONLY` file says `NOT FOR THIS TRACKER`.
  - QA-SED-042: DEFAULTS.
- **Flash:** about 2.5 KiB. **SRAM:** about 80 B.

**S5 `OS-CEPTFX-S5` stereo** (if Q4 a). S, Level B.
- **Files:** `app/src/ptfx_job.cpp`, `tests/ptfx_job_test.cpp`.
- **Acceptance:** the table of 3.6. A stereo sample through a plugin that changes nothing comes back as the importer made it.
- **Tests:** host, QEMU (Multi-Mode Filter with `channels = 2` against the oracle).
- **QA rows:**
  - QA-SED-043: a STEREO plugin on a stereo sample.
  - QA-SED-044: a mono-only plugin on a stereo sample runs on both sides.
  - QA-SED-045: Stereo to Mono makes the sample mono; F8 brings back the stereo.
- **Flash:** about 0.6 KiB. **SRAM:** 0.

**S6 `OS-CEPTFX-S6` unit verification.** S, Level C (hardware).
- **Files:** `platform/mk66/tools/hil/qa_ptfx.json`, the QA rows' unit columns, `module/sdk/README.md` (a `.ptfx` row in "What is real"), `docs/architecture/cpu-budget.md` (the measured numbers).
- **Acceptance:** the unit checks of section 5 pass on a gated image; R3 and R5 answered.
- **QA rows:** the unit column of QA-SED-027..045.
- **Flash / SRAM:** 0.

## 8. Risks

1. **Untrusted native code.**
   - A plugin runs with the OS's memory open to it. Unprivileged mode protects the interrupt mask and the SCS, and perhaps the peripherals (R3), but not RAM. A plugin can corrupt the project or the undo journal before it faults.
   - Mitigations: Q1's trust model, containment (3.3), quarantine, the save-and-restart warning, and the stack layout that keeps an overflow inside the region.
   - This is ADR 0002's model, not a sandbox.
2. **No relocations.** A plugin that reads an absolute pointer from its `.data` (a string table, a function table) reads or jumps into our flash. That is undefined behaviour, caught only if it faults. It is equally broken on tracker-ce, so working plugins do not do it. The format gives no way to detect it.
3. **Containment code is ISR and fault-path code.** A bug there can hang the unit. Level C: QEMU tests of every path, a risk review and an independent review.
4. **UI stalls** of up to one call (8,192 frames) per main-loop pass, bounded at 2 s. Live pads wait meanwhile; audio does not.
5. **Output differs slightly from tracker-ce** (R4): our own math, and other compiler output. It is documented as within a few LSB, never as bit-exact parity.
6. **Licence.**
   - S0-S6 copy nothing from tracker-ce. The format is described from facts with citations. The SDK files are fetched, untracked test fixtures (MIT). Users' plugins are their own; part 2 1(b) of tracker-ce's licence permits building and selling them.
   - Q6 (b) or (c) needs a named-component approval and a `THIRD_PARTY.md` notice per effect.
   - Part 2's no-clone clause concerns the complete tracker-ce application, not a component or the SDK.
7. **Flash:** about 9.5 KiB at about 94 % used. Each slice measures and reports its delta; S4's UI is the largest and the first to trim.
8. **SDRAM:** the `ptfx` region takes `file_job`, `lut` and 64 KiB of `ui_cache`. A later feature that wanted those regions must find room elsewhere (the slack, or the 32 KiB spare at the region's end).
9. **Upstream drift:** a later tracker-ce may add flags, raise `format_version` or extend the host table. We refuse unknown flags and versions (`NEEDS NEWER OS` / `VERSION n`); a plugin built for a longer host table would read past ours. When tracker-ce changes the SDK, re-check against its new commit (open a backlog row then).

## 9. State for implementer

- **Order:** S0 → S1 → S2 → S3 → S4 → S5 → S6. S0 and S1 can go in parallel with nothing else touched. S2 is the risky one: write ADR 0002 amendment 6 first, and have the gate reviewed before S3 builds on it. Level C needs a risk review plus one independent review for S2, S3 and S6, and one `firmware-reviewer` for S0, S1, S4 and S5.
- **Read first** (excerpts, not whole files):
  - this spec's section 2;
  - `tracker-ce@7fd6c2c:core/effector/mtEffectModular.cpp:92-225`, for the exact call sequence; describe it in our own code, never copy it;
  - `editor-fx-sdk/tracker_plugin.h:29-90`, for the offsets; write our own constants with `static_assert`s of the offsets;
  - `platform/mk66/src/startup.cpp:63-89` (the fault stub), `platform/mk66/src/clock.cpp` (SysTick), `sdram_view.h` (the single-alias rule), `app/src/app.cpp:2620-2700` (the edit start), `sample_bank.h:385-405`.
- **Gate details that are easy to get wrong:**
  - `set_param` takes its float in s0 (hard-float AAPCS).
  - PSP must be 8-byte aligned.
  - Clear the IT / ICI bits in a forged xPSR (Thumb bit only).
  - Clear FPCCR.LSPACT on every exit path.
  - The fault stub's `cpsid i` must not leak into the containment path (PRIMASK survives the exception return).
  - Write-1-to-clear CFSR and HFSR.
  - SVCall's priority: any level works from thread mode, but it must not be masked by a BASEPRI the main loop might hold around the call: never call the gate inside a critical section.
  - The gate state is `volatile`. The main loop is its only writer before the call; the handlers write only `reason` and the fault fields.
- **Not to do:**
  - no relocation attempts, and no heuristics on `.data`;
  - no switching the SYSMPU off;
  - no `/Effects` creation;
  - no saving of parameter values (a format change);
  - no running a plugin outside the job;
  - no `.ptfx` or ELF in git.
- **Fixtures:** `tools/testdata/testdata.py fetch` (S0). The tester copies `build-fixtures/ptfx/*.ptfx` and our `hang` / `fault_scs` builds to `/Effects` on the card for S6.
- **Size [est]:** flash about 9.5 KiB in all (S1 1.6, S2 2.8, S3 2.0, S4 2.5, S5 0.6); SRAM about 0.4 KiB; SDRAM 768 KiB from unused regions; CPU per audio block 0 outside a run.
