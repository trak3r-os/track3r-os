# User manual

Status: **being written.** One chapter per screen, built from the QA catalogue (`docs/qa/catalogue/`), the screen specs (`docs/specs/`) and screenshots rendered by the simulator, so the manual follows the UI as it changes.

**Installing, updating and going back to the official firmware: [Installing track3r-os](install.md).**

## What's new in v1.2.0

- Eleven effects for one track: [Insert effects](insert_effects.md), with presets and the processing guard in [Mixer](mixer.md).
- Chip: eight more waves and ten more rows: [Chip](module-sounds.md).
- Your own wavetable contours from the card: [Instrument](instrument.md).
- Installing, the importer files and going back: [Installing track3r-os](install.md).
- The full list is in the [release notes](../release/v1.2.0.md).

## Chapters (planned)

Written by one cheap-model session per chapter, from the rules in `.claude/skills/manual-writing/SKILL.md`. Words per chapter are capped there.

| Chapter | Screen | Source rows |
|---|---|---|
| [Quick start](quick_start.md) | load a sample, write four notes, play, change the volume and save, in about ten minutes | `panel.md`, `sample_load.md`, `pattern.md`, `mixer.md`, `file.md` |
| [Getting started](getting_started.md) | install, first boot, the keys and the screen layout | `global.md` |
| [The panel](panel.md) | the panel drawing and the key map: where each key is and what it does | `global.md`, `bindings.cpp`, `docs/qa/catalogue/*.md` |
| [Cheat sheet](cheat_sheet.md) | the keys on one page: global keys, Pattern editing, the Shift combinations, one line per screen | `panel.md`, `pattern.md`, `instrument.md`, `sample_load.md`, `mixer.md`, `file.md` |
| [Pattern](pattern.md) | step editing, WATCH / STEP / LIVE recording, pads, TOOLS | `pattern.md` |
| [Effects in the pattern](fx.md) | the Fx column: effects, their values, the FX list | `pattern.md` |
| [Fill and tools](pattern_tools.md) | the Fill dialog, and the TOOLS menu | `pattern.md` |
| [Export](export.md) | WAV files of the song, a pattern, a block and stems; OG PROJECT (experimental) | `file.md`, `pattern.md` |
| Song | order list, pattern names | `song.md` |
| Instrument | sample, MIDI and module instruments, play modes, envelopes, LFO | `instrument.md` |
| Wave and Chop | sample view, slices, wavetables | `wave.md` |
| Sample load | browsing, formats, channel choice | `sample_load.md` |
| Sample edit | editing and printing effects | `sample_edit.md` |
| Record | sources, resampling, radio | `record.md` |
| Mixer | levels, mute/solo, sends, master limiter | `mixer.md` |
| Perform | the twelve effect slots, scenes and live effects | `perform.md` |
| [Perform: the DJ page](perform_dj.md) | crossfade two groups of tracks, filter, kills, stutters, transitions, scratch | `perform.md` |
| File | projects, import, export (WAV, stems), recents | `file.md` |
| [USB disk](usb_disk.md) | the card on a computer: at power-on, or from Config while the unit plays; eject and the refusals | `file.md`, `config.md` |
| Modules and apps | installing `.tmod` files, the bundled apps | `modules.md` |
| [App pages](module_app.md) | what every app page shares: the frame, `F8` BACK, `Play`, the help page | `module_app.md` |
| [TB-303, FM and Chip: new parameters](module-sounds.md) | the TB-303's MODS page, FM's sixteen algorithms and wide Detune, the Chip's learnable wave and duty, and its eight extra waves | `modules.md` (`QA-MOD-140` to `QA-MOD-156`) |
| [Step FX on module instruments](module-instrument-fx.md) | which FX a module instrument obeys (short section, to be folded into Instrument and Pattern) | `modules.md` (`QA-MOD-099`) |
| [Insert effects](insert_effects.md) | the eleven track insert effects (Drive, Crush, Biquad, Fold, Env Filter, Phaser, Comp, Ping-Pong, Wide, Chorus, Autopan) | insert_effects.md |
| Config | MIDI, display, SD, USB, pattern settings | `config.md` |
| [Input test](input_test.md) | the hidden screen that counts every key, pad and turn | `input_test.md` |
| [Computer tools](computer_tools.md) | the USB console app on a computer (`o3console`): connect, take screenshots, send and install firmware through the documented `fw` path, read the diagnostics (`diag cpu`, `mem`, `sd` ...), inject keys; also in the LLM guide, so an assistant or a tool can drive the unit | `platform/mk66/tools/` (`o3console.py`, `o3hil.py`, `o3audio.py`), `usb/include/.../command_table.h`, `docs/guides/` |
| Glossary | every term, one line each (`glossary.md`, grows with the chapters) | all |

### Writing order

Stable screens first, so the method is proved on small chapters before the big ones: **Mixer, Song, Config, Sample load, Sample edit, Wave and Chop, File, Perform, Modules and apps**; then **Record**; then **Pattern, Instrument** (each in two or three sub-pages) last, because they change the most. **Getting started**, then **Quick start** and the **Cheat sheet** come once Mixer, Song and a first Pattern page exist. The Glossary is edited by every chapter.

## Pictures

Screenshots come from the simulator (`o3sim` scenario snaps and the gallery) and are listed in the manifest `images.txt`; `tools/manual_images.py` renders them into `img/` (the PNGs are committed, so GitHub shows them; never edit one by hand, regenerate and commit). The panel is one schematic, `img/panel.svg`, of our own. The key-combination diagrams (`img/chord_*.svg`) are that panel with the keys of one combination marked; they are generated by `tools/manual_diagrams.py` from the manifest `diagrams.txt`, which takes the keys from the firmware's binding table and boot chord, so a diagram cannot drift from the firmware (never edit an SVG by hand).

## For assistants and tools

Generated by `tools/manual_llms.py` from the finished chapters (hidden comments stripped); never edited by hand. `tools/ci.sh host` fails when they are out of date. The catalogue and UI-map sections of the full guide (section 12 of the skill) are not built yet.

- `llms.txt`: a short index in the [llms.txt](https://llmstxt.org/) format: what the OS is, the notation, and a link with one line for each reference file.
- `llms-full.txt`: the full reference for a chat assistant or a tool that drives the unit: every screen, key and combination, modes and states, limits and the glossary.

## How a chapter is written

Each chapter follows [the template](chapter-template.md) and the [manual-writing skill](../../.claude/skills/manual-writing/SKILL.md). Every key table row carries the QA catalogue ID (for example `QA-PAT-012`) in a quiet `Ref` column, and steps carry it in a hidden comment, so the manual, the catalogue and the test battery stay in step.

## Keeping the manual current

Each chapter starts with `<!-- manual-sync: <sha> -->`, and each section that states behaviour carries a `<!-- sync: src=... -->` marker whose line in `docs/manual/sync.lock` records what it cited when it was last checked. `python3 tools/manual_check.py --stale` lists, per chapter, the QA rows added, changed or removed since that commit, and the sections whose sources moved; the branch gate (`--gate`, in `tools/ci.sh host`) fails a branch that changes a cited file and leaves the section alone; `python3 tools/manual_check.py` also reports broken links, chapters missing from this page, open TODOs and pictures with no manifest line. The update steps are in section 15 of the skill.
