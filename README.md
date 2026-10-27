<p align="center">
  <img src="ui/assets/boot_logo.png" alt="track3r-os logo" width="300">
</p>

<h1 align="center">track3r-os</h1>

<p align="center"><b>A new operating system for the Polyend Tracker, written from scratch by the community.</b></p>

<p align="center">
  <a href="https://github.com/trak3r-os/track3r-os/actions/workflows/ci.yml"><img src="https://github.com/trak3r-os/track3r-os/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg" alt="License: GPL-3.0-or-later"></a>
  <a href="module/sdk/LICENSE"><img src="https://img.shields.io/badge/module%20SDK-MIT-green.svg" alt="Module SDK: MIT"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C.svg" alt="C++17">
  <img src="https://img.shields.io/badge/target-MK66%20Cortex--M4F-lightgrey.svg" alt="Target: MK66 Cortex-M4F">
  <a href="https://github.com/trak3r-os/track3r-os/releases/latest"><img src="https://img.shields.io/github/v/release/trak3r-os/track3r-os" alt="Latest release"></a>
</p>

<p align="center">
  <img src="docs/img/readme/pattern_typical.png" alt="The Pattern screen" width="720">
</p>

> **Unofficial.** track3r-os is unofficial community firmware for the Polyend Tracker. It is not affiliated with or endorsed by Polyend. "Polyend" and "Tracker" are used only to describe hardware compatibility. Installing third-party firmware is at your own risk; keep the official firmware at hand so you can go back.

## What is it?

track3r-os replaces the software that runs on the Tracker. You keep the hardware you know: the pads, the encoder, the screen and the keys. On top of it you get a tracker that keeps growing:
- **Your old projects open.** Projects saved by the original firmware still load.
- **It speaks the classic formats.** It imports MOD, S3M and MED songs.
- **You can add to it from the SD card.** New instruments, effects and small apps load as modules.
- **It is open.** Anyone can read the code, build it and improve it.

**v1.0.1** is the current release (v1.0.0 was the first). It runs on real hardware and is tested on a unit. The known issues are in the [release notes](docs/release/v1.0.1.md); what is open and what comes next is in [docs/roadmap/BACKLOG.md](docs/roadmap/BACKLOG.md).

## Screens

<table>
  <tr>
    <td align="center"><img src="docs/img/readme/song_typical.png" alt="Song" width="400"><br><sub><b>Song</b>: arrange your patterns</sub></td>
    <td align="center"><img src="docs/img/readme/params_env.png" alt="Instrument" width="400"><br><sub><b>Instrument</b>: envelopes, filter, LFO</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/img/readme/wave_wavetable.png" alt="Wavetable" width="400"><br><sub><b>Wave</b>: samples, slices and wavetables</sub></td>
    <td align="center"><img src="docs/img/readme/sampleedit_typical.png" alt="Sample editor" width="400"><br><sub><b>Sample edit</b>: cut, shape, convert</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/img/readme/mixer_typical.png" alt="Mixer" width="400"><br><sub><b>Mixer</b>: levels, sends, master limiter</sub></td>
    <td align="center"><img src="docs/img/readme/perform_typical.png" alt="Perform" width="400"><br><sub><b>Perform</b>: scenes and live effects</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/img/readme/perform_dj.png" alt="DJ page" width="400"><br><sub><b>DJ</b>: crossfade two groups of tracks, filter, stutter, scratch</sub></td>
    <td align="center"><img src="docs/img/readme/record_recording.png" alt="Record" width="400"><br><sub><b>Record</b>: sample from line in, mic, radio or the mix</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/img/readme/file_export_stems.png" alt="Export" width="400"><br><sub><b>File</b>: export WAV, one file per track</sub></td>
    <td align="center"><img src="docs/img/readme/modules_app.png" alt="An app running" width="400"><br><sub><b>Apps</b>: small tools that run on the device</sub></td>
  </tr>
  <tr>
    <td align="center" colspan="2">
      <img src="docs/img/readme/theme_black.png" alt="Black theme" width="196">
      <img src="docs/img/readme/theme_white.png" alt="White theme" width="196"><br><sub><b>Themes</b>: Amiga, Black and White</sub>
    </td>
  </tr>
</table>

## Features

### Make patterns
- **12 tracks.** Each step holds a note, an instrument, a volume and **two effects**. Patterns run up to 128 steps, and the song can chain up to 255 positions.
- **More than 60 effects:** the ones the official manual documents, plus classic tracker effects (arpeggio, slides, retrigger, tremor, channel and global volume, panning), chords, arpeggios, swing, gate length, glide, overdrive and bit depth.
- **Three ways to record:**
  - **WATCH** just follows the song.
  - **STEP** writes where the cursor is, with the song still playing.
  - **LIVE** writes where the music is, with a count-in, a metronome, quantize and microtiming.
- **Pads that play the way you want:** notes in a scale, piano layout, isomorphic layout, or one pad per track.
- **Pattern tools:** fill dialogs (including Euclidean rhythms), block select, copy and paste, and a **TOOLS** menu with export and the editing apps.
- **Undo and redo** for every edit, and names for tracks, patterns and the project.

### Shape sounds
- **64 instruments**, each one of three kinds:
  - **sample instruments**, with one shot, loop, slices, beat slices, wavetable and granular playback, on a modern engine or an Amiga-style one;
  - **MIDI instruments**, which play your other gear;
  - **module instruments**, loaded from the card.
- **Built-in synth modules:** a TB-303-style bass line, an FM synth and a chip-tune voice.
- **Per instrument:** filter, envelopes, LFO, glide, overdrive and bit depth.
- **Samples in any common format and rate:** WAV, AIFF and IFF 8SVX, at any sample rate, always in tune. On a stereo file you choose left, right or both.

### Mix and perform
- **Mixer:** levels, mute and solo, a delay send and a reverb send per track, and a master limiter.
- **Perform screen:** scenes you can store and recall, plus live effects.
- **DJ page:** put some tracks in group A and some in group B, then mix them live with a crossfader, a filter, band kills, stutters, a scratch and tape stop, and move to the next pattern from the pads. Nothing is written into the song. Open Perform, then press `Perform` again ([manual](docs/manual/perform_dj.md)).
- **Record screen:** sample from line in, the microphone, the FM radio or the Tracker's own output (resampling).

### Files and compatibility
- **Projects:** open, save, save as and new, with a list of recent projects. Deleting is safe: a deleted project is only hidden, so you can get it back from a computer.
- **Projects of the original firmware open**, so your old work comes with you.
- **Import MOD, S3M and MED songs**, with a report of what was kept and what changed.
- **Export to WAV:** the song, one pattern or a selection, or **stems** (one file per track, plus delay, reverb and the full mix), ready for your DAW.

### Modules and apps
- **Extend the Tracker from the SD card** with `.tmod` modules: instruments, effects, file importers and **apps**.
- **Apps:** tap tempo and BPM calculator, MIDI monitor, loudness meter, tuner, scope and spectrum, rhythm generator (Euclidean, polyrhythms, probability), chord progressions, humanize, mutate, morph and MIDI file export. They are a separate download: the `apps-v1.0.0` release.
- **Every app edit is undoable**, and apps can never touch your files.
- **Write your own:** the module SDK is MIT-licensed and documented in [docs/guides/app-modules.md](docs/guides/app-modules.md).

### Connections
- **USB MIDI and USB audio**, including multitrack audio to your computer.
- **MIDI DIN** in and out.
- **MIDI clock** in and out.
- **MIDI learn** for parameters.

### Under the hood
- **A headless simulator** draws every screen. The screenshots on this page come from it, and so do the tests of the user interface.
- **More than 3,000 automated test cases**, run on a computer, in an emulator (QEMU) and on the real hardware.
- **CPU headroom is measured on the unit** before a feature is merged.

## Get started

- **Download:** the [latest release](https://github.com/trak3r-os/track3r-os/releases/latest) has the firmware file (`track3r-os-v1.0.1.ptf`) and its checksum. The app modules are the [`apps-v1.0.0`](https://github.com/trak3r-os/track3r-os/releases/tag/apps-v1.0.0) release.
- **Install:** read the [install guide](docs/manual/install.md) first. It covers installing from the official firmware, updating, and going back.
- **Learn it:** the [user manual](docs/manual/index.md), one chapter per screen.

## First steps

New to track3r-os? Start here: these are the keys worth learning first. The [quick start](docs/manual/quick_start.md) and the [cheat sheet](docs/manual/cheat_sheet.md) have more.

| To | Do this |
|---|---|
| **get help on any screen** | `Shift` + `Config` opens the help page of the screen you are on. `Left` and `Right` turn the pages, `Up`, `Down` and the encoder scroll, any other key closes it. |
| **change the volume** | Hold `Master` and turn the encoder, on any screen. |
| **change the tempo** | Hold `Pattern` and turn the encoder, on any screen: one BPM a click, a tenth of a BPM with `Shift` held first. Tap pads while you hold `Pattern` to tap the tempo. |
| **jump to any screen** | `Shift` + `Pattern` opens the Navigator, a list of every screen. |
| **look at the song** | Hold `Song`. Let go and you are back where you were. |
| **write notes** | `Rec` turns step entry on; `Shift` + `Rec` turns live recording on. |
| **switch patterns while playing** | On Pattern, hold `F1` and pick a pattern. It takes over at the next step, with no stop. |
| **learn a MIDI control** | Press `Vol` and `Fx` together. |
| **open a screen's second page** | Press its button again: the DJ page of Perform, the FX page of Sample edit. |
| **change the look** | `Shift` + `Master` steps through the themes: Amiga, Black and White. |
| **take a screenshot** | `Shift` + `Power` saves the screen to the card as a picture. |

**Save before you switch off.** track3r-os never saves by itself, not even when you turn it off. Press `F4` (**SAVE**) on the File screen first. To turn the unit off, hold `Power` for 2 seconds; if something is not saved, the strip warns **UNSAVED WORK** while you hold. Projects saved by track3r-os may not open on the official firmware yet (not verified), so keep a copy of your old projects.

**Go Amiga.** track3r-os is a tribute to the Amiga as much as to the Tracker:
- **Load Amiga samples.** Sample load reads IFF 8SVX files, next to WAV and AIFF.
- **Play them like Paula.** The **PLAY ENGINE** row on Instrument picks **Modern**, **Amiga**, **12-bit** or **8-bit**. The Amiga engine plays a sample the way the Amiga's sound chip, Paula, did: it holds each value until the next one, and uses 8 bits of it. **Auto** picks it for a one-shot or forward-loop sample at the Paula rate.
- **Convert to an Amiga rate.** On Sample edit, `Shift` + `F1` converts the sample to 8,287 Hz and `Shift` + `F2` to 16,574 Hz; `Shift` + `F3` turns 8-bit on for the next conversion. The sample keeps its pitch, and at 8,287 Hz it needs about a fifth of the memory it took at 44.1 kHz.
- **Import the songs.** On File, `F6` (**IMPORT**) brings in MOD, S3M and MED songs, with a report of what was kept and what changed.

**Sample memory.** About 10 MB holds the samples of a project: about two minutes of mono 16-bit sound at 44.1 kHz. A sample keeps its own rate, so a lower rate takes less room. Sample load shows the used and free KB.

**Record a loop.** On the Record screen you can record a loop of 1 to 16 bars that plays in time with the pattern, and add layers on top ([manual](docs/manual/record.md)).

**At power-on.** Hold these keys while you turn the unit on, until the strip shows the mode:
- `Shift`, `F1` and `F7`: the card shows on your computer as a disk. The **CARD AS DISK** row in Config does the same while the unit keeps playing; the computer can only read the card unless you turn **READ ONLY** off first.
- `Shift`, `F1` and `F8`: Input test, which shows every key and pad you press. If a key or a pad does not answer, run it and add what you see to your bug report.

## Build it yourself

You need CMake 3.20 or newer and a C++17 compiler. For the device you also need the **Arm GNU Toolchain 15.3.Rel1**, and QEMU (`qemu-system-arm`) for the emulator tests. Details are in [docs/toolchain.md](docs/toolchain.md).

```sh
# Host build and tests
cmake -S . -B build-host
cmake --build build-host -j4
ctest --test-dir build-host -j4

# Everything (host, emulator, device image), the way CI runs it
export ARM_GNU_ROOT=/path/to/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi
tools/ci.sh host
tools/ci.sh qemu
tools/ci.sh mk66        # builds the device image, platform/mk66/o3os.hex
```

If you don't have the toolchain yet, `tools/ci.sh toolchain` downloads it, checks its signature and installs it into `.toolchains/`.

<details>
<summary>Repository layout</summary>

| Path | What |
|---|---|
| `app/` | screens, actions and key bindings |
| `audio/` | audio engine, voices, effects |
| `core/`, `model/`, `seq/` | events and actions; the project model, editor and undo; the sequencer |
| `formats/` | the native project format, original-firmware projects, MOD/S3M/MED |
| `module/`, `modules/` | the module host and SDK; built-in and example modules |
| `platform/` | the MK66 board, QEMU, the update path, hardware-in-the-loop tools |
| `sim/` | the headless simulator and its scenarios |
| `ui/`, `eve/` | widgets, themes and assets; the FT812 display driver |
| `midi/`, `usb/`, `hal/` | MIDI, the USB device stack, hardware interfaces |
| `tests/` | host tests |
| `docs/` | specs, ADRs, file formats, UI map, QA catalogue, guides, user manual |

</details>

## Documentation

- [Install guide](docs/manual/install.md) and [user manual](docs/manual/index.md)
- [Writing modules and apps](docs/guides/app-modules.md), [module SDK](docs/guides/module-sdk.md)
- [File formats](docs/formats/), [design decisions (ADRs)](docs/adr/), [feature specs](docs/specs/)
- [QA catalogue](docs/qa/catalogue/README.md): every function, its key, the expected result and its test

## Contributing

Read [CLAUDE.md](CLAUDE.md). It is the working agreement for people and AI agents alike: hard rules, risk levels, tests and branches. In short:
- one branch per change, starting from `dev`;
- tests for pure logic;
- no change to a saved file format without a version bump and a round-trip test;
- no code or assets taken from the official firmware; its on-screen labels and file formats are used for compatibility, and every claim about its behaviour must be marked as observed (with its evidence), documented in the official manual, or an open research question.

## Licence

**GPL-3.0-or-later** (see [LICENSE](LICENSE)). The module SDK in `module/sdk/` is **MIT**, so modules can use any licence. Third-party components and their licences are listed in [THIRD_PARTY.md](THIRD_PARTY.md). Every binary release names the source revision it was built from.

Authors: see [AUTHORS](AUTHORS).
