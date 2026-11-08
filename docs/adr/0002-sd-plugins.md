# ADR 0002 — Extending the tracker from the SD card: data packs, native modules, scripts

Status: **Accepted** (maintainer, 2026-10-10: "Accept, open modules" — anyone can drop modules on the card, no signing; MIT module SDK; TB-303 first) · Maintainer direction: engines like the TB-303 should not be built into the firmware but live as files in a directory on the SD card, loaded by the OS when a project or instrument needs them. The 1 MiB flash holds only the core (HAL, sequencer, mixer, UI, loader). Clarified by the maintainer: "plugins" means instruments and effects like the 303, primarily — not just data.

## Context
- MK66: flash 1 MiB, SRAM 256 KiB (SRAM_L 64 KiB on the code bus, SRAM_U 192 KiB), SDRAM 16 MiB at 0x7000_0000, no OS.
- Code can run from SDRAM: 0x7000_0000/0x8000_0000 are executable [K66 RM]. But fetches through the system bus bypass the 8 KiB LMEM code cache and the SDRAM controller re-opens a row on every access, so uncached code runs an *estimated* 10–30× slower than SRAM. The RM/AN4745 describe a code-bus alias (0x0800_0000–0x0FFF_FFFF) that goes through the cache; using it means re-basing the SDRAM chip select. That has never been tried on this board.
- Prior art (BSD/MIT/Apache):
  - **Korg logue SDK.** Fixed-address units in SRAM with a versioned header; ELF shared objects on the newer M7 units.
  - **Expert Sleepers disting NT.** Relocatable plugins with a versioned MIT API. Each plugin declares its SRAM/DRAM needs.
  - **Playdate.** Moved from ELF relocation to a build-time relocation table and a tiny loader.
  - **Zephyr LLEXT, NuttX modules, elfloader.** Full ELF relocators.
  - **Berry and Lua.** Scripting VMs at about 40–110 KiB of flash.
- Today every fault spins forever and the watchdog is off, so one bad plugin would hang the device.

## Proposal
**SD layout (proposed):**
```
/modules/instruments/tb303.tmod      engines: 303, FM, chip, drum synth, Amiga sampler…
/modules/effects/ladder.tmod         filters, delays, reverbs, saturation…
/modules/importers/med.tmod          song importers (MED, MOD, XM, IT)
/modules/apps/…                      tools, games
/packs/…                             data packs (tier 0)
```
A project records which modules (name + ABI version) its instruments use; opening the project loads them from `/modules` into RAM. A missing module mutes that instrument and tells the user which file is missing. The project itself remains intact.

**Tier 1 — native modules: the primary goal.** Stored on the SD, run from RAM after loading. The first reference module is the **TB-303 voice**: it proves the ABI, the loader, placement and cost end to end. Details below.

**Tier 0 — data packs (in parallel, cheap).** Wavetables, presets, kits, scales, themes, fonts/bitmaps/icons for the FT812 (loaded into its 1 MiB RAM_G), help text, demo projects.
- A versioned manifest per pack; parsed outside real time.
- Zero code risk; moves bulky assets out of flash.

**Tier 1 details.** Compiled C/C++ for instruments, effects, filters, analysers, importers and apps.
- **Narrow C ABI.** The OS passes the module a function table (`host_api`). Modules export `init / render(block) / set_param / note_on / note_off / shutdown`. There is no symbol linking against the firmware.
- **Header.** Magic, target, kind (instrument/effect/importer/app), ABI major.minor (load if major equal and minor ≤ OS), parameters, and memory requests (SRAM hot state, SDRAM buffers, code placement).
- **Build.** A host tool converts the module ELF into a flat blob plus a relocation table, with ABS32 relocations only (the Playdate model). The loader is a few hundred lines with no ELF parsing on the device. Modules are float-only, with no libc, RTTI, exceptions or global constructors.
- **Placement.** The hot `render` code goes into an SRAM_L slab reserved by the OS. Cold code (UI, importers) runs from SDRAM, through the cached alias if measurement validates it. The OS reserves a module region in its own memory map; the new OS designs its SDRAM map from scratch.
- **Same source in the simulator.** The same module source builds as a host shared object against the same ABI, so modules are testable in V1/V4. The loader and relocation are tested on QEMU (V3).
- **SDK licence.** The ABI header and SDK are permissive (MIT proposed), so anyone can write modules whatever licence the firmware takes.

**Tier 2: scripts (later, optional).** Berry (MIT, <40 KiB, single-float option) or float32 Lua.
- Main loop only, with step/time budgets and the heap in SDRAM.
- For generative sequencing, MIDI processing, UI tools and importers.
- Never in the audio path.

**Containment (before Tier 1 ships).**
- Enable the Bus/Mem/Usage fault handlers. On a fault, write a crash record (CFSR, BFAR, PC, module ID) into the system register file that survives reset, then reset.
- A boot counter quarantines a module after repeated crashes. Holding a button at boot gives a safe mode with no modules.
- Watchdog backstop, plus a per-call cycle budget measured with DWT.
- SYSMPU can fence memory but not peripherals, so modules are *trusted community code*, not sandboxed.

## Options considered
| Option | Pros | Cons |
|---|---|---|
| Everything in core flash | simplest | 1 MiB fills quickly; every feature needs a reflash |
| Fixed-address slots (logue v1) | trivial loader | one slot layout forever; modules can't coexist flexibly |
| **Blob + relocation table (proposed)** | small loader, relocatable, proven (Playdate) | host build tool to maintain |
| Full ELF loader (LLEXT/elfloader) | standard toolchain output | larger loader, more relocation types, more failure modes |
| Scripts only | safe, easy to write | too slow for audio DSP |

## Validation (hardware, via `hw-session` bench)
1. The SDRAM code-bus alias works (chip-select re-base) and coexists with SDRAM data.
2. DWT cycles for a 303 voice and a biquad kernel running from flash, SRAM_L, uncached SDRAM, uncached alias and cached alias, at code sizes from 2 to 32 KiB.
3. Audio jitter and underruns with 8 voices streaming from SDRAM while a module executes from it.
4. Cache invalidation after loading a module.
5. SYSMPU faults and the reach of peripherals.
6. The register file survives watchdog and lockup resets.
7. Flash cost of the loader and ABI; Berry/Lua size and GC pauses at `-Os` with float32.

## Questions for the maintainer
1. Accept the plan: crash containment + loader + the TB-303 as first native module (data packs alongside), scripts later?
2. Trust model: anyone can drop a module on the card (open, recommended), or signed modules only?
3. MIT for the module SDK/ABI header, independent of the firmware licence?

## Amendment 1 — module screens (maintainer, 2026-10-12)
- **The OS owns every page; a module draws views inside it** (maintainer: views drawn by the module through a restricted drawing API (rectangles, text, curves) inside an existing page, for the 303 curve and for apps). The page frame stays the OS's: the status strip, the soft-key labels, the exit chord and the theme.
- **One restricted draw API for every module kind.** Rectangles, text in the theme's faces and curves (polylines and the `ui::plot` curve primitive), with colours from the theme's roles. No raw display-list or register access. It is versioned and passed through `host_api`.
- **Instrument and effect modules.** The OS generates their controls from the parameter descriptors in the module header (name, range, unit, learnable, modulation target), and MIDI Learn works on them with no module code (product-ux section 7, decision 10). A module may add one or more **views** to that page, such as the TB-303's filter/envelope curve, drawn by the module in a rectangle the page gives it.
- **App modules.** An app gets an OS page whose body is one module view. It receives input events (buttons, encoder, pads) while that page is open.
- **Value controls** (maintainer, 2026-10-12: with value controls). Every parameter a module declares, apps included, appears on its page as a standard OS control: a value field, slider or list with name, range and unit, edited with the encoder and keys. MIDI Learn, undo, the Perform layer and saving with the project work on it with no module code. A view receives the current parameter values when it draws, so the TB-303's curve follows the cutoff as it turns. An app may draw a control of its own and handle its input, but that control gets no Learn or undo; parameters are the recommended way.
- **Entry points.** An optional UI table for any kind: `view_draw(view, canvas)` for each declared view, plus `enter`, `input`, `exit` for apps. A module without it has no views.
- **Containment.** The OS caps each view's draw calls per frame and the time `view_draw` may take. A module over budget has its views blanked and is quarantined like a module that faults on load. The exit chord always works.
- **ABI.** ABI v1 (OS-012a) reserves the UI table and the draw-API slot in `host_api`, so adding the draw API (OS-012b) is a minor version, not a major one. OS-012b specifies the draw calls, view sizes and budgets.

## Amendment 2 — module screens as built (OS-012b, 2026-10-12)
Specifies what amendment 1 left to OS-012b. ABI **1.1** (a minor bump: every 1.0 module loads and runs unchanged). The SDK guide is `module/sdk/README.md`.
- **Draw API v1** (`o3m_draw_api`, `o3m_host.draw`, and `canvas->draw`): `rect`, `outline`, `text` (the theme's four faces, fixed pitch), `text_width`, `polyline` (2..128 int16 points) and `curve` (the `ui::plot` primitive: linear, log-frequency or dB axes, optional frame/grid/labels and fill). Colours are the theme's 16 roles by number. No raw display list, no register. A view gets an `o3m_canvas` (its size, the current value of every parameter, the draw table); an app also gets `enter`, `input(o3m_ui_event)`, `exit`.
- **Record, then replay.** A view's calls are checked, clipped (rectangles and text by the canvas; polylines by a scissor; curves by the plot) and recorded in fixed memory (`module/view.h`, about 5 KB in SDRAM); the page replays them only when the view returned inside its budgets, so an over-budget view is **blanked**, never half drawn.
- **Budgets**: per view and frame 96 draw calls, 256 points, 384 text characters, 4 ms (the clock is the platform's, checked at every draw call and at the return); and 700 *estimated* display-list words for all the views of a page together (the frame buffer is shared with the page: an overflow drops the whole frame). Over budget: the module's views are blocked for the boot, the module is quarantined (`Status::ViewOverrun`), its audio keeps running until the project changes. An app over budget is stopped, not called again (not even `exit`), and its page says so; the exit chord F7 + F8 is a binding of the OS and always works. A view that never returns and never draws cannot be preempted: the watchdog and the crash record (the platform stamps the running module's id; the next boot quarantines it) are the backstop, and the watchdog is still an open platform item.
- **Value controls.** A module's parameters become standard controls (`describe_module_param`: name upper-cased to 12 characters, unit text, whole-number or percent display, bool as OFF/ON, log curve, learnable unless `O3M_PARAM_NO_LEARN`, `O3M_PARAM_MOD` for a modulation target). Parameter `i` of a Module instrument is `ParamId {Instrument, slot, 0x100 + i}` whose value is the i-th float of the instrument's blob, which the native project format already stores: **no persistent format changed**. MIDI Learn bindings and undo work on them with no module code (the Learn store holds the same 32-bit ids; they follow the module's parameter order, so a published module never reorders its parameters).
- **Pages.** F1 ENGINE opens the picker, F2 to F5 are the pages (VOICE, FILTER, ENV, SENDS for a sample instrument; eight parameters a page for a module), F8 undo. MODULES is a cell of the Navigator with no button of its own. An app's page is captured: the app gets every control raw but the exit chord and the Shift layer of its parameters.
- **Engine picker.** Lists `/modules/instruments` and the project's modules the card lacks (greyed). The pick (one undo unit: the instrument and its default parameter blob; the project's module table grows by an unjournalled append that does not end the undo history) loads the module synchronously and binds an instance. A slot that already has an instance cannot change engine at run time (no `ModuleVoices::unbind`); undoing a pick restores the instrument but not the unbind.
- **What would be needed for the rest, not done**: (1) apps' parameters saved, learnable, undoable and in Perform: a place in the project (a per-project app record or the device settings) is a persistent-format change: version bump, loader of the old version, round-trip test; (2) unbinding an instance: the retire/quiesce protocol per instance in `ModuleVoices`; (3) effects run: an effect slot in the engine's graph; (4) a sliced picker load; (5) hardware validation of view time on the MK66.

## Amendment 3 — the module version in the file header (OS-MODVER, 2026-10-13)
Maintainer decision 2026-10-13: "a version field in the .tmod header", so that only a truly newer card copy replaces a built-in module (OS-012d). ABI **1.2** (a minor bump: every 1.0 and 1.1 file loads unchanged).
- **Header.** `o3m_file_header.module_version` (byte 36, the first of the three reserved words; the header stays 64 bytes and every other field keeps its offset): `O3M_VERSION(major, minor, patch)` = major << 16 | minor << 8 | patch (major 0..65535, minor and patch 0..255), 0 = unversioned. Below ABI 1.2 the word is reserved and must be 0 (a 1.0 or 1.1 file with a value there is still refused). The CRC covers it like the rest of the header. The descriptor, `o3m_host` and the project's module table are unchanged: the table records the stem and the ABI major.minor only, so **no project format changed**.
- **Rule.** A card copy replaces the built-in module of the same name only when it is valid, of the same kind and ABI major, and has a strictly higher version. Equal, older and unversioned (0) copies keep the built-in module and are not quarantined. This also removes the case where a same-minor card copy shadowed a fixed built-in module after a firmware update.
- **Tooling.** `o3m_pack.py --version X.Y.Z` (default 0.0.0 = unversioned; a version needs a module that declares ABI 1.2), `o3m_add_module(... VERSION X.Y.Z)`, `"abi": "1.2"` in the manifest. The TB-303, chip and FM are 1.0.0. The `mod` console lists versions and says when a card copy was passed over. The guide is `module/sdk/README.md`.
- **Reversible** until a release with ABI 1.2 files is published: before that the word is a reserved one that no 1.1 file uses.

## Amendment 4 — tier 2 scripts closed (maintainer, 2026-10-14)

The maintainer discarded tier 2 (Berry / Lua scripts) when accepting ADR 0014 (app host API v2): SD extensions are native modules (data packs, instruments, effects, importers, apps) only. App modules that read and edit the project use ADR 0014's typed read / edit tables.

## Amendment 5 — ABI 1.8: a pitch input, params pages and value labels (OS-MODPITCH + OS-INSTUI S10, 2026-10-16)

Maintainer answers 2026-10-16 (docs/specs/OS-MODPITCH.md): one additive minor shared by two features. Spec: `docs/specs/OS-MODPITCH.md`, `docs/specs/OS-INSTUI.md` S10. **New Community OS behaviour**, no persistent project or settings format change; the `.tmod` file header is unchanged.
- **Shape.** `o3m_module` grows from 64 to 80 bytes, four members appended after `view_count`: `pitch(void* state, float semitones, float per_frame)` (the host drives a note's pitch: Slide 41 / 42 and Glide 19 of a pattern; the module follows an offset from the note, host ramps), `pages` / `page_count` (at most 4 pages of 8 parameters, a name of at most 7 characters) and `labels` (`param_count` entries parallel to `params`; INT parameters only, 2..16 values, at most 11 characters a label). `o3m_page` and `o3m_labels` are 8 bytes each. New `note_on()` flag `O3M_NOTE_RAMP` (2), sent only to a module that exports `pitch`; with `O3M_NOTE_LEGATO` it means "Glide or Slide note: keep the envelope, put the base pitch on the note, no portamento of your own, follow `pitch()`".
- **Gating, per declared minor.** `struct_size` must be `O3M_MODULE_SIZE(minor)`: 64 up to 1.7, 80 from 1.8 (the loader refuses any other, `BadDescriptor`; `ModuleSet::add_native` the same). The members past byte 64 are read and validated only when the module declares 1.8, so a real 1.7 module (64-byte descriptor) loads and plays exactly as before and never receives `O3M_NOTE_RAMP`. A module rebuilt against the 1.8 header with a hand-written `sizeof(o3m_module)` and an older minor is refused on purpose; the generated headers and the templates write the size through the macro.
- **Limits** (published, validated at load): `pitch` is code or NULL; `pages` zero exactly when `page_count` is zero, at most 4, the first at parameter 0, strictly ascending, 1..8 parameters a page, together covering `param_count`, none for a module without parameters; labels only on an INT parameter whose `max - min + 1` is 2..16, every string inside the image and printable.
- **Reversible** until a release with ABI 1.8 files is published; the descriptor's tail is zero-filled in C++ (default member initialisers), so a descriptor written as the first 16 members still compiles under `-Wextra`.

## Amendment 6 — `.ptfx` plugins: a second native kind beside `.tmod` (OS-CEPTFX, 2026-11-05)

Spec: `docs/specs/OS-CEPTFX.md` (the maintainer approved it 2026-11-05: "I want to be able to use tracker-ce's effects: load them and use them"). A `.ptfx` file in `/Effects` on the card is a Sample Editor plugin built with tracker-ce's `editor-fx-sdk`: a foreign, position-independent Cortex-M4F binary with its own 472-byte header and ABI (spec 2.1), run offline on a sample. It is a **second native kind of SD code beside the `.tmod`**, with its own format, loader, region and gate; the `.tmod` loader is not reused as a whole (spec 3.8), only its parts (the code-sync hook, the id stamping, the stepped read pattern, `Arena`).
- **Trust model.** The maintainer's Q1 (a): any `.ptfx` in `/Effects` is listed and can run, like a `.tmod` — ADR 0002's open-modules decision, **the user's own files, trusted community code, no signing**. This is ADR 0002's model, not a sandbox: nothing fences RAM from a plugin (spec risk 1); unprivileged mode protects the interrupt mask and the System Control Space, nothing more. It does not fence the peripherals (SD host, flash controller, watchdog, DMA), and the SYSMPU is not used; containment covers timeouts and CPU faults only (whether a peripheral access faults in unprivileged mode is open research question R3). Containment (below) limits the damage of a bad file, it does not make one safe.
- **Placement.** A new SDRAM region `ptfx` of 768 KiB (spec A3), carved from the unused `file_job` (512 KiB) and `lut` (192 KiB) regions plus the first 64 KiB of `ui_cache` (which keeps 448 KiB, still holding the bench's scratch). Like every cached region, every core access to it goes through `core_view()` (the ICode alias), so plugin code runs cached; the image is **not relocated** (spec 3.2, risk 2: the format carries no relocation table, so an absolute pointer in `.data` stays a link-time offset and is broken here exactly as on tracker-ce — working plugins do not do it). The sub-areas of spec 3.2's table (image 32 KiB, stack guard 4 KiB, stack 12 KiB with its top at 48 KiB, stereo staging 32 KiB, scratch 640 KiB, catalogue/params/host table/job 16 KiB, spare 32 KiB) are named offsets of the region in `mem_map.h`.
- **The call gate.** A plugin runs in **unprivileged thread mode on its own stack (PSP) inside the `ptfx` region, and returns through an SVC gate** (spec A5, `hal/code_gate.h`): the host's callee-saved registers, FPSCR and MSP are saved and restored around every call, and SVCall gets the role `PluginGate`. The gate is written under S2b, not here.
- **Time limit.** **2,000 ms per call** (`kPtfxCallTimeoutMs`, spec A6, the maintainer's Q3 (a)), checked in the SysTick handler; plugins run unprivileged, so they cannot mask it.
- **Quarantine.** A plugin that faults or times out is quarantined **for the boot**, up to 16 plugins, each by an id `0x8000 |` a 15-bit FNV-1a of `"ptfx:" + file name` (spec A7) — a namespace distinct from the `.tmod` stems, so a plugin and a module of the same name never quarantine each other. The id is stamped with `set_module()` while a call runs; a crash record that names it quarantines it at the next boot.
- **What is not done.** No `/Effects` creation (browsing writes nothing to the card, spec A14); no saving of parameter values (a persistent-format change, spec "not to do"); no relocation attempts or heuristics on `.data`; no switching the SYSMPU off; no tracker-ce code in the repository (spec A1).
