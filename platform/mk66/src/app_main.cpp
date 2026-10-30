// SPDX-License-Identifier: GPL-3.0-or-later
//
// The image's composition root (OS-007c): the one file of platform/mk66 that includes the app and ui layers, which a platform
// file otherwise may not (tools/check_layers.py lists it as the exception). It builds the OS app over the real display and
// the SysTick clock and gives main.cpp two calls: init() once, pass() every pass of the main loop.
//
// What runs: the OS app (the pattern view over a fresh document since OS-009: pattern 1, one song position, in the SDRAM project region, its
// undo journal in the undo region; the default theme), a fixed frame tick of 16 ms from SysTick,
// the display driver's service() each pass, and (OS-007d) the input: the scan of the two key scanners and the encoder, whose events
// the app polls, and the pad LEDs, which follow the pads (a pad that is down lights at the theme's front level; on the INPUT TEST
// screen the others light at the back level, so the hardware session sees both levels). With -DO3OS_DEMO_CYCLE=ON the cursor walks
// down the pattern and the three themes cycle, so a hardware session can see the screen move.
//
// OS-003f, the boot logo: once the bitmaps are up, the first frame the display is given is the boot screen (ui/boot_screen.h: the
// maintainer's logo, "Track3r" and the build id under it), before the app exists on screen and so before the backlight ramp (the display
// driver lights the screen only behind the first committed frame). The app's first frame replaces it when it has been lit for
// core::kBootMinMs (800 ms) and the boot work has finished: the rest of main()'s boot (the audio bring-up blocks for about 0.6 s)
// runs under it, and nothing is waited for beyond that minimum. The audio start's waits (the codec's, boot::delay_ms(); the USB start
// does not wait with it) keep the display serviced through the idle hook of boot::delay_ms(), so the logo swaps in and the backlight
// ramps while the codec comes up. What decides when (the hook, the first light, the hold, the release) is BootGate (boot_gate.h,
// tested against the real driver); this file keeps the hardware's part.
//
// OS-005d lite, the sound: unless the image is the tone image (O3OS_AUDIO_TEST), the document gets the demo kit and its pattern when it has no samples (four
// sounds generated into the sample arena, four instruments, two bars at 120 BPM on tracks 1..4: audio/demo_kit.h, model/demo.h), the sequencer is built over
// the project and handed to the audio engine as its event source with the sampler voices (seq/engine_link.h), and the app's Play button is a transport on the
// sequencer's command ring (app::TransportSink). The outputs are muted at reset and main.cpp opens them once, at boot (OS-OUTON: after the project load, mk66::open_wanted); a Play or a note that finds them closed asks for them (audio_open_pending) and waits. The block's order of work
// is the engine's (audio/engine.h): the sequencer's step(), the voices at their sample offsets, then the graph.
//
// OS-005c2, MIDI: the sequencer is wrapped in a seq::MidiLink (seq/midi_link.h), which is the engine's event source: it feeds the MIDI timing core (the DIN emitter on
// UART4, the USB MIDI endpoint) with the MIDI events of every block and the sequencer with the transport and clock of the port it follows. The drivers (midi.h, usb0.h
// bind_midi) are started here, once, with the document. The device settings (CLOCK IN, CLOCK MODE, CLOCK SYNC, MIDI OUT, OUT CHANNEL; seq::sync_config_from_settings) drive the
// link's mode, followed port, PAR-119 correction, output ports and channel at boot and whenever one of them changes (sync_midi_settings, every pass of the main loop);
// the offsets, FOLLOW and the CC table stay the link's own (runtime only, no row in the settings file).
// OS-005g, the live notes: the pads, the MIDI keyboard (the notes of the same ring as MIDI Learn's controllers, midi::CcInput's second ring) and the previews reach the voices through
// a bounded SPSC queue (audio::LiveQueue, in SDRAM beside the control-change ring: the contract is the top of audio/live_notes.h). The main loop is its one producer (LivePreview,
// below, the app's PreviewSink), the render its one consumer (Engine::play_live, at the start of every block). The first live note before any Play asks for the outputs to
// open, like the Play button, and waits for them (OS-FU1: the latest such note is kept in LiveBox::pending and queued when they open, about 0.6 s later; a release that came
// meanwhile follows it).
//
// OS-013a, MIDI Learn: the control changes of both ports go from the render (MidiLink) through a small ring in SDRAM (midi::CcInput) to the main loop, which hands them
// to the app (poll_midi_cc: the `midi.cc` action binds or moves a parameter; MIDI IN and IN CHANNEL filter them there).
//
// OS-007e/f, the project: a load (boot: Workspace; `diag load <folder>`) goes through four phases of the main loop, none of them in the audio interrupt, with the
// app's frames held and the display still serviced: the transport is stopped and the voices are waited for (Settle), the OG readers fill the document (Project),
// the voices' table is cleared and every sample instrument's PCM goes from the card into the SDRAM sample arena and is bound to the voices (Samples,
// app/og_samples.h), and the result is logged ("boot: samples N loaded, M missing, K KB"). The demo kit is for when no project is loaded or the project has no
// sample instrument; loading a project replaces it and its pattern.
//
// OS-007g, "Play after a project load makes no sound": the sequence is run end to end on the host (tests/play_after_load_test.cpp: the demo, a load through the
// card stack, Play, over a small project and the maintainer's v1.9.0 Workspace) and sounds; what that test cannot see is the outputs' gate (platform/mk66/output_gate.h:
// the first Play waits for a clean run of the audio path, and any earlier fault used to shut it for good) and the hardware. The log now says what a Play did
// (log_play_probe) so that the next silence names its stage.
//
// OS-012a, SD modules (ADR 0002): an instrument of kind Module plays an engine that lives as a file on the card (modules/instruments/<name>.tmod, the TB-303 first). Opening
// a project (the phases above, a fourth one after Samples) loads the modules its instruments name into the SRAM_L slab (hot code) and the SDRAM module region (the rest, and
// each instance's state) and binds every Module instrument (app/module_binder.h); the audio engine hands those instruments' notes to audio::ModuleVoices and takes their blocks
// as the tracks' feeds. A module that cannot be had mutes its instruments and says why in the log; a file that fails validation is quarantined for the boot, and so is the module
// the crash record names (the platform stamps the module that is running into the crash context, fault_id()). Nothing of this is in SRAM_U: the module system is one object
// at the start of the module region, the slab holds its output blocks and the modules' hot code. `diag mod` lists, loads, binds and auditions.
//
// The hidden INPUT TEST screen (app/screens/input_test_screen.h) is opened by a boot chord: Shift + F1 + F8 held while the unit powers
// up (read after the key scanners have been configured: a poll of at most kBootChordWindowMs, boot_chord.h), or always, with -DO3OS_INPUT_TEST=ON (an image for
// the hardware session that needs no chord).
#include "internal.h"
#include "regs.h"

#include <o3os/app/app.h>
#include <o3os/app/brightness.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/project_state.h>
#include <o3os/app/metro_sync.h>
#include <o3os/app/usb_return.h>
#include <o3os/app/og_folder.h>
#include <o3os/app/module_binder.h>
#include <o3os/app/module_import.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/native_load.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/pad_board.h>
#include <o3os/app/pad_palette.h>
#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/app/state_line.h>
#include <o3os/app/track_state.h>
#include <o3os/audio/demo_kit.h>
#include <o3os/audio/diag.h>
#include <o3os/audio/events.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/hal/audio.h>
#include <o3os/hal/cycles.h>
#include <o3os/hal/display.h>
#include <o3os/hal/input.h>
#include <o3os/model/demo.h>
#include <o3os/model/editor.h>
#include <o3os/platform/mk66/blink.h>
#include <o3os/platform/mk66/audio.h>
#include <o3os/platform/mk66/capture.h>
#include <o3os/platform/mk66/boot_chord.h>
#include <o3os/platform/mk66/boot_gate.h>
#include <o3os/platform/mk66/disk_restart.h>
#include <o3os/platform/mk66/disk_share.h>
#include <o3os/platform/mk66/display/device.h>
#include <o3os/platform/mk66/input/device.h>
#include <o3os/platform/mk66/input/pad_bus_log.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/midi.h>
#include <o3os/platform/mk66/platform.h>
#include <o3os/platform/mk66/power/power_device.h>
#include <o3os/platform/mk66/sdram_box.h>
#include <o3os/platform/mk66/sdram_view.h>
#include <o3os/platform/mk66/sections.h>
#include <o3os/platform/mk66/save/fat_storage_writer.h>
#include <o3os/platform/mk66/storage/device.h>
#include <o3os/platform/mk66/update/fw_device.h>
#include <o3os/platform/mk66/usb/console.h>
#include <o3os/platform/mk66/usb/diag_text.h>
#include <o3os/platform/mk66/usb/screenshot_service.h>
#include <o3os/platform/mk66/usb/usb0.h>
#include <o3os/model/params.h>
#include <o3os/module/loader.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/midi_link.h>
#include <o3os/seq/offline.h>
#include <o3os/seq/sequencer.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/plot/curve.h>
#include <o3os/ui/plot/peak_cache.h>
#include <o3os/ui/plot/ram_g_map.h>
#include <o3os/ui/boot_screen.h>
#include <o3os/usb/identity.h>

#include "o3os_build_info.h"  // O3OS_BUILD_LABEL: the same string `diag version` prints (generated at build time)

#include <cstdint>
#include <cstring>
#include <new>

#ifndef O3OS_DEMO_CYCLE
#define O3OS_DEMO_CYCLE 0
#endif
#ifndef O3OS_INPUT_TEST
#define O3OS_INPUT_TEST 0
#endif
#ifndef O3OS_AUDIO_TEST
#define O3OS_AUDIO_TEST 0
#endif

namespace o3os::platform::mk66::appimg {
namespace {

constexpr std::uint32_t kFrameMs = 16;       // the app's tick: about 60 Hz, the display refreshes at 54 Hz
constexpr std::uint32_t kAssetRetries = 50;  // upload_assets() restarts from the first chunk; Busy is a display that is not ready
constexpr std::uint32_t kBootFrameRetries = 50;  // the boot frame is refused while a swap is pending: a millisecond between tries

// OS-MEM1: main loop only, so in the SDRAM slack window (28 KB off SRAM_U); zeroed before each construction, as its .bss was (see make_app).
O3OS_SDRAM_NOINIT alignas(app::App) unsigned char g_app_storage[sizeof(app::App)];  // the app lives for ever: built once, never destroyed
app::App* g_app = nullptr;
// OS-POWER P1: the power-off latch (set at stage A of the shutdown, never cleared: see internal.h power_off_latched) and the end of stage E (the display is dark and held, the chip in power-down).
// Main loop only; main.cpp reads the latch through power_off_latched() in the same thread.
bool g_power_latched = false;
bool g_display_off = false;

// The document (OS-009 D1): a fresh project in the SDRAM project region (a typed object placed there: mem_map.h keeps the region the size of
// the model's worst case), its undo journal in the undo region and the editor over them. Built once, at the first start; a recovery that
// starts the app over attaches the same document again (it holds the user's work, the app does not).
model::ModelArena* g_arena = nullptr;
model::UndoHistory g_journal;  // OS-UNDOGRP: three rings carved out of the one 384 KiB region
alignas(model::Editor) unsigned char g_editor_storage[sizeof(model::Editor)];
model::Editor* g_editor = nullptr;

#if !O3OS_AUDIO_TEST
// ---- The sound (OS-005d lite) --------------------------------------------------------------------------------------------------------
// The sequencer over the document (it reads the project in SDRAM from the render line, rows with their generation check), built once with the document, in
// SRAM_U .bss, and the events it hands the engine a block at a time. The sizes are the plan's: ADR 0013 D7's "sequencer" row is 8 KiB.
alignas(seq::Sequencer) unsigned char g_seq_storage[sizeof(seq::Sequencer)];
seq::Sequencer* g_seq = nullptr;
// E-SONG-07: the sequencer's copy of the song list, 656 B, in SDRAM so that SRAM_U keeps its 32 KiB free floor (render reads it once a pattern end and once an edit; attach_order_store zeroes it).
O3OS_SDRAM_NOINIT seq::OrderStore g_seq_order;
audio::SeqEvent g_events[audio::kMaxEventsPerBlock];
static_assert(sizeof(seq::Sequencer) <= mem::budget_bytes(mem::kSramUBudget, mem::kSramUBudgetCount, "sequencer") + 640u,  // (512 B of slack, +128 for OS-INSTUI S8b: the MIDI instrument's output state is 72 B: a program and the routes a bend went to for each channel, the route of the note a track sounds)
              "the sequencer outgrew its SRAM_U row (mem_map.h sequencer; the 2.3 KB event buffer is in the rings row)");

// OS-005c2: the MIDI link over the sequencer (the emitter's rings, USB out, the router, the two midiRt rings, the clock-follow loop: about 23 KB of SRAM_U, ADR 0013 D7's
// "rings" row and the slack Amendment 2 left; the cap keeps its growth visible), built with the sequencer, and the configuration the main loop last gave it.
alignas(seq::MidiLink) unsigned char g_midi_storage[sizeof(seq::MidiLink)];
seq::MidiLink* g_midi = nullptr;
static_assert(sizeof(seq::MidiLink) <= 24u * mem::kKiB, "the MIDI link outgrew the 24 KiB OS-005c2 measured: update the SRAM_U plan (mem_map.h) first");
midi::MidiConfig g_midi_cfg;                 // what the engine was last told (main loop only): the settings' fields are rewritten from the device settings, the rest is kept
std::uint32_t g_midi_clock_gen = 0;          // the sum of the generations of the device settings that drive the link, at the last sync_midi_settings()
bool g_midi_clock_seen = false;
app::UsbReturnSync g_usb_return;             // the device setting USB RETURN against the USB driver's return flag (app/usb_return.h)
app::UsbAudioSync g_usb_audio;               // OS-USBAUDIO: the device setting USB AUDIO against the USB driver's mode (the same sync; the driver's console changes are not copied back)
app::UsbReturnLevelSync g_usb_return_level;  // OS-006e: the device setting RETURN LEVEL against the engine's (app/usb_return.h)
app::MetroSync g_metro_sync;                 // OS-METRO: the device settings METRO and METRO VOL against the engine's and the sequencer's (app/metro_sync.h)
midi::CcInput* g_cc = nullptr;               // MIDI Learn's control changes (OS-013a) and the notes in (OS-005g): in SDRAM, behind the sample arena's other tail parts
audio::LiveBox* g_live = nullptr;            // the live notes (OS-005g): the queue and the render's table of held notes, in SDRAM behind the control-change ring
app::PerformBoard* g_perform = nullptr;      // the Perform screen's slots (OS-011b): in SDRAM behind the mixer's board
app::MixBoard* g_mix = nullptr;              // the mixer's runtime side (OS-011b): routing, cue level, meters' ballistics, the stem map's hook; in SDRAM behind the live notes

// OS-SENDFX, the send effects: SND A is the delay (audio/send_delay.h: its 4 s ring is the start of the SDRAM delay region), SND B the reverb (audio/send_reverb.h: its lines are the start of the
// SDRAM reverb region). The objects hold the per-block scratch (5,336 B in all, SRAM_U: the render touches it every sample), the long memory is SDRAM, cleared when attached.
audio::SendDelay g_send_delay;
audio::SendReverb g_send_reverb;
static_assert(mem::kDelayRingBytes == audio::SendDelay::kRingBytes && mem::kReverbMemBytes == audio::SendReverb::kMemBytes, "mem_map.h mirrors what the send effects take of their regions");
static_assert(sizeof(audio::SendDelay) + sizeof(audio::SendReverb) <= 6u * mem::kKiB, "the send effects' SRAM_U statics outgrew the 6 KiB the OS-SENDFX report gives them");

// OS-LIMIT, the master limiter: the chain's limiter stage (audio/master_limiter.h), always on. Its delay line and window state are about 1.2 KiB of SRAM_U, touched every sample; the code runs from flash.
audio::MasterLimiter g_master_limiter;
static_assert(sizeof(audio::MasterLimiter) <= 1536u, "the master limiter's SRAM_U static outgrew the 1.5 KiB the OS-LIMIT report gives it");

// Whether the outputs are open (main.cpp unmuted them, at boot since OS-OUTON), whether a Play or a note is waiting for that (the fallback when they are closed), and what it
// asked for: a pattern (g_pending_pattern, 1..255) or the song from a row (g_pending_song, g_pending_row).
bool g_outputs_open = false;
bool g_open_pending = false;
unsigned g_pending_pattern = 0;
bool g_pending_song = false;
unsigned g_pending_row = 0;

// OS-007g: what the sequencer and the voices did since a Play went to the ring, in the log (`diag log`), kPlayProbeMs after it: whether it plays, how many steps,
// events and notes there were, and what went wrong (a pattern that was not there, rows that tore, events lost). The render context writes these counters a word at
// a time and the main loop only reads them: a figure may be a block old.
constexpr std::uint32_t kPlayProbeMs = 1500;
std::uint64_t g_probe_ms = 0;  // when to log it; 0: nothing to log

void log_play_probe() noexcept {
  const seq::Sequencer::Stats s = g_seq->stats();
  const audio::VoicePool::Stats v = audio_voices().stats();
  char line[56];
  audio::diag::Text first(line, sizeof line);
  first.str("play: steps ").dec(s.steps).str(" events ").dec(s.events).str(" notes ").dec(v.note_ons).str(" ignored ").dec(v.ignored);
  console::log(line);
  audio::diag::Text second(line, sizeof line);
  second.str(g_seq->playing() ? "play: running" : "play: STOPPED").str(" missing ").dec(s.rows_missing).str(" torn ").dec(s.rows_torn).str(" lost ").dec(s.queue_overflow);
  console::log(line);
}

// OS-005e: the song's row in the log. The render publishes the song row with the pattern in one word (audio/play_position.h); the main loop reads it once a pass and
// says "play: song row N / pattern P" whenever the row is another one (row counted from 0, as the Song screen's POS): a Song Play, then every switch, the loop
// back to row 0 included, so a hardware check can follow the song by ear and by log. Nothing in Pattern mode or while stopped.
void log_song_row() noexcept {
  static std::uint16_t last = audio::PlayPosition::kNoOrder;
  const audio::PlayPosition::Value v = g_seq->play_position().read();
  const std::uint16_t row = v.playing ? v.order : audio::PlayPosition::kNoOrder;
  if (row == last) {
    return;
  }
  last = row;
  if (row != audio::PlayPosition::kNoOrder) {
    char line[40];
    audio::diag::Text text(line, sizeof line);
    text.str("play: song row ").dec(row).str(" / pattern ").dec(v.pattern);
    console::log(line);
  }
}

// OS-PATSWITCH: each landing of a pattern switch (F1 held, a pick while playing) in the log, for the unit check: "play: switched to pattern P row R", row counted from 0 as the Pattern screen's rows
// (numbering From1 shows R + 1). The render publishes the last landing as one word (Sequencer::last_landing: count, pattern, row; one writer), the main loop reads it once a pass; two landings between two
// passes show the later one.
void log_switch_landing() noexcept {
  static std::uint32_t last = 0;
  const std::uint32_t word = g_seq->last_landing();
  if (word == last) {
    return;
  }
  last = word;
  char line[44];
  audio::diag::Text text(line, sizeof line);
  text.str("play: switched to pattern ").dec((word >> 8) & 0xFFu).str(" row ").dec(word & 0xFFu);
  console::log(line);
}

bool push_command(seq::Command::Kind kind, unsigned pattern, std::uint16_t row = 0, std::uint32_t from_step = 0) noexcept {
  const bool pushed = g_seq != nullptr && g_seq->commands().push(seq::Command{kind, static_cast<std::uint8_t>(pattern), row, from_step});  // the main loop is the ring's one producer
  if (pushed && (kind == seq::Command::Kind::PlayPattern || kind == seq::Command::Kind::PlaySong)) {
    g_probe_ms = system_clock().uptime_ms() + kPlayProbeMs;
  }
  return pushed;
}

// The Play button's transport: a command on the sequencer's ring, taken at the next audio block. With the outputs closed (they open at boot; this is the fallback) it waits for them.
class SeqTransport final : public app::TransportSink {
 public:
  bool play(unsigned pattern) noexcept override {
    if (g_seq == nullptr || !audio_running()) {
      return false;  // no sequencer (headless) or no block clock: nothing would sound
    }
    if (!g_outputs_open) {
      g_pending_pattern = pattern;
      g_pending_song = false;
      g_open_pending = true;
      console::log("play: waiting for the outputs to open");
      return true;
    }
    return push_command(seq::Command::Kind::PlayPattern, pattern);
  }
  // OS-METRO: the same command with the bars to count in in its `b` field (the pattern's start step stays 0). With the outputs still closed (the fallback above) the count-in is dropped: the first Play opens them.
  bool play_count_in(unsigned pattern, unsigned bars) noexcept override {
    if (g_seq == nullptr || !audio_running()) {
      return false;
    }
    if (!g_outputs_open) {
      return play(pattern);  // the row is not kept across the deferred open
    }
    return push_command(seq::Command::Kind::PlayPattern, pattern, static_cast<std::uint16_t>(bars > 2u ? 2u : bars));
  }
  // OS-PLAYCUR: PlayPattern from step `row` (no count-in), and LocateRow while playing. With the outputs not yet open (the start-up only) it falls back to play(pattern): the pattern starts at its first step.
  bool play_from(unsigned pattern, unsigned row) noexcept override {
    if (g_seq == nullptr || !audio_running()) {
      return false;
    }
    if (!g_outputs_open) {
      return play(pattern);
    }
    return push_command(seq::Command::Kind::PlayPattern, pattern, 0, row);
  }
  bool locate_row(unsigned row) noexcept override {
    if (g_seq == nullptr || !g_outputs_open || !audio_running()) {
      return false;
    }
    return push_command(seq::Command::Kind::LocateRow, 0, 0, row);
  }
  bool play_song(unsigned row) noexcept override {  // the sequencer starts from the first position when the row is past the end of the order list
    if (g_seq == nullptr || !audio_running()) {
      return false;
    }
    const unsigned at = row < 0xFFFFu ? row : 0xFFFFu;
    if (!g_outputs_open) {
      g_pending_song = true;
      g_pending_row = at;
      g_pending_pattern = 0;
      g_open_pending = true;
      console::log("play: waiting for the outputs to open");
      return true;
    }
    return push_command(seq::Command::Kind::PlaySong, 0, static_cast<std::uint16_t>(at));
  }
  bool stop() noexcept override {
    g_pending_pattern = 0;
    g_pending_song = false;
    return push_command(seq::Command::Kind::Stop, 0);
  }
  // The live jump (OS-SONG1): a command on the same ring (the main loop is its one producer); the sequencer applies it at the pattern's end on the render's timeline. Only while playing, so
  // the outputs are open and the render drains the ring.
  bool queue_jump(unsigned row) noexcept override {
    if (g_seq == nullptr || !g_outputs_open || !audio_running()) {
      return false;
    }
    return push_command(seq::Command::Kind::QueueJump, 0, static_cast<std::uint16_t>(row < seq::kNoJump ? row : seq::kNoJump), g_seq->jump_stamp());  // E-SONG-07: the position is authored against the list as it is now
  }
  // The PATS pads (OS-PADS-B): the sequencer's existing QueuePattern command (the pattern id, 0 cancels), on the same ring.
  bool queue_pattern(unsigned id) noexcept override {
    if (g_seq == nullptr || !g_outputs_open || !audio_running() || id > 255u) {
      return false;
    }
    return push_command(seq::Command::Kind::QueuePattern, id);
  }
  // The held PATTERN chooser (OS-PATSWITCH): the playing pattern becomes `id` at the next step boundary. The sequencer's SwitchPattern command on the same ring; only while the render drains it.
  bool switch_pattern(unsigned id) noexcept override {
    if (g_seq == nullptr || !g_outputs_open || !audio_running() || id < 1u || id > 255u) {
      return false;
    }
    return push_command(seq::Command::Kind::SwitchPattern, id);
  }
  // A whole-pattern paste over the pattern that plays (OS-BUG-PASTEHANG): the sequencer's ReleasePattern command on the same ring; the tracks of that pattern end what they still sound at the next step boundary.
  bool release_pattern(unsigned id) noexcept override {
    if (g_seq == nullptr || !g_outputs_open || !audio_running() || id < 1u || id > 255u) {
      return false;
    }
    return push_command(seq::Command::Kind::ReleasePattern, id);
  }
  // What an external MIDI Start plays (OS-MIDISLV; seq::Command::Select, taken while stopped): the App says it when the choice changes. Only while the render drains the ring (the outputs are open);
  // until then it refuses and the App asks again on a later pass.
  bool select(bool song, unsigned pattern) noexcept override {
    if (g_seq == nullptr || !g_outputs_open || !audio_running() || pattern > 255u) {
      return false;
    }
    return g_seq->commands().push(seq::Command{seq::Command::Kind::Select, static_cast<std::uint8_t>(song ? 1u : 0u), static_cast<std::uint16_t>(pattern), 0});
  }
};
SeqTransport g_transport;

// The sequencer's per-track side (app::TrackSink, OS-MUTE): the deferred mute's commands on the same ring, from the main loop; the pending bits and the note counters are the sequencer's own
// published words (one writer: the render), read here. Nothing is pushed while the outputs are closed: the render does not run, and the caller then tells the strip at once.
class SeqTrackSink final : public app::TrackSink {
 public:
  bool playing() const noexcept override { return g_seq != nullptr && g_outputs_open && g_seq->play_position().read().playing; }
  bool mute_defer(unsigned track, app::MuteMode mode, bool was_muted) noexcept override {
    return g_seq != nullptr && g_outputs_open &&
           g_seq->commands().push(seq::Command{seq::Command::Kind::MuteQuant, static_cast<std::uint8_t>(track), static_cast<std::uint16_t>(mode), was_muted ? 1u : 0u});
  }
  bool mute_cancel(unsigned track) noexcept override {
    return g_seq != nullptr && g_outputs_open && g_seq->commands().push(seq::Command{seq::Command::Kind::MuteCancel, static_cast<std::uint8_t>(track), 0, 0});
  }
  std::uint16_t mute_pending() const noexcept override { return g_seq != nullptr ? g_seq->mute_pending() : std::uint16_t{0}; }
  std::uint8_t note_count(unsigned track) const noexcept override { return g_seq != nullptr ? g_seq->note_count(track) : std::uint8_t{0}; }
};
SeqTrackSink g_track_sink;

// The Perform screen's commands (OS-011b, seq/perform.h): on the sequencer's command ring, as the transport's are, from the main loop (the ring's one producer). Nothing is drained
// while the outputs are closed (the render does not run), so until they are open the sink refuses and the bank keeps what it wants to tell: the first Play opens them and the next
// pass tells the engine. A refusal is never a loss: the bank sends again.
class SeqPerformSink final : public seq::CommandSink {
 public:
  bool push(const seq::Command& command) noexcept override { return g_seq != nullptr && g_outputs_open && g_seq->commands().push(command); }
};
SeqPerformSink g_perform_sink;

// The app's live notes (app::PreviewSink, OS-005g): every call is the main loop's, the one producer of the live queue. Nothing is pushed while the audio is not running (nothing
// would take it, and a ring that fills would replay stale notes); while the outputs are still closed a note asks for them to open and is kept (the latest one) until they do (opened()). A pad's release is pushed only for a
// pad this sink pushed a note on. A push that does not go (a full ring: the render has stalled) is counted; for an Off or an AllOff it also sets `flush_`, and the next call (or
// the next pass of the main loop, tick()) puts an all-notes-off first, so that a lost release cannot leave a note ringing.
class LivePreview final : public app::PreviewSink {
 public:
  void preview(unsigned instrument, unsigned note) noexcept override {
    (void)push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePreview, 0, clamp7(note), clamp_instrument(instrument), audio::kLiveVelocityDefault, 0, 0});
  }
  void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept override {
    if (pad >= app::PadBoard::kPads) {
      return;
    }
    if (push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), clamp7(note), clamp_instrument(instrument), audio::kLiveVelocityDefault,
                              clamp_track(track), 0})) {
      held_ |= std::uint64_t{1} << pad;
    }
  }
  // A Record screen pad playing the take (OS-REC2): a pad note of the preview descriptor (instrument kPreviewSlot + 1, which clamp_instrument refuses for every other path), ended by pad_up().
  void take_pad_down(unsigned pad, unsigned track, unsigned note) noexcept override {
    if (pad >= app::PadBoard::kPads) {
      return;
    }
    if (push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), clamp7(note), static_cast<std::uint8_t>(audio::kPreviewSlot + 1u),
                              audio::kLiveVelocityDefault, clamp_track(track), 0})) {
      held_ |= std::uint64_t{1} << pad;
    }
  }
  // The Sample load audition (OS-FU2): the preview buffer's descriptor, `instrument` kPreviewSlot + 1 (clamp_instrument refuses it for every other path), at the root note.
  void audition(bool on, unsigned track) noexcept override {
    if (on) {
      (void)push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePreview, 0, audio::kRootNote, static_cast<std::uint8_t>(audio::kPreviewSlot + 1u), audio::kLiveVelocityDefault,
                                  clamp_track(track), 0});
    } else {
      release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePreview, 0, 0, 0, 0, 0, 0});
    }
  }
  // The Preview key of the sample screens (OS-FU3): source kLiveSourcePreview, key 1 (the audition's is key 0), so it is its own note and a pad's release does not end it.
  void hold_note(bool on, unsigned track, unsigned instrument, unsigned note) noexcept override {
    if (on) {
      (void)push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePreview, 1, clamp7(note), clamp_instrument(instrument), audio::kLiveVelocityDefault, clamp_track(track), 0});
    } else {
      release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePreview, 1, 0, 0, 0, 0, 0});
    }
  }
  std::uint32_t live_position() const noexcept override { return audio_voices().live_position(); }
  void pad_up(unsigned pad) noexcept override {
    if (pad >= app::PadBoard::kPads || (held_ & (std::uint64_t{1} << pad)) == 0u) {
      return;
    }
    held_ &= ~(std::uint64_t{1} << pad);
    release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), 0, 0, 0, 0, 0});
  }
  void midi_note(bool on, unsigned track, unsigned instrument, unsigned note, unsigned velocity, unsigned channel) noexcept override {
    if (note > 127u) {
      return;
    }
    if (on) {
      (void)push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourceMidi, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(note), clamp_instrument(instrument),
                                  clamp7(velocity == 0u ? 1u : velocity), clamp_track(track), static_cast<std::uint8_t>(channel > 16u ? 0u : channel)});
    } else {
      release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourceMidi, static_cast<std::uint8_t>(note), 0, 0, 0, 0, 0});
    }
  }
  // OS-APPUX S3 (ABI 1.10): the notes of the running app, source kLiveSourceApp with the app's slot 0..3 as the key (at most four held: ModuleUi counts them); an AllOff of the source ends them all.
  bool app_note(bool on, unsigned slot, unsigned instrument, unsigned note, unsigned velocity) noexcept override {
    if (slot >= 4u || note > 127u) {
      return false;
    }
    if (on) {
      return push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourceApp, static_cast<std::uint8_t>(slot), clamp7(note), clamp_instrument(instrument), clamp7(velocity == 0u ? 1u : velocity), 0, 0});
    }
    release(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourceApp, static_cast<std::uint8_t>(slot), 0, 0, 0, 0, 0});
    return true;
  }
  void app_notes_off() noexcept override { release(audio::LiveEvent{audio::LiveKind::AllOff, audio::kLiveSourceApp, 0, 0, 0, 0, 0, 0}); }
  void notes_off(bool midi_only) noexcept override {
    if (!midi_only) {
      held_ = 0;
    }
    release(audio::LiveEvent{audio::LiveKind::AllOff, midi_only ? audio::kLiveSourceMidi : audio::kLiveAllSources, 0, 0, 0, 0, 0, 0});
  }
  // The sample bank's handover (app/sample_bank.h): no live note is queued or held, and no lost release waits to be repeated. Without a running render nothing reads a descriptor.
  bool live_idle() const noexcept override { return g_live == nullptr || !audio_running() || (!flush_ && audio_engine().live_idle()); }
  // The outputs just opened (main loop, from audio_opened): the note that waited goes to the queue now, with the release that came while it waited.
  void opened() noexcept {
    if (g_live == nullptr || g_live->pending.kind != audio::LiveKind::On) {
      return;
    }
    const audio::LiveEvent on = g_live->pending;
    const bool off = g_live->pending_off;
    g_live->pending.kind = audio::LiveKind::None;
    g_live->pending_off = false;
    if (push(on) && off) {
      release(audio::LiveEvent{audio::LiveKind::Off, on.source, on.key, 0, 0, 0, 0, 0});
    }
  }
  // Once a pass of the main loop: a release that did not go is repeated as an all-notes-off.
  void tick() noexcept {
    if (flush_ && g_live != nullptr && g_live->queue.push(all_notes_off())) {
      flush_ = false;
      held_ = 0;
    }
  }
  std::uint32_t dropped() const noexcept { return dropped_; }

 private:
  static audio::LiveEvent all_notes_off() noexcept { return audio::LiveEvent{audio::LiveKind::AllOff, audio::kLiveAllSources, 0, 0, 0, 0, 0, 0}; }
  static std::uint8_t clamp7(unsigned v) noexcept { return static_cast<std::uint8_t>(v > 127u ? 127u : v); }
  static std::uint8_t clamp_instrument(unsigned v) noexcept { return static_cast<std::uint8_t>(v >= 1u && v <= audio::kSampleSlots ? v : 0u); }  // 0 names none: the voices ignore it
  static std::uint8_t clamp_track(unsigned v) noexcept { return static_cast<std::uint8_t>(v < audio::kLiveTracks ? v : 0u); }

  bool push(const audio::LiveEvent& e) noexcept {
    if (g_live == nullptr || !audio_running() || (audio_engine().offline_wanted() && e.kind == audio::LiveKind::On)) {  // (OS-WAVEXP: no new live note while the engine is the export's: nothing consumes the queue, and a burst of stale notes must not play when it ends. A release goes through: an export stops the notes that are held when it takes the engine (Engine::offline_enter, OS-EXPNOTE: the table forgets them, so a release that arrives later finds no note and does nothing; the releases queued during the export are sent when the engine is given back and find none))
      return false;
    }
    if (!g_outputs_open) {
      if (!g_open_pending) {
        g_open_pending = true;  // main.cpp opens the outputs once the audio path has run clean; audio_opened() then calls opened()
        console::log("live: waiting for the outputs to open");
      }
      if (e.kind != audio::LiveKind::On) {
        return false;
      }
      g_live->pending = e;  // the latest note wins; a pad stays held (this push "went"), its release is release()'s to note
      g_live->pending_off = false;
      return true;
    }
    if (flush_ && !g_live->queue.push(all_notes_off())) {
      ++dropped_;
      return false;
    }
    flush_ = false;
    if (!g_live->queue.push(e)) {
      ++dropped_;
      return false;
    }
    return true;
  }
  void release(const audio::LiveEvent& e) noexcept {
    if (g_live == nullptr || !audio_running()) {
      return;  // nothing can be ringing: a note was never pushed, or the audio is gone
    }
    if (!g_outputs_open) {  // a note may be waiting for the outputs: its release is kept (the Off of its key), an all-notes-off cancels it
      audio::LiveEvent& w = g_live->pending;
      if (w.kind == audio::LiveKind::On) {
        if (e.kind == audio::LiveKind::Off && e.source == w.source && e.key == w.key) {
          g_live->pending_off = true;
        } else if (e.kind == audio::LiveKind::AllOff && (e.source == audio::kLiveAllSources || e.source == w.source)) {
          w.kind = audio::LiveKind::None;
          g_live->pending_off = false;
        }
      }
      return;
    }
    if (!push(e)) {
      flush_ = true;
    }
  }

  std::uint64_t held_ = 0;  // the pads this sink pushed a note on, not yet released (main loop only)
  bool flush_ = false;
  std::uint32_t dropped_ = 0;
};
LivePreview g_live_preview;

// The status strip's CPU (OS-FU3, app/system_load.h): the audio render's cycles from AudioStatus, which the render publishes under its seqlock (snapshot() is the reader's
// side, bounded, any context). Main loop only, once a pass.
audio::gov::Snapshot module_governor_snapshot() noexcept;  // OS-GOV: defined with the module voices below
class AudioLoadFigures final : public app::AudioLoadSource {
 public:
  app::AudioLoad read() const noexcept override {
    const audio::StatusSnapshot s = audio_status().snapshot();
    const audio::VoicePool::VoiceSnapshot v = audio_voices().voice_snapshot();  // OS-VOX: the render's lock-free words (voices.h)
    const audio::gov::Snapshot g = module_governor_snapshot();                  // OS-GOV: the module governor's counters (relaxed words, one writer: the render)
    return app::AudioLoad{s.render_last, s.render_max, hal::audio::kCyclesPerBlock, v.in_use, audio::kMaxVoices, v.steals,
                          g.drops, g.mutes, static_cast<std::uint16_t>((static_cast<unsigned>(g.last) << 8) | g.last_slot)};
  }
};
AudioLoadFigures g_audio_load;

// The sample arena: SDRAM, as int16 samples. The demo kit and a loaded project's PCM both go here from the start of it.
// OS-011a: the last part of it is the memory the peak cache of the sample in view is built in (ui::plot::PeakCache: 1/12 of a byte a sample; for 4 Mi frames, 95 s
// of mono, 350 KB), so the sample screens can draw a waveform at any zoom. The loaders see the arena without it (kArenaSamples).
std::int16_t* sample_arena() noexcept { return reinterpret_cast<std::int16_t*>(static_cast<std::uintptr_t>(mem::core_view(mem::kSampleArenaBase))); }  // (the cached alias: sdram_view.h)
constexpr std::uint32_t kPeakFrames = std::uint32_t{1} << 22;
constexpr std::uint32_t kPeakSamples = static_cast<std::uint32_t>((ui::plot::PeakCache::elements_required(kPeakFrames, 1) + 3u) & ~std::size_t{3});
// Behind it, the bank's own big parts (app::SampleBankMem: the slot table, the edit history, the WAV reader, the slicer's hits: about 5 KB), which would otherwise be 5 KB of
// the main loop's SRAM (OS-011a: SRAM_U is nearly all spoken for).
constexpr std::uint32_t kBankMemSamples = static_cast<std::uint32_t>((sizeof(app::SampleBankMem) + 7u) & ~std::size_t{7}) / 2u;
// And behind that the control-change ring of MIDI Learn (midi::CcInput, about 300 bytes with the notes' ring), for the same reason: SRAM_U has about 200 bytes to give.
constexpr std::uint32_t kCcSamples = static_cast<std::uint32_t>((sizeof(midi::CcInput) + 7u) & ~std::size_t{7}) / 2u;
// And behind that the live notes' queue and table (audio::LiveBox, OS-005g: 64 events of 8 bytes and 8 slots).
constexpr std::uint32_t kLiveSamples = static_cast<std::uint32_t>((sizeof(audio::LiveBox) + 7u) & ~std::size_t{7}) / 2u;
// And behind that the mixer's board (app::MixBoard, OS-011b: the routing the Mixer screen edits, the 28 meters' ballistics and the dB scale: about 1.4 KB).
constexpr std::uint32_t kMixSamples = static_cast<std::uint32_t>((sizeof(app::MixBoard) + 7u) & ~std::size_t{7}) / 2u;
// And behind that the Perform screen's slots (app::PerformBoard, OS-011b: twelve slots, the live values and what each track was last told: about 0.5 KB).
constexpr std::uint32_t kPerformSamples = static_cast<std::uint32_t>((sizeof(app::PerformBoard) + 7u) & ~std::size_t{7}) / 2u;
// And behind that the capture device (platform/mk66/capture.h, OS-011c: the radio's register shadow and status, about 130 bytes).
constexpr std::uint32_t kCaptureSamples = static_cast<std::uint32_t>((kCaptureStorageBytes + 7u) & ~std::size_t{7}) / 2u;
// And behind that the recorder (app::Recorder, OS-011c: the config, the take's state and its overview, 2,048 pairs of min and max: about 9 KB).
constexpr std::uint32_t kRecorderSamples = static_cast<std::uint32_t>((sizeof(app::Recorder) + 7u) & ~std::size_t{7}) / 2u;
// And behind that the card's save layer (OS-007n: the FatFs StorageWriter with its two FIL, the save job with its read-back buffer, the native writer and the settings buffer: about 7 KB),
// again so that none of it is SRAM_U.
// OS-007o: and the native project loader (app::NativeFolderLoader: the native reader with its 3 KB of scratch), here for the same reason; an OG load uses the static g_loader below.
// OS-WAVEXP: the WAV export's memory, in the same SDRAM block (nothing of it is SRAM): the staging ring the render fills and the card drains (32 KiB), the export's event buffer, and the places of the
// export's own sequencer (a fresh one per pass, in place) and its offline renderer. The render runs on the main loop, so none of it is touched by the render line; SDRAM costs the sequencer's step
// 28 cycles an access instead of 10, which the export pays in speed, not in anything live.
struct ExportHost {
  alignas(8) std::uint8_t ring[app::ExportSource::kRingBytes];
  audio::SeqEvent events[audio::kMaxEventsPerBlock];
  alignas(seq::Sequencer) unsigned char sequencer[sizeof(seq::Sequencer)];
  seq::OrderStore order;  // E-SONG-07: the export sequencer's copy of the song list
  alignas(seq::OfflineRender) unsigned char render[sizeof(seq::OfflineRender)];
  const model::Project* project;
};
struct SaveHost {
  SaveHost() noexcept : writer(storage::device::fat(), storage::device::block()) {}
  save::FatStorageWriter writer;
  app::SaveService service;
  app::NativeFolderLoader native_loader;
  ExportHost export_host;
};
constexpr std::uint32_t kSaveSamples = static_cast<std::uint32_t>((sizeof(SaveHost) + 7u) & ~std::size_t{7}) / 2u;
// And behind that the apps' audio tap (audio::AppTap, OS-H2a: 64 blocks of 64 frames as atomic words and the per-block stats: about 9 KB of SDRAM). The render writes it a block at a time while an
// app with audio_tap has selected a point; the whole object is cleared when it is built, and every access goes through the arena's alias (the single-alias rule, sdram_view.h).
constexpr std::uint32_t kAppTapSamples = static_cast<std::uint32_t>((sizeof(audio::AppTap) + 7u) & ~std::size_t{7}) / 2u;
// And behind that the Sample load audition's buffer (OS-FU2): the first 4 s of a WAV at 44.1 kHz, mono (app::PreviewJob clips a longer file to it), 345 KiB of SDRAM.
constexpr std::uint32_t kPreviewSamples = 4u * 44100u;
constexpr std::uint32_t kArenaSamples = mem::kSampleArenaSize / static_cast<std::uint32_t>(sizeof(std::int16_t)) - kPeakSamples - kBankMemSamples - kCcSamples - kLiveSamples - kMixSamples - kPerformSamples - kCaptureSamples - kRecorderSamples - kSaveSamples - kAppTapSamples - kPreviewSamples;
static_assert(((kArenaSamples + kPeakSamples + kBankMemSamples) * sizeof(std::int16_t)) % alignof(midi::CcInput) == 0, "the control-change ring sits at an aligned address");
static_assert(((kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples) * sizeof(std::int16_t)) % alignof(audio::LiveBox) == 0, "the live queue sits at an aligned address");
static_assert(((kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples) * sizeof(std::int16_t)) % alignof(app::MixBoard) == 0, "the mixer board sits at an aligned address");
static_assert(((kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples) * sizeof(std::int16_t)) % alignof(app::PerformBoard) == 0, "the perform board sits at an aligned address");
std::int16_t* peak_storage() noexcept { return sample_arena() + kArenaSamples; }
static_assert(((kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples + kCaptureSamples + kRecorderSamples + kSaveSamples) * sizeof(std::int16_t)) %
                      alignof(audio::AppTap) == 0,
              "the app tap sits at an aligned address");
static_assert(((kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples + kCaptureSamples + kRecorderSamples + kSaveSamples + kAppTapSamples) *
               sizeof(std::int16_t)) % 4 == 0,
              "the audition's buffer is word aligned");
audio::AppTap* make_app_tap() noexcept {
  return new (sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples + kCaptureSamples + kRecorderSamples + kSaveSamples) audio::AppTap();
}
std::int16_t* preview_storage() noexcept {
  return sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples + kCaptureSamples + kRecorderSamples + kSaveSamples + kAppTapSamples;
}
app::SampleBankMem* make_bank_mem() noexcept { return new (sample_arena() + kArenaSamples + kPeakSamples) app::SampleBankMem(); }
midi::CcInput* make_cc_input() noexcept { return new (sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples) midi::CcInput(); }
audio::LiveBox* make_live_box() noexcept { return new (sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples) audio::LiveBox(); }
app::MixBoard* make_mix_board() noexcept { return new (sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples) app::MixBoard(); }
bool usb_request_outputs(usb::Profile profile) noexcept;  // OS-USBOUTS: the STEMS page's OUTPUTS hook; defined below, after g_usb_apply
app::PerformBoard* make_perform_board() noexcept {
  return new (sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples) app::PerformBoard();
}
static_assert(((kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples) * sizeof(std::int16_t)) % kCaptureStorageAlign == 0, "the capture device sits at an aligned address");
hal::capture::CaptureHw* capture_at() noexcept {  // (no pointer of its own in SRAM_U: the address is the arena's tail part; valid once make_sound() has built it)
  return reinterpret_cast<hal::capture::CaptureHw*>(sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples);
}
hal::capture::CaptureHw& make_capture() noexcept {
  return capture_create(sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples);
}
static_assert(((kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples + kCaptureSamples) * sizeof(std::int16_t)) % alignof(app::Recorder) == 0, "the recorder sits at an aligned address");
app::Recorder* recorder() noexcept {  // (no pointer of its own in SRAM_U: the address is the arena's tail part)
  return reinterpret_cast<app::Recorder*>(sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples + kCaptureSamples);
}
app::Recorder* make_recorder() noexcept {
  return new (recorder()) app::Recorder();
}
static_assert(((kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples + kCaptureSamples + kRecorderSamples) * sizeof(std::int16_t)) % alignof(SaveHost) == 0, "the save layer sits at an aligned address");
SaveHost* save_host() noexcept {  // (no pointer of its own in SRAM_U: the address is the arena's tail part; valid once make_sound() has built it)
  return reinterpret_cast<SaveHost*>(sample_arena() + kArenaSamples + kPeakSamples + kBankMemSamples + kCcSamples + kLiveSamples + kMixSamples + kPerformSamples + kCaptureSamples + kRecorderSamples);
}
// OS-H4: the staging ring of the apps' export calls (ADR 0014 amendment A): 16 KiB in the SDRAM slack window (SDRAM_STATIC), static, written by the app's calls and drained by the save job, both on the main
// loop. Not cleared at boot (the ring's counters are the object's; no byte of it is read before it is written).
O3OS_SDRAM_NOINIT alignas(8) std::uint8_t g_app_export_ring[app::AppExport::kRingBytes];
SaveHost* make_save_host() noexcept {
  SaveHost* const host = new (save_host()) SaveHost();
  host->service.attach(storage::device::storage(), host->writer);
  host->writer.attach_owner(storage::device::swap());  // OS-USBDISK U2a: gate() says Host while the computer has the card (WriteStatus::Host: the strip, and the settings stay dirty)
  host->service.attach_app_export(g_app_export_ring, sizeof g_app_export_ring);
  static_assert(app::OgTiming::kLines == 10u, "console.cpp prints 10 lines of the timing report");
  host->service.attach_cycles(&hal::cycles, core::kCpuHz / 1000000u);  // T16: the OG export's per-stage timing (`diag ogexp status`)
  return host;
}
// OS-WAVEXP: the export's sequencer, built in place for every pass over the same project (seq::OfflineRender::Wiring::make_sequencer); the live one (g_seq) is not touched.
seq::Sequencer* make_export_sequencer(void* ctx) noexcept {
  ExportHost& h = *static_cast<ExportHost*>(ctx);
  seq::Sequencer* s = new (h.sequencer) seq::Sequencer(*h.project);
  s->attach_order_store(h.order);
  return s;
}
bool export_live_running() noexcept { return audio_running(); }
// Wires the offline renderer and the export source into the save service. After the live sequencer and the engine are built (make_sound), before the audio starts.
void wire_export(SaveHost& host, const model::Project& project) noexcept {
  ExportHost& h = host.export_host;
  h.project = &project;
  seq::OfflineRender::Wiring w;
  w.engine = &audio_engine();
  w.project = &project;
  w.live = g_seq;
  w.events = h.events;
  w.event_cap = audio::kMaxEventsPerBlock;
  w.live_running = &export_live_running;
  w.make_sequencer = &make_export_sequencer;
  w.make_ctx = &h;
  seq::OfflineRender* const render = new (h.render) seq::OfflineRender(w);
  app::ExportSource::Wiring ew;
  ew.renderer = render;
  ew.ring = h.ring;
  ew.ring_bytes = sizeof h.ring;
  ew.cycles = &hal::cycles;
  ew.slice_cycles = app::ExportSource::kSliceUs * (core::kCpuHz / 1000000u);
  host.service.attach_export(ew);
}
// The sample bank (OS-011a, app/sample_bank.h): what each instrument slot has in the arena, the load of a WAV and the safe handover to the voices. One for ever,
// like the document it works on; the app is attached to it (and may be built again: a recovery) without the bank noticing.
app::SampleBank g_bank;

// ---- SD modules (OS-012a) -------------------------------------------------------------------------------------------------------------
// The module system: one object at the start of the module region (SDRAM, mem_map.h kModuleRegionBase), built by make_sound(), the rest of the region is the arena the
// modules' cold parts and instances' state are taken from; the top 16 KiB of SRAM_L (kModuleSlabBase) holds the voices' output blocks and then the modules' hot code.
// OS-PERF3: the two parts of the module system the RENDER touches every block live in SRAM_U instead of the SDRAM record below: the module voices (1.1 KB: the command ring, the instances'
// table, the owner map: the render pops, looks up and calls through it for every block) and the instruments' STATE blocks (module::ModuleSet::state(), 4 KiB: the voice's state is read
// and written about a hundred times a sample, and an SDRAM access costs the core 28 cycles against 10 for SRAM, `diag bench mem`). A state that does not fit the 4 KiB falls
// back to the SDRAM arena, slower and correct (kMaxModuleInstances is 8: eight TB-303s at 400 B each, their manifest's state_bytes, take 3,200 B and fit; the fallback is for modules with bigger states). The scratch blocks the modules render into are in the SRAM_L slab (attach_scratch), as before.
audio::ModuleVoices g_module_voices;
audio::gov::Snapshot module_governor_snapshot() noexcept { return g_module_voices.governor().snapshot(); }
constexpr std::uint32_t kModuleStateBytes = 4096;
alignas(64) std::uint8_t g_module_state[kModuleStateBytes];  // (.bss: zeroed at start, and the binder zeroes each block it takes)

struct ModuleSystem {
  ModuleSystem() noexcept : voices(g_module_voices) {}
  module::ModuleSet set;
  module::ModuleLoader loader;
  audio::ModuleVoices& voices;
  app::ModuleBinder binder;
  app::ModuleUi ui;          // OS-012b: the Instrument page's module controls and views, the engine picker, MODULES and the apps' pages (the view host's record is in it)
  std::uint16_t crashed_id;  // the module the previous run died in (module::fault_id), 0 none
  bool manual;               // `diag mod load` is stepping the loader
  char manual_name[module::kStemMax + 1];
};
constexpr std::uint32_t kModuleHostBytes = 64u * mem::kKiB;  // OS-012b: 16 KiB before the module UI (the view host's record is about 5 KB); OS-H3A: 32 KiB more for an app's edit group (2,048 steps and their 5-byte ops, about 26 KB)
static_assert(sizeof(ModuleSystem) <= kModuleHostBytes, "the module system outgrew the 64 KiB at the start of the module region");
static_assert(kModuleHostBytes % 64u == 0u && mem::kModuleRegionSize > kModuleHostBytes + 64u * mem::kKiB, "the module region keeps room for modules");
static_assert(audio::ModuleVoices::kScratchBytes % 64u == 0u && audio::ModuleVoices::kScratchBytes < mem::kModuleSlabSize, "the output blocks fit the slab");

ModuleSystem& modsys() noexcept { return *reinterpret_cast<ModuleSystem*>(static_cast<std::uintptr_t>(mem::core_view(mem::kModuleRegionBase))); }

// The crash context's module word while module code runs (audio::ModuleVoices::Hooks::enter): the id in, the previous one back out. The render interrupts the main loop, so it
// must restore what the main loop had stamped (a driver's module id), not clear it.
std::uint16_t stamp_module(std::uint16_t id) noexcept {
  const std::uint32_t ctx = boot::load_context();
  const std::uint16_t before = ctx == crash::kUnknownContext ? std::uint16_t{0} : crash::module_of(ctx);
  set_module(id);
  return before;
}
void module_log(const char* text) noexcept { console::log(text); }

// After code was written that will be executed (module/loader.h SyncFn): the data is in memory before the instruction fetch looks, the pipeline starts over, and the LMEM code
// cache is invalidated if it is on, so that a module placed over an older one never runs stale lines. LOAD-BEARING since OS-PERF3: the module region is used through the cached ICode
// alias (mem::core_view(), sdram_view.h), so a module's cold code is fetched through the code cache, and a module loaded over an older one in the same place would otherwise run the
// old lines. (The hot part goes to the SRAM_L slab, which the cache does not hold.)
void sync_code(const void*, std::uint32_t) noexcept {
  asm volatile("dsb" ::: "memory");
  (void)cache::invalidate_all();
  asm volatile("dsb\n\tisb" ::: "memory");
}

void make_modules() noexcept {
  ModuleSystem* m = new (reinterpret_cast<void*>(static_cast<std::uintptr_t>(mem::core_view(mem::kModuleRegionBase)))) ModuleSystem;
  // the slab: the voices' output blocks first, the rest is the hot arena
  std::uint8_t* slab = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(mem::kModuleSlabBase));
  (void)m->voices.attach_scratch(slab, audio::ModuleVoices::kScratchBytes);
  const std::uint32_t hot_addr = mem::kModuleSlabBase + audio::ModuleVoices::kScratchBytes;
  (void)m->set.hot().init(slab + audio::ModuleVoices::kScratchBytes, hot_addr, mem::kModuleSlabSize - audio::ModuleVoices::kScratchBytes);
  (void)m->set.cold().init(reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(mem::core_view(mem::kModuleRegionBase + kModuleHostBytes))),
                           mem::core_view(mem::kModuleRegionBase + kModuleHostBytes), mem::kModuleRegionSize - kModuleHostBytes);
  (void)m->set.state().init(g_module_state, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_module_state)), kModuleStateBytes);
  audio::ModuleVoices::Hooks hooks;
  hooks.enter = &stamp_module;
  hooks.log = &module_log;
  m->voices.set_hooks(hooks);
  m->set.set_builtins(module::kBuiltinModules, module::kBuiltinModuleCount);  // OS-012d: the TB-303, chip and FM, bytes in flash, loaded like a card's file (module/builtin.h)
  m->ui.attach(m->set, m->loader, m->voices);  // OS-012b
  m->ui.set_storage(&storage::device::storage());
  m->ui.set_sync(&sync_code);
  m->ui.set_clock(&system_clock(), &stamp_module);  // the views' time budget on the system's microsecond clock; a fault in view code names its module
  m->voices.set_sample_rate(static_cast<float>(hal::audio::sample_rate_mhz()) * 0.001f);
  m->crashed_id = 0;
  m->manual = false;
  const crash::CrashRecord* rec = boot_crash_record();
  if (rec != nullptr && rec->context != crash::kUnknownContext && crash::module_of(rec->context) >= 0x8000u) {
    m->crashed_id = crash::module_of(rec->context);  // the previous run died inside module code: that module is not loaded this boot
  }
}

// The demo kit into the start of the arena and the voices' table, its four instruments into the project and, if pattern 1 is empty, its pattern. Only
// into an empty table: it is for when there is nothing else to play (the boot, a project with no sample instrument). File > NEW does not come here any more (OS-NEWKEEP: a new project has
// the instruments the user chose, or none).
void install_demo(model::Project& project) noexcept {
  (void)app::install_demo_kit(g_bank, audio_voices(), project, sample_arena(), kArenaSamples);  // SDRAM: 61 KB of the 10.8 MB arena (OS-BUG-PASTE24: app/sample_bank.h; the instruments at unity under the document's law)
}

// The sound of a fresh document: the demo kit when the project holds nothing (a project load replaces it later), then the sequencer and the engine's wiring.
// Before the audio starts (main() calls start() after the app's init), so the render line never sees a half-built sequencer.
void make_sound(model::Project& project) noexcept {
  audio::VoicePool& voices = audio_voices();
  g_bank.init(voices, *g_arena, *make_bank_mem(), sample_arena(), kArenaSamples, peak_storage(), kPeakSamples);
  g_bank.set_preview_buffer(preview_storage(), kPreviewSamples);  // OS-FU2: Sample load's F5
  if (project.instrument_count() == 0) {
    install_demo(project);
  }
  g_seq = new (g_seq_storage) seq::Sequencer(project);
  g_seq->attach_order_store(g_seq_order);
  {
    seq::Sequencer::Config sc = g_seq->config();
    sc.emit_ticks = true;  // OS-005h: the Tick events (core FX 43) the Amiga voices' MED tick processor runs on; nothing else reads them
    g_seq->configure(sc);
  }
  // The MIDI link between the engine and the sequencer: the engine's event source is the link's step(), which does the MIDI side of every block around the sequencer's.
  seq::MidiLinkHooks hooks;
  hooks.heard_at = [](void*) noexcept -> std::uint32_t { return audio_block_heard_at(); };
  hooks.kick = [](void*) noexcept { midi_io::kick(); };
  g_midi = new (g_midi_storage) seq::MidiLink(*g_seq, hooks);
  g_cc = make_cc_input();
  g_midi->attach_cc(g_cc);  // before the drivers: the render is the ring's one producer, the main loop its one consumer
  g_live = make_live_box();
  make_modules();
  audio::AppTap* const app_tap = make_app_tap();  // OS-H2a: before the audio starts; the graph's render is its one writer, the module host's main-loop calls its reader
  audio_engine().graph().attach_app_tap(app_tap);
  modsys().ui.set_audio_tap(app_tap);
  modsys().ui.set_midi_monitor(&g_cc->monitor());  // OS-H1: the apps' MIDI-in ring lives in the control-change object; the render fills it only while an app with midi_in runs
  g_mix = make_mix_board();
  g_mix->meters = &audio_engine().graph().meters();  // the Mixer screen's bars follow the engine's meters (a lock-free snapshot, main loop)
  g_mix->stems = &usb0::stem_map();                  // and its STEMS page edits the USB stream's stem map (runtime only), handed on by the same hook as `diag usb map`
  g_mix->apply_stems = &usb0::apply_stem_map;
  g_mix->request_outputs = &usb_request_outputs;     // OS-USBOUTS: its OUTPUTS row asks the driver for 2 / 4 / 6 / 8 / 10 outputs
  g_mix->tracks = &g_track_sink;                     // the TRACKS pads' deferred mutes and activity (OS-MUTE)
  g_perform = make_perform_board();
  g_perform->out = &g_perform_sink;
  hal::capture::CaptureHw& capture = make_capture();  // OS-011c: the recorder's source switch and the FM radio (off until a radio source is chosen)
  app::Recorder* const rec = make_recorder();
  app::Recorder::Wiring recording;
  recording.hw = &capture;
  recording.graph = &audio_engine().graph();
  recording.params = &audio_engine().params();  // the main loop is the ParamChange ring's one producer: the app's pushes and the recorder's share it
  recording.bank = &g_bank;
  recording.mix = g_mix;
  rec->attach(recording);
  wire_export(*make_save_host(), project);  // OS-007n: the save layer (any mounted writable FAT card), attached to the app by start_app(); OS-WAVEXP: with the offline render behind its WAV export
  audio::Engine& engine = audio_engine();
  engine.attach_voices(&voices);
  engine.attach_live(g_live);  // the main loop is the queue's one producer (g_live_preview), the render its one consumer
  engine.attach_modules(&modsys().voices);
  engine.attach_amiga(&audio_amiga());  // OS-005h: the instruments whose engine is Amiga
  engine.attach_events(g_midi->event_source(g_events, audio::kMaxEventsPerBlock));
  // OS-SENDFX: the send effects go into the graph's two slots, un-bypassed (the sends reach them through the ParamChange ring: the track's sends, the routing bits, the master DELAY and REVERB
  // levels and mutes, and the effects' own controls on the Master FX page; the tempo from the event source). Before start: attach() clears the SDRAM (about 0.76 MB, a few tens of ms).
  g_send_delay.attach(reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(mem::core_view(mem::kDelayBase))), audio::SendDelay::kRingFrames);
  g_send_reverb.attach(reinterpret_cast<std::int16_t*>(static_cast<std::uintptr_t>(mem::core_view(mem::kReverbBase))), audio::SendReverb::kMemSamples);
  engine.graph().install_send_fx(0, &audio::SendDelay::process_fx, &g_send_delay, &audio::SendDelay::param_fx);
  engine.graph().install_send_fx(1, &audio::SendReverb::process_fx, &g_send_reverb, &audio::SendReverb::param_fx);
  engine.graph().set_send_fx_bypass(0, false);
  engine.graph().set_send_fx_bypass(1, false);
  // OS-LIMIT: the master limiter takes the chain's limiter slot, on from the first block (the base limiter holds MAIN to -0.3 dBFS, 63 samples behind); its switch, mode and controls come as ParamChanges.
  engine.graph().install_master_limiter(g_master_limiter);
  engine.graph().scratch().set_counter(&input::device::encoder_count);  // OS-DJ C1: the scratch platter reads the FTM2 counter once a block (Q8)
  // The drivers: UART4 and the emitter's PIT (priorities 48 and 96), the USB interrupt's MIDI binding (64). The lines are enabled last; the render has not started.
  (void)midi_io::start(g_midi->din(), g_midi->din_in(), g_midi->time_ref());
  usb0::bind_midi(g_midi->usb_out(), g_midi->usb_in(), g_midi->time_ref());
}

// The device settings that drive the MIDI link (seq::sync_config_from_settings): CLOCK IN, CLOCK SYNC, CLOCK MODE, MIDI OUT and OUT CHANNEL. When any of them changes (and once at
// the first pass, which is the boot: the settings file's values) the link's mode, followed port, PAR-119 correction, output ports and channel follow; the runtime-only rest of the
// configuration is kept. E-CFG-03: TRANSPORT IN and TRANSPORT OUT are two more of them. Main loop, every pass: eight generation reads. Each generation only ever advances, so their sum moves whenever one of them does.
void sync_midi_settings() noexcept {
  if (g_midi == nullptr || g_app == nullptr) {
    return;
  }
  namespace ds = model::device_setting;
  const model::Settings& dev = g_app->device_settings();
  const std::uint32_t gen = dev.generation(ds::ClockIn) + dev.generation(ds::ClockSync) + dev.generation(ds::ClockMode) + dev.generation(ds::MidiInPort) +
                            dev.generation(ds::MidiOutPort) + dev.generation(ds::MidiOutChannel) + dev.generation(ds::TransportIn) + dev.generation(ds::TransportOut);
  if (g_midi_clock_seen && gen == g_midi_clock_gen) {
    return;
  }
  g_midi_clock_seen = true;
  g_midi_clock_gen = gen;
  g_midi_cfg = seq::sync_config_from_settings(dev, g_midi_cfg);
  g_midi->set_config(g_midi_cfg);
}

// What the unit presents, or is changing to (the mode syncs below ask).
usb::Mode usb_mode_now() noexcept { return usb0::change_phase() != 0u ? usb0::pending_mode() : usb0::mode(); }  // what the unit presents, or is changing to

// OS-USBRET: the device setting USB RETURN and the USB driver's, kept in step by app::sync_usb_return (app/usb_return.h: SETTING -> DRIVER when the setting changed, and once at the first pass,
// the settings file's value; DRIVER -> SETTING when the `usb return` / `usb profile <n> out|duplex` console commands changed the driver, so that Config shows what the unit presents; a
// request the driver cannot take yet is made again on a later pass). A change while AUDIO is presented re-enumerates the unit. Main loop, every pass.
struct UsbReturnDriver {
  bool ready() const noexcept { return usb0::attached(); }
  bool disk() const noexcept { return usb_mode_now() == usb::Mode::Disk; }  // OS-USBDISK: the boot disk's DISK set is final (app/usb_return.h): the settings were never read
  bool target() const noexcept { return usb0::return_wanted(); }  // the WISH (OS-USBOUTS): committed when a request is accepted, so no pending branch; the presented value drops at 10 channels, the setting does not
  bool request(bool on) noexcept { return usb0::request_layout(usb0::mode(), usb0::profile(), on); }
};

// OS-USBAUDIO: the same for the mode. Asks with USB RETURN as the setting has it, so that Config's APPLY of both is one re-enumeration (this runs before the return sync, which then finds the
// driver's target equal to its setting). The console's `usb mode` is runtime only: the sync never copies the driver's mode back into the setting.
struct UsbAudioDriver {
  const model::Settings& device;
  bool ready() const noexcept { return usb0::attached(); }
  bool disk() const noexcept { return usb_mode_now() == usb::Mode::Disk; }
  bool audio() const noexcept { return usb_mode_now() == usb::Mode::Audio; }
  bool request(bool on) noexcept {
    return usb0::request_layout(on ? usb::Mode::Audio : usb::Mode::Standard, usb0::profile(), device.get(model::device_setting::UsbReturn) != 0.0f);
  }
};

void sync_usb_return() noexcept {
  if (g_app == nullptr) {
    return;
  }
  UsbAudioDriver audio_driver{g_app->device_settings()};
  app::sync_usb_audio(g_usb_audio, g_app->device_settings(), audio_driver);
  UsbReturnDriver driver;
  app::sync_usb_return(g_usb_return, g_app->device_settings(), driver);
  (void)g_usb_return_level.step(g_app->device_settings(), audio_engine().params());  // OS-006e: RETURN LEVEL to the engine (the first pass sends the settings file's value)
}

// OS-METRO: the metronome's two settings to the engine and the sequencer over the ParamChange ring (app/metro_sync.h); the first pass sends the settings file's values.
void sync_metro() noexcept {
  if (g_app != nullptr) {
    (void)g_metro_sync.step(g_app->device_settings(), audio_engine().params());
  }
}

// OS-004d: the MIDI and USB parts of the project (model/project_state.h, app/project_state.h; the mixer, Perform and the apps are the App's). PROJECT -> RUNTIME: when a load has ended the project's
// MIDI part is applied AFTER the device settings (so it wins until a setting changes again) and its USB part is handed to app::UsbApply, which asks the driver for the mode and profile and then
// sets the stem map, from the main loop, one step a pass (switching to or from AUDIO re-enumerates on the host; the MODE is the device setting USB AUDIO's since OS-USBAUDIO, so the project
// asks for its profile and stem map only; a project without a USB or MIDI part leaves them as they are). RUNTIME -> PROJECT: every change of either is stored in the project, compared with the last state seen (so a boot, a load or a device setting that was
// there before does not make the project unsaved), never while a load or a save runs (App::project_state_writable) and never while a USB change is in flight.
model::MidiState g_midi_seen;
model::UsbState g_usb_seen;
bool g_midi_seen_valid = false;
bool g_usb_seen_valid = false;
app::UsbApply g_usb_apply;

// OS-USBOUTS (ADR 0012 amendment 8): the number of USB outputs of the project, asked for by the Mixer's STEMS page. The profile goes to the driver with the return WISH as it is; the driver re-enumerates (AUDIO) or only
// remembers it (STANDARD), resets the stem map to the factory slots of the new profile, and sync_project_platform_state() stores profile and map in the project (unsaved, not an undo unit). Refused (false: the page keeps
// its pending value and says so) while a project's USB part is being applied or a change is in flight: usb0::request_layout alone would take a request in between and the apply would then fight it.
bool usb_request_outputs(usb::Profile profile) noexcept {
  if (g_usb_apply.pending() || usb0::change_phase() != 0u) {
    return false;
  }
  return usb0::request_layout(usb0::mode(), profile, usb0::return_wanted());
}

void apply_project_platform_state() noexcept {
  if (g_midi == nullptr || g_arena == nullptr) {
    return;
  }
  const model::Project& project = g_arena->project;
  if ((project.state_present() & model::Project::kStateMidi) != 0) {
    g_midi_cfg = seq::apply_project_midi(g_midi_cfg, project.midi());
    g_midi->set_config(g_midi_cfg);
  }
  g_midi_seen = seq::project_midi_state(g_midi_cfg);
  g_midi_seen_valid = true;
  g_usb_seen_valid = false;  // seeded again from the driver when it is idle (or by the apply below when it ends)
  if ((project.state_present() & model::Project::kStateUsb) != 0) {
    // OS-USBAUDIO: the mode stays as the unit presents it (the setting USB AUDIO decided it at boot, a runtime `usb mode` may have changed it since); the project's part brings its profile and stem map.
    g_usb_apply.begin(app::usb_apply_target(project.usb(), usb_mode_now()));
  } else {
    g_usb_apply.abandon();
  }
}

void sync_project_platform_state(bool loading) noexcept {
  if (g_midi == nullptr || g_app == nullptr || g_editor == nullptr) {
    return;
  }
  const bool was_pending = g_usb_apply.pending();
  switch (g_usb_apply.step(usb0::mode(), usb0::profile(), usb0::change_phase())) {
    case app::UsbApply::Act::Request:
      if (!usb0::request_mode(static_cast<usb::Mode>(g_usb_apply.target().mode), static_cast<usb::Profile>(g_usb_apply.target().profile))) {
        g_usb_apply.abandon();
      }
      break;
    case app::UsbApply::Act::Masks:
      (void)app::stem_map_apply(usb0::stem_map(), g_usb_apply.target());
      usb0::apply_stem_map();
      break;
    case app::UsbApply::Act::None:
      break;
  }
  const bool usb_idle = !g_usb_apply.pending() && usb0::attached() && usb0::change_phase() == 0u;
  if (usb_idle && (was_pending || !g_usb_seen_valid)) {
    g_usb_seen = app::usb_state_of(usb0::stem_map(), usb0::mode());  // what the driver is now is what the project has (or what there is to compare against)
    g_usb_seen_valid = true;
    return;
  }
  const model::MidiState midi_now = seq::project_midi_state(g_midi_cfg);
  if (!g_midi_seen_valid) {
    g_midi_seen = midi_now;
    g_midi_seen_valid = true;
  }
  if (loading || !g_app->project_state_writable()) {
    return;
  }
  if (midi_now != g_midi_seen) {
    g_midi_seen = midi_now;
    (void)g_editor->set_midi_state(midi_now);
  }
  if (usb_idle) {
    const model::UsbState usb_now = app::usb_state_of(usb0::stem_map(), usb0::mode());
    model::UsbState seen = g_usb_seen;
    seen.mode = usb_now.mode;  // OS-USBAUDIO: the mode is the setting's (and the console's), not the project's: a change of it alone is not a change of the project
    if (usb_now != seen) {
      g_usb_seen = usb_now;
      (void)g_editor->set_usb_state(usb_now);
    }
  }
}

// MIDI Learn's control changes (OS-013a): the main loop is the ring's one consumer. While a project loads (`loading`) the document is being rewritten behind the app's back, so
// what has arrived is thrown away instead of applied (bounded: the ring holds kRingSize).
void poll_midi_cc(bool loading) noexcept {
  if (g_power_latched) {
    return;  // OS-POWER P1: the MIDI input is ignored once the shutdown has begun (D2)
  }
  if (g_cc == nullptr || g_app == nullptr) {
    return;
  }
  if (loading) {
    midi::CcMsg drop;
    for (std::uint32_t i = 0; i < midi::CcInput::kRingSize && g_cc->pop(drop); ++i) {
    }
    midi::NoteMsg drop_note;  // a note ends up as nothing too: the document is not being played
    for (std::uint32_t i = 0; i < midi::CcInput::kNoteRingSize && g_cc->pop_note(drop_note); ++i) {
    }
    return;
  }
  (void)g_app->poll_midi_cc(*g_cc);
  (void)g_app->poll_midi_notes(*g_cc);
  g_live_preview.tick();
}
#endif

void make_document() noexcept {
  g_arena = new (reinterpret_cast<void*>(static_cast<std::uintptr_t>(mem::core_view(mem::kProjectBase)))) model::ModelArena;  // trivial: nothing is written yet
  model::Project& project = g_arena->project;
  project.reset();
  (void)project.create_pattern(1, model::kDefaultPatternLength);  // pattern 1, 32 steps on every track, one song position that plays it
  (void)project.append_order(1);
#if !O3OS_AUDIO_TEST
  make_sound(project);  // the demo kit and its pattern, the sequencer, the voices
#endif
  (void)g_journal.begin(reinterpret_cast<void*>(static_cast<std::uintptr_t>(mem::kUndoBase)), mem::kUndoSize);
  g_editor = new (g_editor_storage) model::Editor(project, g_journal);
}
// OS-007e: the project load. The loader (its OG readers are about 7 KB) is a static of this file; one load at a time, stepped from pass().
// OS-QAFIX-4: the Verify phase of an open (app::ProjectVerify, about 1 KB, main loop only): the file is checked before anything is replaced; a refusal ends the open with the document, the transport and the
// voices untouched (g_open_refused, the words in g_open_refused_why; the CardProject sink reports it).
O3OS_SDRAM_NOINIT SdramBox<app::ProjectVerify> g_verify;
bool g_open_refused = false;
const char* g_open_refused_why = "";
O3OS_SDRAM_NOINIT SdramBox<app::OgFolderLoader> g_loader;  // OS-MEM1: main loop only (stepped from pass()), so in the SDRAM slack window: 8.3 KB off SRAM_U
// OS-007o: a native project is loaded by the native loader of the save layer's block (SDRAM); native_load() says which of the two the last load started was.
#if !O3OS_AUDIO_TEST
app::NativeFolderLoader* native_loader() noexcept { return g_seq != nullptr ? &save_host()->native_loader : nullptr; }  // (the sound, and with it the block, is built)
#else
app::NativeFolderLoader* native_loader() noexcept { return nullptr; }  // the tone image has no save layer, so no native load
#endif
app::NativeFolderLoader* native_load() noexcept {
  app::NativeFolderLoader* const n = native_loader();
  return n != nullptr && n->active() ? n : nullptr;
}
// OS-FILES part A: a module imported from the File screen (app::ModuleImport: the MOD / S3M and OctaMED importers, about 39 KB, main loop only: SDRAM like the loaders). When it is the load that was
// started, its phases replace the project loader's and the samples come from it (OgSampleLoader::begin_feed). The tone image has no voices to bind, so no import.
#if !O3OS_AUDIO_TEST
O3OS_SDRAM_NOINIT SdramBox<app::ModuleImport> g_module;
app::ModuleImport* module_load() noexcept { return g_module->active() ? &*g_module : nullptr; }
#else
app::ModuleImport* module_load() noexcept { return nullptr; }
#endif
void module_release() noexcept {
#if !O3OS_AUDIO_TEST
  g_module->release();  // from here the OG or native loader's report is the load's
#endif
}
char g_load_summary[96] = "";
constexpr std::size_t kLoadBytesPerPass = 16 * 1024;  // about 2 ms of card at 8 MB/s: a pass of the main loop stays well under a frame

// OS-007f: where a load is. Verify (OS-QAFIX-4, an open of a project folder only): the file is checked (app::ProjectVerify; the transport runs on, the app's frames are held, the card claim, saves and the USB hand-over are held out as in every phase); a refusal ends the open there, nothing
// replaced. Settle: the transport is stopped and the voices are let to die away (at most kSettleMaxMs: a voice that will not is a glitch, not a
// hang); Project: the OG readers (app::OgFolderLoader); Samples: the PCM of the sample instruments into the arena (app::OgSampleLoader); Modules (OS-012a): the SD modules the
// project's Module instruments name, loaded and bound (app::ModuleBinder). The app's frames are held through all four. Only the tone image (O3OS_AUDIO_TEST) has no voices: its load is the Project phase alone.
enum class LoadPhase : std::uint8_t { Idle, Verify, Settle, Project, Samples, Modules };
LoadPhase g_load_phase = LoadPhase::Idle;
char g_sample_summary[64] = "";

// OS-010a: the File screen's open goes through the very load that `diag load` starts (start_project_load: one loader, one set of phases), asked of it
// through the app's ProjectSink. The app reads the outcome from the loader's state once the load has ended.
class CardProject final : public app::ProjectSink {
 public:
  Start start_new() noexcept override { return new_project(false); }
  Start start_new_keeping_instruments() noexcept override { return new_project(true); }
  static Start new_project(bool keep_instruments) noexcept {
    switch (start_new_project(keep_instruments)) {
      case LoadStart::Started: return Start::Started;
      case LoadStart::Busy: return Start::Busy;
      case LoadStart::NoCard: return Start::NoCard;
      case LoadStart::NoDocument: return Start::NoDocument;
      case LoadStart::Scratch: return Start::Busy;  // (the File actions refuse it first, with the page's own words)
      case LoadStart::BadPath: break;
    }
    return Start::BadPath;
  }
  Start open(const char* folder, Format format) noexcept override {
    switch (start_project_load(folder, format == Format::Native ? LoadFormat::Native : (format == Format::NativeBackup ? LoadFormat::NativeBackup : LoadFormat::Og))) {
      case LoadStart::Started: return Start::Started;
      case LoadStart::NoCard: return Start::NoCard;
      case LoadStart::NoDocument: return Start::NoDocument;
      case LoadStart::Busy: return Start::Busy;
      case LoadStart::Scratch: return Start::Busy;  // (the File actions refuse it first, with the page's own words)
      case LoadStart::BadPath: break;
    }
    return Start::BadPath;
  }
  bool busy() const noexcept override { return g_load_phase != LoadPhase::Idle; }
  Start import_module(const char* file, app::ImportEngine engine) noexcept override {
    switch (start_module_import(file, static_cast<std::uint8_t>(engine))) {
      case LoadStart::Started: return Start::Started;
      case LoadStart::NoCard: return Start::NoCard;
      case LoadStart::NoDocument: return Start::NoDocument;
      case LoadStart::Busy: return Start::Busy;
      case LoadStart::Scratch: return Start::Busy;  // (the File actions refuse it first, with the page's own words)
      case LoadStart::BadPath: break;
    }
    return Start::BadPath;
  }
  bool module_file(const char* file) noexcept override {  // OS-IMPORT-ANY I4: the first bytes of the file, before the NEW question
#if !O3OS_AUDIO_TEST
    hal::storage::Storage& card = storage::device::storage();
    return !card.mounted() || app::ModuleImport::probe(card, file) != app::ModuleImport::Probe::NotModule;
#else
    (void)file;
    return true;
#endif
  }
  const app::ModuleSummary* import_summary() const noexcept override {
#if !O3OS_AUDIO_TEST
    return &g_module->summary();
#else
    return nullptr;
#endif
  }
  const char* folder() const noexcept override {
    return module_load() != nullptr ? "" : (native_load() != nullptr ? native_load()->folder() : g_loader->folder());  // (a module is in no project folder)
  }
  bool loaded() const noexcept override {
    if (module_load() != nullptr) {
      return module_load()->state() == app::ModuleImport::State::Done;
    }
    return native_load() != nullptr ? native_load()->state() == app::NativeFolderLoader::State::Done : g_loader->state() == app::OgFolderLoader::State::Done;
  }
  bool refused() const noexcept override { return g_open_refused; }
  const char* detail() const noexcept override {
    if (g_open_refused) {
      return g_open_refused_why;
    }
    if (module_load() != nullptr) {
      return module_load()->summary().error;
    }
    return native_load() != nullptr ? native_load()->report().detail : g_loader->report().detail;
  }
};
CardProject g_card_project;
#if !O3OS_AUDIO_TEST
O3OS_SDRAM_NOINIT SdramBox<app::OgSampleLoader> g_sample_loader;  // 2.2 KB, main loop only: in the SDRAM slack window with the project loader (OS-MEM1)
constexpr std::uint32_t kSettleMinMs = 4;    // the Stop is taken at a block start (1.45 ms), a block already running may still start a note: two blocks and a margin
constexpr std::uint32_t kSettleMaxMs = 250;  // the longest a voice's release can take is a few ms (voices.h); this is a generous bound
std::uint64_t g_settle_start_ms = 0;
#endif

std::uint64_t g_last_tick_ms = 0;
bool g_start_failed = false;  // the app could not start: the safe state (pass() reports it for good)
bool g_input_degraded = false;  // the control-button key scanner did not answer at boot (kInputDegraded, main.cpp plays it once; the pads have their own verdict, pads_gave_no_answer)
bool g_input_up = false;        // init() brought the input up: until then nothing touches its pins, its bus clocks or its chips
BootGate g_gate;                // the boot screen: the hook's rule, the hold, the driver flag (main thread only)

// The tracks the project saved muted (project.mt, 1-based as the Pattern screen numbers them) are in the log: here the flag is the Track parameter Mute, told to the graph's strip when the document is replaced
// (app::tracks_apply, OS-MUTE): the track keeps playing but its strip is down until it is unmuted (Shift + its pad). A project whose drums seem to be missing may only have them muted (OS-007j).
void log_muted_tracks(const model::Project& project) noexcept {
  char line[80];
  audio::diag::Text text(line, sizeof line);
  text.str("load: muted by the project:");
  bool any = false;
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    const model::TrackSlot* slot = project.track(t);
    if (slot != nullptr && (slot->flags & model::kTrackMute) != 0u) {
      text.chr(' ').dec(t + 1u);
      any = true;
    }
  }
  if (any) {
    console::log(line);
  }
}

// A load ended (Done or Failed): the undo history no longer matches the project (it changed behind the editor's back), the app is attached to the
// document again so that its view is clamped to what is there (and, for a project that loaded, shows the pattern its file was saved on), and the result goes
// to the log and to `diag sd`.
void finish_load() noexcept {
  const app::ModuleImport* const module = module_load();
  const app::NativeFolderLoader* const native = module != nullptr ? nullptr : native_load();
  if (module != nullptr) {
    app::module_strip_text(module->summary(), g_load_summary, sizeof g_load_summary);
  } else if (native != nullptr) {
    (void)app::describe_native_load(*native, g_load_summary, sizeof g_load_summary);
  } else {
    (void)app::describe_load(*g_loader, g_load_summary, sizeof g_load_summary);
  }
  const bool loaded = module != nullptr   ? module->state() == app::ModuleImport::State::Done
                      : native != nullptr ? native->state() == app::NativeFolderLoader::State::Done
                                          : g_loader->state() == app::OgFolderLoader::State::Done;
  console::log(loaded ? "boot: project loaded" : "boot: project load FAILED");
  console::log(g_load_summary);
  if (module == nullptr) {
    console::log_dec("load: bytes read", native != nullptr ? native->report().bytes_read : g_loader->report().bytes_read);
  }
  if (loaded) {
    log_muted_tracks(g_arena->project);
  }
#if !O3OS_AUDIO_TEST
  apply_project_platform_state();  // OS-004d: the project's MIDI and USB parts (the mixer and Perform are applied by the App's document_replaced below)
#endif
  if (g_editor != nullptr) {
    g_editor->history_reset();
    if (g_app != nullptr) {
      g_app->attach_document(g_editor);
      g_app->document_replaced(module != nullptr && loaded);  // OS-007o: the unsaved-changes baseline and "the project SAVE replaces" start over with this document (an imported song is on no card: dirty)
      if (loaded) {
        // an OG project opens on the pattern its file says it was saved on; a native one has no such field: the first of its song, or pattern 1
        const model::Project& project = g_arena->project;
        (void)g_app->open_pattern(module != nullptr || native != nullptr ? (project.song_length() != 0u ? project.order(0) : std::uint8_t{1}) : g_loader->report().start_pattern);
      }
    }
  }
  g_load_phase = LoadPhase::Idle;
}

#if !O3OS_AUDIO_TEST
bool has_sample_instrument(const model::Project& project) noexcept {
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = project.instrument(i);
    if (in != nullptr && in->kind == model::InstrumentKind::Sample) {
      return true;
    }
  }
  return false;
}

void begin_modules() noexcept;  // (below: begin_samples starts it for a new project)

// The voices' sample table emptied (the previous document's PCM, or the demo kit's: nothing is playing, the transport has been stopped since the load began, the Settle is over).
void clear_sample_table() noexcept {
  audio::VoicePool& voices = audio_voices();
  for (unsigned i = 0; i < audio::kSampleSlots; ++i) {
    voices.set_sample(i, audio::SampleDesc{});
  }
}

// The project is in the document: the voices' table is cleared (the demo kit's sounds, or the last project's: nothing is playing, the transport has been stopped
// since the load began) and the sample loader starts over the arena. A folder the loader refuses is logged and the load ends there.
void begin_samples() noexcept {
  audio::VoicePool& voices = audio_voices();
  // OS-NEWKEEP: a new project that kept the instrument table keeps the voices' sample table and the sample memory with it (the audio the instruments name)
  const bool keep_audio = module_load() == nullptr && native_load() != nullptr && native_load()->keeps_instruments();
  if (!keep_audio) {
    clear_sample_table();
  }
  g_sample_summary[0] = '\0';
  // OS-007i: the instrument volume law follows the project file's version (app::instrument_law): 1.2+ projects play their instruments at the -24..+24 dB law.
  // (a native project carries no version of the OG file it may have come from: it plays at the 1.2+ law, the one a new project has)
  app::ModuleImport* const module = module_load();
  if (module != nullptr) {  // OS-FILES: the module's samples are decoded on request into the arena (no folder: nothing is marked as on the card)
    g_bank.set_law(audio::LevelLaw::Og12);
    if (g_sample_loader->begin_feed(*module, g_arena->project, voices, sample_arena(), kArenaSamples, audio::LevelLaw::Og12, &g_bank)) {
      g_load_phase = LoadPhase::Samples;
    } else {
      console::log("boot: module samples NOT loaded");
      module->close();  // (the file is released here: samples_done() will not run)
      finish_load();
    }
    return;
  }
  app::NativeFolderLoader* const native = native_load();
  if (native == nullptr) {
    (void)app::commit_og_law_if_done(g_arena->project, *g_loader);  // OS-BUG-PASTE24: the load has succeeded (begin_samples is its Done): the document's law is the file's version; a failed load never gets here
  }
  const audio::LevelLaw law = app::project_law(g_arena->project);  // (a native project, an import and a new one are the 1.2+ law: Project::reset)
  g_bank.set_law(law);  // a WAV loaded afterwards into this project follows its law
  if (native != nullptr && native->fresh()) {  // OS-007o: a new project has no samples to load. ALL (OS-NEWKEEP): the sample memory is emptied, no demo kit goes in (the project has no instrument); KEEP INSTRUMENTS: the audio stays
    g_bank.new_project(keep_audio);  // (and no slot counts as held by the old project's folder: the first save writes them all)
    begin_modules();
    return;
  }
  if (g_sample_loader->begin(storage::device::storage(), native != nullptr ? native->folder() : g_loader->folder(), g_arena->project,
                            native != nullptr ? std::uint64_t{0} : g_loader->pti_slots(), voices, sample_arena(), kArenaSamples, law, &g_bank, native != nullptr)) {
    g_load_phase = LoadPhase::Samples;
  } else {
    console::log("boot: samples NOT loaded (the folder is not a path)");
    finish_load();
  }
}

// OS-012a: the samples are in (or there are none): the SD modules the project's Module instruments name. The modules the previous run died in are quarantined first (the platform
// stamped the running module's id into the crash context; the record names it), then the binder loads what is left and binds the instruments, a slice a pass.
void begin_modules() noexcept {
  ModuleSystem& m = modsys();
  const model::Project& project = g_arena->project;
  for (std::uint32_t i = 0; m.crashed_id != 0u && i < project.module_count(); ++i) {
    const model::ModuleRef* ref = project.module(i);
    if (ref != nullptr && module::fault_id(ref->name) == m.crashed_id && !m.set.quarantined(ref->name)) {
      (void)m.set.quarantine(ref->name, module::Status::Quarantined);
      char line[80];
      audio::diag::Text t(line, sizeof line);
      t.str("mod: ").str(ref->name).str(" was running when the last run crashed: not loaded");
      console::log(line);
    }
  }
  if (m.binder.begin(storage::device::storage(), project, m.set, m.loader, m.voices, &sync_code)) {
    g_load_phase = LoadPhase::Modules;
  } else {
    finish_load();
  }
}

void finish_modules() noexcept {
  const app::ModuleReport& r = modsys().binder.report();
  char line[112];
  (void)app::describe_modules(r, line, sizeof line);
  if (r.instruments != 0u || r.problem_count != 0u) {
    console::log(line);
  }
  for (unsigned i = 0; i < r.problem_count; ++i) {
    (void)app::describe_module_problem(r.problems[i], line, sizeof line);
    console::log(line);
  }
  if (r.problems_dropped != 0u) {
    console::log_dec("mod: more problems", r.problems_dropped);
  }
  finish_load();
}

// For a project with no sample instrument, the demo kit (the table is empty: begin_samples cleared it, the kit goes in from the start of the arena); then the SD modules.
void demo_then_modules() noexcept {
  if (module_load() == nullptr && !has_sample_instrument(g_arena->project)) {  // (an imported song has its own instruments: a song with none stays silent, it does not get the kit)
    install_demo(g_arena->project);
    console::log("boot: the project has no sample instrument: demo kit");
  }
  begin_modules();
}

// The samples are in: the summary and the first problems to the log and `diag sd`, then, for a project with no sample instrument, the demo kit again.
void finish_samples() noexcept {
  const app::SampleLoadReport& r = g_sample_loader->report();
  (void)app::describe_samples(r, g_sample_summary, sizeof g_sample_summary);
  char line[96];
  std::memcpy(line, "boot: ", 7);
  (void)app::describe_samples(r, line + 6, sizeof line - 6);
  console::log(line);
  for (unsigned i = 0; i < r.problem_count; ++i) {
    (void)app::describe_problem(r.problems[i], line, sizeof line);
    console::log(line);
  }
  if (r.missing + r.no_room > r.problem_count) {
    console::log_dec("load: more instruments without sound", r.missing + r.no_room - r.problem_count);
  }
  if (r.no_room != 0u) {
    console::log_dec("load: sample arena full, instruments silent", r.no_room);
  }
  if (r.approximated != 0u) {
    console::log_dec("load: instruments played one shot (a play mode the voices lack)", r.approximated);
  }
  app::ModuleImport* const module = module_load();
  if (module != nullptr) {
    module->samples_done(r);  // the counts into the report; the file is let go
  }
  demo_then_modules();  // (an imported song gets no kit: demo_then_modules checks, so it keeps its own patterns)
}
#endif

void begin_replace() noexcept;  // (below: the verified open starts it)

// The verified folder's loader begins (the verify said the file is whole; the loader's first step resets the document, after the Settle), then the hand-over of begin_replace.
bool begin_verified_open() noexcept {
  hal::storage::Storage& card = storage::device::storage();
  app::NativeFolderLoader* const native = native_loader();
  if (!g_verify->og()) {
    if (native == nullptr || !native->begin(card, g_verify->folder(), *g_arena, g_verify->backup())) {
      return false;
    }
  } else {
    if (!g_loader->begin(card, g_verify->folder(), *g_arena)) {
      return false;
    }
    if (native != nullptr) {
      native->release();  // from here the OG loader's report is the load's
    }
  }
  module_release();
  begin_replace();
  return true;
}

// One slice of a load: the verify, the settle, the project's slice, or the samples' slice. The app's frames are held by the caller while any of them runs.
void advance_load(std::uint64_t now) noexcept {
  (void)now;  // only the image with voices waits for them
  if (g_load_phase == LoadPhase::Verify) {
    const app::ProjectVerify::State state = g_verify->step(kLoadBytesPerPass);
    if (state == app::ProjectVerify::State::Running) {
      return;
    }
    if (state == app::ProjectVerify::State::Ok && begin_verified_open()) {
      return;
    }
    g_open_refused = true;  // the document, the history, the transport and the voices are as they were: no finish_load (it would reset the history and the baseline)
    g_open_refused_why = state == app::ProjectVerify::State::Refused ? g_verify->detail() : "NOT A PROJECT FOLDER PATH";
    char line[96];
    audio::diag::Text text(line, sizeof line);
    text.str(g_verify->folder()).str(": REFUSED ").str(g_open_refused_why);
    console::log("boot: project load REFUSED, the document is untouched");
    console::log(line);
    for (std::size_t i = 0; i < sizeof g_load_summary; ++i) {
      g_load_summary[i] = line[i];
      if (line[i] == '\0') {
        break;
      }
    }
    g_load_summary[sizeof g_load_summary - 1u] = '\0';
    g_load_phase = LoadPhase::Idle;
    return;
  }
#if !O3OS_AUDIO_TEST
  if (g_load_phase == LoadPhase::Settle) {
    const std::uint64_t waited = now - g_settle_start_ms;
    const bool quiet = !audio_running() || (waited >= kSettleMinMs && audio_voices().voices_in_use() == 0);  // the render context writes the voices: a benign read
    // OS-012a: the module instruments were withdrawn when the load began; the render has to be clear of them (two block starts) before they are shut down and their memory reused
    ModuleSystem& m = modsys();
    if ((quiet && (!audio_running() || m.voices.quiescent())) || waited >= kSettleMaxMs) {
      m.voices.release_all();  // shutdown() of each, main loop (after kSettleMaxMs the render has long passed its block starts; an audio that is not running calls nothing)
      m.set.clear();           // the modules and their memory: the new project's are loaded in the Modules phase (the quarantine stays)
      m.ui.reset();            // the apps' state and the catalogs pointed into them: OS-012b
      g_load_phase = LoadPhase::Project;
    }
    return;
  }
#endif
  if (g_load_phase == LoadPhase::Project) {
    app::ModuleImport* const module = module_load();
    app::NativeFolderLoader* const native = native_load();
    bool running;
    bool done;
    if (module != nullptr) {
      const app::ModuleImport::State state = module->step(kLoadBytesPerPass);
      running = state == app::ModuleImport::State::Running;
      done = state == app::ModuleImport::State::Done;
    } else if (native != nullptr) {
      const app::NativeFolderLoader::State state = native->step(kLoadBytesPerPass);
      running = state == app::NativeFolderLoader::State::Running;
      done = state == app::NativeFolderLoader::State::Done;
    } else {
      const app::OgFolderLoader::State state = g_loader->step(kLoadBytesPerPass);
      running = state == app::OgFolderLoader::State::Running;
      done = state == app::OgFolderLoader::State::Done;
    }
    if (running) {
      return;
    }
#if !O3OS_AUDIO_TEST
    if (done) {
      begin_samples();
      return;
    }
#else
    (void)done;
#endif
#if !O3OS_AUDIO_TEST
    if (module != nullptr && module->document_touched()) {
      clear_sample_table();  // the failed import emptied the document (its reader resets it first): the old PCM bound to the voices and held by the bank belongs to nothing
      g_bank.clear();
    }
#endif
    finish_load();  // the project did not load: the document and the voices stay as they were (a project file that read badly may have emptied the project; a failed import that began did)
    return;
  }
#if !O3OS_AUDIO_TEST
  if (g_load_phase == LoadPhase::Samples) {
    (void)g_sample_loader->step(kLoadBytesPerPass);
    if (g_sample_loader->state() != app::OgSampleLoader::State::Running) {
      finish_samples();
    }
    return;
  }
  if (g_load_phase == LoadPhase::Modules) {
    if (modsys().binder.step(kLoadBytesPerPass) != app::ModuleBinder::State::Running) {
      finish_modules();
    }
  }
#endif
}

// The boot chord: held at power-up it opens the INPUT TEST screen. Shift + F1 + F8, all three: a combination nobody holds by accident
// and none of the screen's own exit and clear chords (F4 + F5, F2 + F3), or the chord that opened it would close it again.
constexpr unsigned kChordButtons[] = {static_cast<unsigned>(app::Button::Shift), static_cast<unsigned>(app::Button::F1),
                                      static_cast<unsigned>(app::Button::F8)};
static_assert(sizeof(kChordButtons) / sizeof(kChordButtons[0]) == kBootChordKeys, "boot_chord.h counts the keys of this chord");
// The platform tests (mk66_boot_chord_test.cpp) may not include the app layer and spell these codes: this is where they are pinned.
static_assert(kChordButtons[0] == 29u && kChordButtons[1] == 0u && kChordButtons[2] == 7u, "the boot chord is Shift (29), F1 (0), F8 (7), bits 0, 1, 2 of the held mask");
// OS-USBDISK U1c: the boot disk chord is Shift + F1 + F7 (boot_chord.h boot_disk_chord: the INPUT TEST poll's Shift and F1 bits, F8 up, and F7 read when the poll ends).
constexpr unsigned kDiskChordKey = static_cast<unsigned>(app::Button::F7);
static_assert(kDiskChordKey == 6u, "the boot disk chord's third key is F7 (6)");
// OS-DIAG1: the chord was found at power-up and the INPUT TEST screen is the one to show. It outlives the app: a display re-initialised after a
// stall rebuilds the app on the Pattern screen, and the choice is applied to the new one (pass()). It follows the live screen: pass() sets it
// from the app that is about to be destroyed, so a user who has left INPUT TEST (F4 + F5) is not sent back to it. Main loop only.
bool g_boot_chord = false;
// OS-USBDISK (b): the boot disk mode, set once by init() from the chord Shift + F1 + F7 (boot_chord.h boot_disk_chord). The card is the computer's from the first instruction, so nothing watches the
// slot: poll_card() returns before the debounce and the mount, main.cpp mounts nothing and opens no output. Main loop only, never cleared (leaving is a reset).
bool g_boot_disk = false;
void open_input_test() noexcept {
  g_app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::InputTest));
}

// The pad LEDs follow the pads. What was last handed to the driver, so that a pass that changes nothing writes nothing.
std::uint64_t g_led_pads = ~std::uint64_t{0};
ui::ThemeId g_led_theme = ui::ThemeId::Amiga;
bool g_led_test = false;
bool g_led_lit = false;                     // the screen in view has its own roles for the pads (OS-011a: Chop lights its slices)
app::PadRole g_led_roles[hal::kPadLedCount] = {};
unsigned g_led_brightness = 0xFFu;           // the device setting PAD LIGHTS the LEDs were last set at (OS-CFGWIRE)
std::uint32_t g_backlight_generation = ~std::uint32_t{0};  // the generation of the device setting DISPLAY the backlight was last set from (OS-CFGWIRE)

#if O3OS_DEMO_CYCLE
constexpr std::uint32_t kDemoStepMs = 1500;  // demo cycle: one cursor step ...
constexpr unsigned kDemoThemeEvery = 5;      // ... a theme change every fifth step ...
constexpr unsigned kDemoHomeEvery = 24;      // ... and back to the top every 24th
std::uint64_t g_last_demo_ms = 0;
unsigned g_demo_step = 0;

void demo_step() noexcept {
  ++g_demo_step;
  if (g_demo_step % kDemoHomeEvery == 0) {
    g_app->invoke(core::action_id("cursor.home"));
  } else {
    g_app->invoke(core::action_id("nav.down"));
  }
  if (g_demo_step % kDemoThemeEvery == 0) {
    g_app->invoke(core::action_id("ui.theme.next"));
  }
}
#endif

// While a wait of boot::delay_ms() blocks (the audio start's codec waits), the display is serviced from here: the boot frame's swap,
// the backlight ramp. The first light also starts the boot screen's minimum. Not while the main loop is inside the driver (BootGate).
void idle_service() noexcept { g_gate.idle(display::device::display(), system_clock().uptime_ms()); }

// Shows the boot screen (the first frame of the device) and starts its hold. A frame the display will not take is not fatal: the app
// starts without a logo and the backlight ramps behind its first frame as before.
void show_boot_logo(hal::Display& display) noexcept {
  bool shown = false;
  for (std::uint32_t attempt = 0; attempt < kBootFrameRetries && !shown; ++attempt) {
    shown = ui::show_boot_screen(display, usb::kProductString, O3OS_BUILD_LABEL);
    if (!shown) {
      boot::delay_ms(1);
    }
  }
  if (shown) {
    g_gate.shown(system_clock().uptime_ms());
    boot::set_idle_hook(&idle_service);
  }
}

// OS-007o: the firmware update path holds the card from `fw put` to the end of `fw update`'s hand-over (an upload, its read-back, an arm, a re-check, the wait for the
// bootloader). The save layer asks before it starts anything (SaveService::attach_claim), so a save never starts while an upload, a check or the hand-over is going on.
// Main loop only.
bool firmware_has_card() noexcept { return update::fw_phase_holds_card(update::device::service().phase()); }

bool card_mount_pending() noexcept;  // (poll_card's, below)

// OS-USBDISK F3: a listing, an audition or any other job of the app that reads the card across passes. Conservative on purpose: any running job of the app holds the card.
bool app_card_jobs() noexcept { return g_app != nullptr && g_app->card_jobs_busy(); }

#if !O3OS_AUDIO_TEST
// OS-USBDISK F3: `diag mod load` steps the module loader across passes (ModuleSystem::manual) and reads the card meanwhile.
bool manual_module_load() noexcept { return modsys().manual; }
#endif

#if !O3OS_AUDIO_TEST
// OS-SCREENSHOT (docs/specs/OS-SCREENSHOT.md): Shift then Power saves the screen. The service (usb/screenshot_service.h) and its 2 KiB piece buffer live in the SDRAM window (nothing of them is SRAM);
// they are attached to the card's write side once the sound, and with it the save layer, is built. The card is claimed while a file is open. Main loop only.
class ShotHostImpl final : public console::ScreenshotHost {
 public:
  hal::storage::shot::Refusal card_refusal() noexcept override {
    using R = hal::storage::shot::Refusal;
    switch (storage::device::owner()) {
      case storage::CardOwner::Host: return R::OnComputer;
      case storage::CardOwner::NoCard: return R::NoCard;
      case storage::CardOwner::Tracker: break;
    }
    switch (save_host()->writer.gate()) {
      case hal::storage::WriteStatus::Ok: break;
      case hal::storage::WriteStatus::NoCard: return R::NoCard;
      default: return R::Unwritable;
    }
    return storage::device::card_busy() || g_load_phase != LoadPhase::Idle ? R::Busy : R::None;
  }
  bool console_shot_active() noexcept override { return console::shot_active(); }
  void notice(const char* text, bool warn) noexcept override {
    if (g_app != nullptr) {
      g_app->shot_notice(text, 3000, warn);
    }
  }
};
struct ShotStore {
  alignas(4) std::uint8_t buffer[console::ScreenshotService::kBufferBytes];
  ShotHostImpl host;
  console::ScreenshotService service;
};
static_assert(alignof(ShotStore) >= 4 && console::ScreenshotService::kBufferBytes % 4 == 0, "the screenshot's piece buffer is word aligned (alignas(4), and the store sits at its own alignment)");
O3OS_SDRAM_NOINIT SdramBox<ShotStore> g_shot;
bool g_shot_attached = false;
bool screenshot_has_card() noexcept { return g_shot_attached && g_shot->service.capturing(); }
#endif

// OS-SCREENSHOT: ends a screenshot that is armed or in flight, silently (its temporary file removed, the card released) and gives the app its input back. The shutdown's commit calls it (after the commit
// nothing is taken, spec behaviour 1), and so do a display that gave up and an app that failed, whose passes return before shot_pass.
void shot_cancel() noexcept {
#if !O3OS_AUDIO_TEST
  if (g_shot_attached) {
    g_shot->service.cancel(console::screenshot_port());
    console::hold_input(false);
  }
#endif
}

// Uploads the bitmaps and builds the app (again, if there is one: a recovery starts it over). false: the upload failed.
// `first_boot`: also puts the boot screen up (a recovery after a display re-initialisation goes straight to the app).
bool start_app(hal::Display& display, bool first_boot) noexcept {
  BootGate::DriverScope in_driver(g_gate);  // the uploads and the boot frame go through the driver, which may wait (BootGate)
  // The bitmaps the screen draws from (faces, tiles, icons, the boot logo) go to RAM_G first, through the same path the simulator uses.
  hal::UploadResult result = hal::UploadResult::Busy;
  for (std::uint32_t attempt = 0; attempt < kAssetRetries && result == hal::UploadResult::Busy; ++attempt) {
    result = ui::upload_assets(display);
  }
  if (result != hal::UploadResult::Ok) {
    return false;
  }
  if (first_boot) {
    show_boot_logo(display);  // before anything else is built: the sooner the frame is committed, the sooner the screen can light
  }
  if (g_app != nullptr) {
    g_app->~App();
    g_app = nullptr;
  }
  // The input goes through the console's tap (OS-006b), which hands every event on to the app unchanged and offers a copy to `diag input`.
  // The document and the sound first (the first boot builds them; a recovery keeps them): make_sound() creates the mixer and perform boards, the
  // sequencer and the save layer that the attach_* calls below hand to the app. Built after them, the first boot's app held null boards
  // (STEMS said "no stem map", the Mixer and Perform were not wired, SAVE was not attached) until a recovery rebuilt it (0.4.0, found on the unit).
  if (g_editor == nullptr) {
    make_document();
  }
  std::memset(g_app_storage, 0, sizeof g_app_storage);  // the SDRAM holds whatever it held: the App's members that its constructor leaves alone read zero, as in .bss
  g_app = new (g_app_storage) app::App(console::tap_input(input::device::scanner()), system_clock(), display);
#if !O3OS_AUDIO_TEST
  g_app->attach_transport(&g_transport);  // the Play button
  g_app->attach_preview(&g_live_preview);  // OS-005g: the pads, the previews and the MIDI keyboard play the voices
#endif
  g_app->set_build_info(usb::kProductString, O3OS_BUILD_LABEL);  // OS-011a: the Config screen's Info page
#if !O3OS_AUDIO_TEST
  g_app->attach_audio_load(&g_audio_load);  // OS-FU3: the strip's CPU
  g_app->attach_scratch(&audio_engine().graph().scratch());  // OS-DJ C1
  g_app->attach_samples(&g_bank);  // OS-011a: the sample screens (Sample load now) work on the arena through the bank
  g_app->attach_param_ring(&audio_engine().params());  // every ParamChange the app sends (Mixer levels/pan/mute/solo/routing, Master, the Params
                                                      // screen's engine fields, Learn); the main loop is the ring's one producer. Missing until
                                                      // 0.4.2: the simulator attached it, the unit did not, so the Mixer moved on screen and not in the sound
  g_app->attach_mix(g_mix);        // OS-011b: the Mixer screen: routing, meters, stems
  g_app->attach_perform(g_perform);  // OS-011b: the Perform screen: the slots, told to the sequencer's command ring
  g_app->attach_recorder(recorder());  // OS-011c: the Record screen: the sources, the take, the radio
  if (g_seq != nullptr) {              // (the sound, and with it the save layer, is built)
    save_host()->service.attach_claim(&firmware_has_card);  // OS-007o
    g_app->attach_saves(&save_host()->service);  // OS-007n: File's SAVE / SAVE AS, the WAV saves, the device settings' own save: all behind the card gate of hal/storage_write.h
  }
#endif
#if !O3OS_AUDIO_TEST
  g_app->attach_modules(&modsys().ui);  // OS-012b: the module pages, the engine picker, MODULES and the apps
#endif
  // OS-USBDISK F3: the one list of who keeps the card across passes (storage/card_claims.h); the hand-over to a computer and `sd mount` ask storage::device::card_busy().
  // A new user of the card across passes adds its claim here and nowhere else.
  // start_app() runs again on a recovery: claim() ignores a claim it already holds, and a refusal (table full) is logged, never dropped.
  bool claims_ok = storage::device::claim(&storage_in_use);       // a save job (project, WAV, settings...) or a project load phase (a module load is one)
  claims_ok = storage::device::claim(&firmware_has_card) && claims_ok;    // `fw put|arm|update` from the first byte to the end of the hand-over, every FwPhase but Idle and Done
  claims_ok = storage::device::claim(&card_mount_pending) && claims_ok;   // a card that was put in and waits for its mount
  claims_ok = storage::device::claim(&app_card_jobs) && claims_ok;        // the File / Sample load listing, the audition's reader, an import: the app's job runner
#if !O3OS_AUDIO_TEST
  claims_ok = storage::device::claim(&manual_module_load) && claims_ok;   // `diag mod load` stepping the loader across passes
  claims_ok = storage::device::claim(&screenshot_has_card) && claims_ok;  // OS-SCREENSHOT: the screenshot's file is open
#endif
  if (!claims_ok) {
    console::log("boot: card claims table full, a card user is not registered");
  }
  g_app->attach_storage(&storage::device::storage());  // OS-010a: the File screen lists Projects/ ...
  g_app->attach_project_sink(&g_card_project);         // ... and opens one through the load of `diag load`
  g_app->attach_firmware(&update::device::sink());     // OS-FWCONFIG: Config's INFO page, F8 UPDATE: the console's own check and hand-over behind the App's port (the unit's install() is the maintainer's hands session)
  g_app->attach_document(g_editor);
#if !O3OS_AUDIO_TEST
  if (g_seq != nullptr) {
    g_app->attach_play_position(&g_seq->play_position());  // OS-009b: the play bar, Browse following it and the strip's BPM (one lock-free word from the render)
  }
#endif
  g_last_tick_ms = system_clock().uptime_ms();
#if O3OS_DEMO_CYCLE
  g_last_demo_ms = g_last_tick_ms;
#endif
  return true;
}

}  // namespace

bool input_degraded() noexcept { return g_input_degraded; }
void input_audio_started() noexcept {
  if (g_input_up) {  // (a headless run never started the input: nothing to open)
    input::device::audio_started();
  }
}
bool pads_gave_no_answer() noexcept { return g_input_up && input::device::scanner().pads_gave_no_answer(); }

#if !O3OS_AUDIO_TEST
bool audio_open_pending() noexcept { return g_open_pending; }

void audio_opened() noexcept {
  g_outputs_open = true;
  g_open_pending = false;
  g_live_preview.opened();  // OS-FU1: the first live note, which waited for this
  if (g_pending_song) {
    (void)push_command(seq::Command::Kind::PlaySong, 0, static_cast<std::uint16_t>(g_pending_row));  // the Song Play that asked for them
    g_pending_song = false;
  } else if (g_pending_pattern != 0u) {
    (void)push_command(seq::Command::Kind::PlayPattern, g_pending_pattern);  // the Play that asked for them, from its first step
    g_pending_pattern = 0;
  }
}
bool audio_outputs_open() noexcept { return g_outputs_open; }
void audio_closed() noexcept { g_outputs_open = false; }  // (the pending request, if any, stays: it is what opens them again)
#else
bool audio_open_pending() noexcept { return false; }
void audio_opened() noexcept {}
bool audio_outputs_open() noexcept { return false; }
void audio_closed() noexcept {}
#endif

bool firmware_update_active() noexcept { return firmware_has_card(); }

// OS-006c: `fw update` stops the sequencer before it mutes the outputs and hands over to the bootloader: a Stop on the sequencer's ring and the badge, as a project load does.
void stop_transport() noexcept {
#if !O3OS_AUDIO_TEST
  (void)g_transport.stop();
  if (g_app != nullptr) {
    g_app->transport_stopped();
  }
#endif
}

// ---- OS-POWER P1 (internal.h has the contract; power/src/power_host_mk66.cpp is the caller) ----------------------------------------------------------------------
bool power_off_latched() noexcept { return g_power_latched; }
void power_display_off() noexcept { g_display_off = true; }

bool power_export_running() noexcept { return g_app != nullptr && g_app->export_running(); }
void power_export_stop() noexcept {
  if (g_app != nullptr) {
    g_app->export_stop();
  }
}

void power_quiesce() noexcept {
  g_power_latched = true;  // FIRST: from here nothing reopens the outputs and the audio poll stands down
  console::lock_input();   // the keys, pads and the encoder are dropped at the tap (D2); the shell's gate refuses the commands that would start anything
  shot_cancel();           // OS-SCREENSHOT: an armed or running screenshot ends here, its temporary file removed and the card released; shot_pass takes no new request from now on
#if !O3OS_AUDIO_TEST
  (void)g_transport.stop();         // a take in progress ends and stays in RAM, which is lost; MIDI note-offs and Stop go out as on any Stop
  g_live_preview.notes_off(false);  // the pads' held notes too
#endif
  if (g_app != nullptr) {
    g_app->transport_stopped();
    g_app->power_begin();
  }
}

bool power_save_busy() noexcept {
#if !O3OS_AUDIO_TEST
  return g_seq != nullptr && save_host()->service.power_busy();  // (the sound, and with it the save layer, is built)
#else
  return false;
#endif
}
bool power_save_committing() noexcept {
#if !O3OS_AUDIO_TEST
  return g_seq != nullptr && save_host()->service.power_committing();
#else
  return false;
#endif
}
void power_save_cancel() noexcept {
#if !O3OS_AUDIO_TEST
  if (g_seq != nullptr) {
    save_host()->service.power_cancel();
  }
#endif
}

// OS-007n: the device settings from Settings/device.o3st (read only), once the card is mounted: the file's values, or the defaults when there is none. The app's first pass after
// this takes them as "what is saved". A file that is damaged or newer is reported in the log and the defaults stand.
void load_device_settings() noexcept {
#if !O3OS_AUDIO_TEST
  if (g_app == nullptr || !storage::device::storage().mounted()) {
    return;
  }
  model::Settings loaded;
  const bool taken = app::load_device_settings(storage::device::storage(), loaded);
  if (taken && g_app->device_settings().assign(loaded) == model::Status::Ok) {
    console::log("boot: device settings loaded from Settings/device.o3st");
  } else if (storage::device::storage().exists("Settings/device.o3st", nullptr) == hal::storage::Status::Ok) {
    console::log("boot: Settings/device.o3st could not be read: defaults");
  }
#endif
}

// OS-USBAUDIO: starts the USB port, once, with the set the device settings decide: AUDIO when USB AUDIO is On (the default; also what a card without a settings file, an unreadable one, or no card
// gives) with the boot profile and USB RETURN as saved, else STANDARD. main.cpp calls it after the card is read (load_device_settings), before the pull-up, so the host enumerates once.
bool start_usb() noexcept {
  namespace ds = model::device_setting;
  bool usb_audio = false;  // no app (the display or the asset upload failed) and the tone images: STANDARD, the console only
  bool usb_return = true;
#if !O3OS_AUDIO_TEST
  if (g_app != nullptr) {
    usb_audio = g_app->device_settings().get(ds::UsbAudio) != 0.0f;
    usb_return = g_app->device_settings().get(ds::UsbReturn) != 0.0f;
  }
#endif
  const app::UsbBootLayout boot = app::usb_boot_layout(usb_audio, usb0::profile(), usb_return);  // (usb0::profile() is the boot layout until start() runs)
  if (g_boot_disk) {
    return usb0::start(usb::Mode::Disk, boot.profile, boot.usb_return);  // OS-USBDISK (b): the DISK set (CDC + MIDI + MSC) whatever the settings say (the card was not read)
  }
  return usb0::start(boot.mode, boot.profile, boot.usb_return);
}

// The document is about to be replaced (a load, a new project): the transport stops (a Stop on the sequencer's ring, and a Play that still waits for the outputs is forgotten) and
// the badge says so, the live notes are let go (a held pad or MIDI key would still sound on a voice whose sample is about to go), the modules withdraw, and the voices are let to
// die away before the first slice (Settle). The app's frames are held from here, so nothing starts it again.
namespace {
// OS-SAVEABORT: a save job (project, samples, WAV or settings) is running. A load or a new project replaces the document and empties the sample memory, which would cut that save
// half done (a first save of a new project would leave a folder with some of its samples and no project file), so neither starts meanwhile. Main loop only.
bool save_running() noexcept {
#if !O3OS_AUDIO_TEST
  return g_seq != nullptr && save_host()->service.busy();  // (the sound, and with it the save layer, is built)
#else
  return false;
#endif
}

void begin_replace() noexcept {
  g_load_summary[0] = '\0';
  g_sample_summary[0] = '\0';
#if !O3OS_AUDIO_TEST
  (void)g_transport.stop();
  g_live_preview.notes_off(false);  // OS-007o: all sources (the pads' held notes too)
  if (g_app != nullptr) {
    g_app->transport_stopped();
  }
  modsys().loader.cancel();   // a `diag mod load` in progress (OS-012a) gives way
  modsys().manual = false;
  modsys().ui.diag_clear();      // OS-BUGS4: the console's binds go with the instances
  modsys().voices.retire_all();  // the module instruments leave the render now; Settle waits for it to be clear of them
  g_settle_start_ms = system_clock().uptime_ms();
  g_load_phase = LoadPhase::Settle;
#else
  g_load_phase = LoadPhase::Project;
#endif
}
}  // namespace

namespace {
// OS-DJ C1: the DJ scratch ring is the sample arena's tail while SCR or TAPE STOP runs and while it ends: a project load, a new project and an import empty the arena (SampleBank::clear) and write over it, so the three starters refuse
// first, from the console as from the File screen (which says so itself, ahead of this). Main loop, like the starters.
bool scratch_holds_arena() noexcept { return g_app != nullptr && g_app->state().dj.scratch_holds_arena(); }
}  // namespace

LoadStart start_project_load(const char* folder, LoadFormat format) noexcept {
  hal::storage::Storage& card = storage::device::storage();
  if (!card.mounted()) {
    return LoadStart::NoCard;
  }
  if (g_arena == nullptr || g_editor == nullptr) {
    return LoadStart::NoDocument;
  }
  if (g_load_phase != LoadPhase::Idle || save_running()) {
    return LoadStart::Busy;
  }
  if (scratch_holds_arena()) {
    return LoadStart::Scratch;
  }
  if (app::folder_marked_deleted(card, folder)) {
    return LoadStart::BadPath;  // OS-FILES part B: a project DELETE hid is not loaded, from the console or the File screen alike (and so never saved into)
  }
  if (format != LoadFormat::Og && native_loader() == nullptr) {  // OS-007o: a native project (the tone image has no native loader: not a folder it can load)
    return LoadStart::BadPath;
  }
  if (!g_verify->begin(card, folder, format == LoadFormat::Og, format == LoadFormat::NativeBackup)) {
    return LoadStart::BadPath;
  }
  g_open_refused = false;
  g_load_summary[0] = '\0';
  g_sample_summary[0] = '\0';
  g_load_phase = LoadPhase::Verify;  // OS-QAFIX-4: nothing is replaced until advance_load has verified the file; begin_verified_open() then starts the loader and begin_replace()
  return LoadStart::Started;
}

// OS-QAFIX-E-FILE-10 (maintainer, 2026-10-22): the project that was open last (the first of Settings/recent.o3rc, app::boot_recent_project) comes back at power-on, opened through the app's File open so
// that its end is told on the strip (OPENED, or FAILED: why: the document is then as it booted). False when none is on the card, the app did not start (a failed display init leaves g_app null or
// g_start_failed set) or the load did not begin: main.cpp then opens the Workspace as before.
bool boot_open_last_project() noexcept {
  if (g_app == nullptr || g_start_failed) return false;
  char folder[48];  // "Projects/" and a name of at most 31 characters
  bool native = false;
  if (!app::boot_recent_project(storage::device::storage(), folder, sizeof folder, native)) return false;
  if (!g_app->boot_open(folder, native)) return false;
  char line[80];
  audio::diag::Text text(line, sizeof line);
  text.str("boot: opening last project ").str(folder);
  console::log(line);
  return true;
}

// OS-007o (File F3 NEW): a fresh project in the document, through the same hand-over as a load: the native loader's fresh form is the Project phase (it resets the document), the
// phases after it empty the sample memory (ALL) or leave it (OS-NEWKEEP, KEEP INSTRUMENTS) and bind the modules (begin_samples).
LoadStart start_new_project(bool keep_instruments) noexcept {
  if (g_arena == nullptr || g_editor == nullptr) {
    return LoadStart::NoDocument;
  }
  if (g_load_phase != LoadPhase::Idle || save_running()) {
    return LoadStart::Busy;
  }
  if (scratch_holds_arena()) {
    return LoadStart::Scratch;
  }
  app::NativeFolderLoader* const native = native_loader();
  if (native == nullptr) {
    return LoadStart::NoDocument;  // the tone image: no voices, no save layer, nothing to start over
  }
  g_open_refused = false;
  native->begin_fresh(*g_arena, keep_instruments);
  module_release();
  begin_replace();
  return LoadStart::Started;
}

// OS-FILES part A (File > IMPORT): a module file of the card replaces the document, through the same hand-over as a load (begin_replace: transport stopped, voices let to die away). The phases
// that follow are the module importer's and the PCM feed's (advance_load).
LoadStart start_module_import(const char* file, std::uint8_t engine) noexcept {
#if !O3OS_AUDIO_TEST
  hal::storage::Storage& card = storage::device::storage();
  if (!card.mounted()) {
    return LoadStart::NoCard;
  }
  app::NativeFolderLoader* const native = native_loader();
  if (g_arena == nullptr || g_editor == nullptr || native == nullptr) {
    return LoadStart::NoDocument;
  }
  if (g_load_phase != LoadPhase::Idle || save_running()) {
    return LoadStart::Busy;
  }
  if (scratch_holds_arena()) {
    return LoadStart::Scratch;
  }
  if (g_module->begin(card, file, *g_arena, static_cast<app::ImportEngine>(engine > 2u ? 0u : engine)) != app::ModuleImport::Start::Started) {
    return LoadStart::BadPath;
  }
  g_open_refused = false;
  native->release();  // from here the module's report is the load's
  begin_replace();
  return LoadStart::Started;
#else
  (void)file;
  (void)engine;
  return LoadStart::NoDocument;
#endif
}

bool project_loading() noexcept { return g_load_phase != LoadPhase::Idle; }

bool storage_in_use() noexcept {
#if !O3OS_AUDIO_TEST
  if (g_seq != nullptr && save_host()->service.busy()) {  // (the sound, and with it the save layer, is built)
    return true;  // a save job (project, WAV or settings) is running: `fw update` must not hand over under it, `fw put|rm` must not touch the volume meanwhile
  }
#endif
  return g_load_phase != LoadPhase::Idle;
}

const char* project_load_summary() noexcept { return g_load_summary; }

const char* project_sample_summary() noexcept { return g_sample_summary; }

// OS-BUG-PASTE24: a law as `diag inst` prints it.
int law_number(audio::LevelLaw law) noexcept { return law == audio::LevelLaw::Og1114 ? 1114 : 12; }

std::size_t project_instrument_line(unsigned slot, char* out, std::size_t cap) noexcept {
#if !O3OS_AUDIO_TEST
  if (g_arena != nullptr && slot < audio::kSampleSlots) {
    return app::describe_instrument(slot, g_arena->project.instrument(slot), audio_voices().sample(slot), out, cap, law_number(app::project_law(g_arena->project)), g_bank.demo_kit(), law_number(g_bank.law()), law_number(audio_voices().law()));  // set_sample() is the main loop's: this is it
  }
#else
  (void)slot;
#endif
  static constexpr char kNone[] = "inst: this image has no sample table";
  std::size_t n = 0;
  while (kNone[n] != '\0' && n + 1u < cap) {
    out[n] = kNone[n];
    ++n;
  }
  if (cap != 0u) {
    out[n] = '\0';
  }
  return n;
}

// ---- `diag ogexp` (OS-FILES-C C2a). Main loop only. The App decides what may start (no take, no transport, no job, no load); the save layer does the rest and the card guard has the last word.
const char* start_og_export() noexcept {
#if !O3OS_AUDIO_TEST
  if (g_app == nullptr || g_seq == nullptr) {
    return "THE APP HAS NOT STARTED";
  }
  const char* const why = g_app->start_og_export();
  if (why == nullptr) {
    console::log("ogexp: started by the console");
  }
  return why;
#else
  return "THIS IMAGE HAS NO SAVE LAYER";
#endif
}

void cancel_og_export() noexcept {
#if !O3OS_AUDIO_TEST
  if (g_app != nullptr && g_seq != nullptr) {
    g_app->cancel_og_export();
  }
#endif
}

std::size_t og_timing_line(unsigned i, char* out, std::size_t cap) noexcept {
#if !O3OS_AUDIO_TEST
  if (g_app != nullptr && g_seq != nullptr) {
    return save_host()->service.og_timing().line(i, out, cap);
  }
#else
  (void)i;
#endif
  if (out != nullptr && cap != 0u) {
    out[0] = '\0';
  }
  return 0;
}

bool og_export_status(OgExportStatus& out) noexcept {
#if !O3OS_AUDIO_TEST
  if (g_app == nullptr || g_seq == nullptr) {
    return false;
  }
  app::SaveService& s = save_host()->service;
  switch (s.og_state()) {
    case app::SaveService::OgState::Idle: out.state = "IDLE"; break;
    case app::SaveService::OgState::Running: out.state = "RUNNING"; break;
    case app::SaveService::OgState::Done: out.state = "DONE"; break;
    case app::SaveService::OgState::Failed: out.state = "FAILED"; break;
    case app::SaveService::OgState::Cancelled: out.state = "CANCELLED"; break;
    case app::SaveService::OgState::Modified: out.state = "MODIFIED"; break;
  }
  out.done = s.og_files_done();
  out.total = s.og_files_total();
  out.folder = s.og_folder();
  out.why = s.og_why();
  out.lost = s.og_drops().any_lost();
  out.levels = s.og_levels();
  out.clipped = s.og_clipped();
  const formats::og::ExportDrops& d = s.og_drops();
  out.instruments_not_written = d.instruments_not_written;
  out.no_sample_memory = d.no_sample_memory;
  out.bit_engine = d.bit_engine;
  out.amiga_engine = d.amiga_engine;
  out.pcm_too_long = d.pcm_too_long;
  out.levels_clamped = d.levels_clamped;
  out.max_step_us = s.og_max_step_us();
  out.overruns = s.og_overruns();
  return true;
#else
  (void)out;
  return false;
#endif
}

// ---- `diag mod` (OS-012a): the module system from the console. Main loop only.
ModStart start_module_load(const char* stem) noexcept {
#if !O3OS_AUDIO_TEST
  ModuleSystem& m = modsys();
  hal::storage::Storage& card = storage::device::storage();
  if (!card.mounted() && m.set.builtin(stem) == nullptr) {
    return ModStart::NoCard;
  }
  if (g_load_phase != LoadPhase::Idle || m.manual) {
    return ModStart::Busy;
  }
  if (!module::is_stem(stem)) {
    return ModStart::BadName;
  }
  if (m.set.find(stem) >= 0) {
    return ModStart::Already;
  }
  if (m.set.quarantined(stem)) {
    return ModStart::Quarantined;
  }
  if (!m.loader.begin(card, m.set, stem, &sync_code)) {
    return ModStart::Busy;
  }
  m.manual = true;
  std::strncpy(m.manual_name, stem, module::kStemMax);
  m.manual_name[module::kStemMax] = '\0';
  return ModStart::Started;
#else
  (void)stem;
  return ModStart::NoSound;
#endif
}

#if !O3OS_AUDIO_TEST
namespace {

// OS-MODVER: a module version for the console: "v1.0.0", or "unversioned" for the header's word 0 (o3module.h O3M_VERSION).
void put_version(audio::diag::Text& t, std::uint32_t v) noexcept {
  if (v == 0u) {
    t.str("unversioned");
    return;
  }
  t.chr('v').dec(O3M_VERSION_MAJOR(v)).chr('.').dec(O3M_VERSION_MINOR(v)).chr('.').dec(O3M_VERSION_PATCH(v));
}

// OS-303PERF: where an instance's state block lives, for `diag mod`: the 4 KiB state arena in SRAM_U (OS-PERF3, 10 cycles a load), or the SDRAM arena when it was full (28 cycles a load).
const char* state_region(const void* state) noexcept {
  const std::uint32_t a = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(state));
  if (a >= mem::kSramLBase && a < mem::kSramUBase + mem::kSramUSize) {
    return "SRAM";
  }
  const bool sdram = (a >= mem::kSdramBase && a < mem::kSdramBase + mem::kSdramSize) || (a >= mem::kSdramAliasBase && a < mem::kSdramAliasBase + mem::kSdramSize);
  return sdram ? "SDRAM" : "?";
}

// Why a built-in module kept its place against a card copy (card_status Older): "v1.0.0 not newer than built-in v1.0.0". A card copy with a HIGHER version that still lost is of another kind
// (same kind and a higher version is what prefers_card() takes), and the text says that instead of a falsehood.
void put_older(audio::diag::Text& t, const module::LoadedModule& lm) noexcept {
  put_version(t, lm.card_version);
  t.str(lm.card_version > lm.header.module_version ? " is another kind than built-in " : " not newer than built-in ");
  put_version(t, lm.header.module_version);
}

}  // namespace
#endif

std::size_t module_line(unsigned i, char* out, std::size_t cap) noexcept {
#if !O3OS_AUDIO_TEST
  ModuleSystem& m = modsys();
  audio::diag::Text t(out, cap);
  if (i == 0u) {
    t.str("mod: slab ").dec(m.set.hot().used()).str("/").dec(m.set.hot().size()).str(" B, module memory ").dec(m.set.cold().used() / 1024u).str("/")
        .dec(m.set.cold().size() / 1024u).str(" KiB, ").dec(m.voices.instances()).str(" instances, ").dec(m.set.count()).str(" loaded");
    t.str(", state ").dec(m.set.state().used()).str("/").dec(m.set.state().size()).str(" B (SRAM)");  // OS-PERF3
    t.str(m.voices.perf_enabled() ? ", perf on" : ", perf off (cpu reset starts it)");                 // OS-303PERF: the instances' min / mean / max below
    return t.size();
  }
  i -= 1u;
  if (i < m.set.quarantine_count()) {  // OS-303PERF: the refused stems come first (after the header): the listing is cut at the console's line limit, and the instances' perf lines (two for each instance) are what should give way, not these
    t.str("mod: quarantined ").str(m.set.quarantine_name(i)).str(" (").str(module::status_name(m.set.quarantine_reason(i))).chr(')');
    return t.size();
  }
  i -= m.set.quarantine_count();
  for (unsigned k = 0; k < m.set.count(); ++k) {  // a loaded module is a line, and a second one when a card copy of it was passed over as not newer (OS-MODVER)
    const module::LoadedModule& lm = *m.set.at(k);
    const unsigned lines = lm.builtin && lm.card_status == module::Status::Older ? 2u : 1u;
    if (i >= lines) {
      i -= lines;
      continue;
    }
    if (i == 1u) {
      t.str("mod: card ").str(lm.name).chr(' ');
      put_older(t, lm);
      return t.size();
    }
    t.str("mod: ").str(lm.name).chr(' ');
    put_version(t, lm.header.module_version);
    t.str(" ABI ").dec(lm.header.abi_major).chr('.').dec(lm.header.abi_minor).str(" hot ").dec(lm.header.hot_size)
        .str(lm.hot_in_sdram ? " B (SDRAM) cold " : " B (slab) cold ").dec(lm.header.image_size - lm.header.hot_size).str(" B state ").dec(lm.header.state_bytes)
        .str(" B params ").dec(lm.header.param_count).str(lm.builtin ? " (built-in)" : " (card)");
    return t.size();
  }
  for (unsigned k = 0; k < audio::kMaxModuleInstances; ++k) {
    audio::ModuleVoices::Info in;
    if (!m.voices.info(k, in)) {
      continue;
    }
    // OS-303PERF: with the measure on, every instance has a second line (its figures, or "no full block measured yet"). Whether it has figures changes under the render between two calls of this function, and a line
    // count that follows it would shift the lines of the instances after it (a line twice, a line lost) in the middle of a listing; perf_enabled() is the main loop's alone, so one listing sees one value.
    const unsigned lines = m.voices.perf_enabled() ? 2u : 1u;
    if (i >= lines) {
      i -= lines;
      continue;
    }
    if (i == 1u) {
      audio::ModulePerf perf;  // (stays empty when the render was writing it all four times the reader looked: "no full block measured", the next listing has it)
      (void)m.voices.perf(k, perf);
      return audio::diag::format_module_perf(in.slot + 1u, perf, out, cap);
    }
    t.str("mod: instrument ").dec(in.slot + 1u).str(": ").str(in.mod->name).str(in.active ? " sounding" : (in.held ? " held" : " idle"));
    if (in.track != audio::kNoTrack) {
      t.str(" on track ").dec(in.track + 1u);
    }
    if (in.cost != 0u) {  // OS-GOV: what the render measured of one voice, a tenth of a percent of the block (the governor's figure)
      const std::uint32_t tenths = audio::gov::tenths_of_block(in.cost);
      t.str(", ").dec(in.cost).str(" cycles/voice (").dec(tenths / 10u).chr('.').dec(tenths % 10u).str("%)");
    }
    const module::LoadedModule* from = nullptr;  // OS-303PERF: where it runs from: the state block, and the module's hot part (the slab, or the SDRAM when the slab had no room)
    for (unsigned q = 0; q < m.set.count() && from == nullptr; ++q) {
      from = m.set.at(q)->entry == in.mod ? m.set.at(q) : nullptr;
    }
    t.str(", state ").str(state_region(in.state)).str(", hot code ").str(from == nullptr ? "?" : (from->hot_in_sdram ? "SDRAM" : "slab"));
    if (in.muted) {
      t.str(", MUTED by the governor");
    }
    return t.size();
  }
#else
  (void)i;
  (void)out;
  (void)cap;
#endif
  return 0;
}

const char* module_notices() noexcept { return module::kBuiltinNotices; }

const char* module_bind(unsigned slot, const char* stem) noexcept {
#if !O3OS_AUDIO_TEST
  ModuleSystem& m = modsys();
  if (g_load_phase != LoadPhase::Idle || m.manual) {
    return "busy: a load is running";
  }
  const int idx = m.set.find(stem);
  if (idx < 0) {
    return "not loaded (`mod load <name>` first)";
  }
  const module::LoadedModule& lm = *m.set.at(static_cast<unsigned>(idx));
  if (lm.entry == nullptr || lm.header.kind != O3M_KIND_INSTRUMENT) {
    return "not an instrument this build can run";
  }
  // OS-BUGS4: the gate follows the project (ModuleUi::sync_slots): a slot that holds a sample or MIDI instrument of the project (t-v16's 49..64 are MIDI) is never routed to a module, so a bind
  // there could only be refused later, at `mod note`, as "not bound, or the queue is full". Say it now. A free slot (held open for the bench) and a Module slot are fine.
  if (g_arena != nullptr) {
    const model::Instrument* held = g_arena->project.instrument(slot);
    if (held != nullptr && held->kind == model::InstrumentKind::Midi) {
      return "that slot holds a MIDI instrument (bind a free slot or a module slot)";
    }
    if (held != nullptr && held->kind == model::InstrumentKind::Sample) {
      return "that slot holds a sample instrument (bind a free slot or a module slot)";
    }
  }
  if (m.voices.bound(slot)) {
    return "that instrument is bound already";
  }
  const module::ModuleSet::StateMark mark = m.set.state_mark();
  std::uint8_t* mem = nullptr;
  std::uint32_t addr = 0;
  if (!m.set.take_state(lm.entry->state_bytes, mem, addr)) {
    return "no module memory left";
  }
  std::memset(mem, 0, lm.entry->state_bytes);
  if (m.voices.bind(slot, *lm.entry, module::fault_id(stem), mem, nullptr, 0) != audio::ModuleVoices::Bind::Ok) {
    m.set.state_rollback(mark);
    return "bind failed (no free instance, or init() refused)";
  }
  m.ui.diag_keep_open(slot);  // OS-BUGS4: the project's slot is usually free: the once-a-pass gate sync must not close what the console just opened
  return nullptr;
#else
  (void)slot;
  (void)stem;
  return "this image has no sound";
#endif
}

bool module_audition(unsigned slot, unsigned note, unsigned velocity, bool on) noexcept {
#if !O3OS_AUDIO_TEST
  return modsys().voices.audition(slot, audio::kAuditionTrack, note, velocity, on);  // track 16: the spare strip a diag note plays on (the engine switched it on with the modules: OS-MODSILENT)
#else
  (void)slot; (void)note; (void)velocity; (void)on;
  return false;
#endif
}

std::uint32_t sample_pcm_bytes() noexcept {
#if !O3OS_AUDIO_TEST
  const std::uint32_t used = kArenaSamples - g_bank.free_samples();
  return (used > kArenaSamples ? kArenaSamples : used) * static_cast<std::uint32_t>(sizeof(std::int16_t));
#else
  return 0;  // the tone image has no sample bank
#endif
}

audio::ModuleVoices* module_voices() noexcept {
#if !O3OS_AUDIO_TEST
  return g_seq != nullptr ? &modsys().voices : nullptr;  // (g_seq is set by make_sound(), which also builds the module system)
#else
  return nullptr;
#endif
}

bool module_set_param(unsigned slot, unsigned id, float value) noexcept {
#if !O3OS_AUDIO_TEST
  return modsys().voices.set_param(slot, id, value);
#else
  (void)slot; (void)id; (void)value;
  return false;
#endif
}

std::size_t radio_report(char* out, std::size_t cap) noexcept {
#if !O3OS_AUDIO_TEST
  if (g_seq != nullptr) {  // (the sound, and with it the capture device, is built)
    return capture_radio_report(*capture_at(), out, cap);
  }
#else
  (void)out; (void)cap;
#endif
  return 0;
}

RadioRetry radio_retry() noexcept {
#if !O3OS_AUDIO_TEST
  if (g_seq != nullptr) {
    switch (recorder()->radio_retry()) {  // OS-RAD1 review: not under a take from the radio (the reset would record ~0.7 s of silence), not when line or mic is selected
      case app::Recorder::RadioRetry::Allowed:
        capture_radio_restart(*capture_at());
        return RadioRetry::Started;
      case app::Recorder::RadioRetry::TakeRunning:
        return RadioRetry::TakeRunning;
      case app::Recorder::RadioRetry::NotSelected:
        return RadioRetry::NotSelected;
      case app::Recorder::RadioRetry::NoRadio:
        break;
    }
  }
#endif
  return RadioRetry::NoRadio;
}

void service_while_blocked(bool on) noexcept {
  if (on) {
    if (display::device::display().ready()) {  // a display that never came up has nothing to service
      boot::set_idle_hook(&idle_service);
    }
  } else if (!g_gate.active()) {
    boot::set_idle_hook(nullptr);
  }
}

bool boot_disk() noexcept { return g_boot_disk; }

// The last step of the boot disk, after usb0::start(DISK) put the pull-up on (4.2: the layout first, the owner last): the card is handed to the computer with nothing mounted and started for it.
// The card's initialisation blocks (tens of ms, 3 s for a card that does not answer) with the display serviced; the computer's commands wait in the link's ring meanwhile (NAK).
// The disk is exported WRITABLE (the writable step, spec 10 [default]; the maintainer's read-only-first decision of 2026-10-16 ends here). The spec's READ ONLY row is a Config page row of the running
// mode (U2a): this mode reads no settings (nothing is mounted), so it has no way to hear one. A card that is write-protected itself is still exported protected (MscBridge::write_protected).
constexpr bool kBootDiskWritable = true;
void boot_disk_open() noexcept {
  if (!g_boot_disk) {
    return;
  }
  if (!storage::device::boot_to_host()) {
    console::log("boot: USB DISK: the card is already taken, nothing handed over");
    return;
  }
  service_while_blocked(true);
  (void)usb0::disk_open(kBootDiskWritable);
  service_while_blocked(false);
}

bool init(hal::Display& display) noexcept {
  power::device::init();  // OS-POWER P1: PTC3 as an input with the board's pull; read once a pass from here on (power_pass)
  // The input first: its chips scan while the bitmaps go up, so a key held at power-up is in their FIFOs when the app starts.
  const input::device::BringUp up = input::device::bring_up();
  g_input_up = true;
  g_input_degraded = !up.buttons_online;  // (OS-BUG-PADS: the pads are brought up later, after the audio start; their verdict is pads_gave_no_answer())
  const bool ok = start_app(display, true);
  g_start_failed = !ok;
  if (ok) {
    input::Scanner& scan = input::device::scanner();
    unsigned held = 0;  // OS-009b: what the scanner saw when the chord was read, in the log (`diag log`), because the chord did not open the screen on the unit (INP-8)
    std::uint32_t steps = 0;
    const std::uint64_t scan_began = system_clock().uptime_ms();
    if (O3OS_INPUT_TEST == 0) {
      // OS-DIAG1: a bounded poll (boot_chord.h), not a fixed 40 ms: ends when the three keys are down or after kBootChordWindowMs. Every pass services
      // both scanners, so the pads and the other buttons are in the picture the log line below gives.
      steps = poll_boot_chord(
          [&scan] {
            boot::delay_ms(1);
            scan.service();
          },
          [&scan] {
            unsigned bits = 0;
            for (unsigned i = 0; i < kBootChordKeys; ++i) {
              bits |= scan.button_held(kChordButtons[i]) ? 1u << i : 0u;
            }
            return bits;
          },
          held);
    }
    console::log_hex("boot: chord keys held (bit 0 Shift, 1 F1, 2 F8)", held);
    {
      // OS-DIAG1: which case the next unit run is in. buttons = every button the scanner holds down (bit n = button n, Shift is bit 29): 0 with a chord
      // physically held means the chip reported nothing for the held keys; a partial chord means some arrived; `ms` is the window used (150 = timed out).
      std::uint32_t buttons = 0;
      for (unsigned b = 0; b < 32u; ++b) {
        buttons |= scan.button_held(b) ? 1u << b : 0u;
      }
      const hal::InputStats stats = scan.stats();
      char line[96];
      audio::diag::Text t(line, sizeof line);
      t.str("boot: chord scan ").dec(steps).str(" steps, ").dec(static_cast<std::uint32_t>(system_clock().uptime_ms() - scan_began)).str(" ms, buttons down ").hex32(buttons)
          .str(", bus errors ").dec(stats.errors).str(", online ").dec(stats.online);
      console::log(line);
    }
    g_boot_chord = O3OS_INPUT_TEST != 0 || boot_chord_complete(held);
    // OS-USBDISK (b): Shift + F1 + F7, read when the INPUT TEST poll ended (it ran its whole window: that chord is not complete). INPUT TEST wins, and a test image never boots as a disk.
    g_boot_disk = O3OS_INPUT_TEST == 0 && !g_boot_chord && boot_disk_chord(held, scan.button_held(kDiskChordKey));
    if (g_boot_disk) {
      console::log("boot: chord Shift+F1+F7, USB DISK mode (the card is not mounted, nothing plays)");
    }
    if (g_boot_chord) {
      open_input_test();
    }
  }
  return ok;
}

namespace {

// One slice of a `diag mod load`: the loader is stepped from pass() like a project load's, unless a project load has the main loop's attention (it cancels this one).
#if !O3OS_AUDIO_TEST
void step_manual_module() noexcept {
  ModuleSystem& m = modsys();
  if (!m.manual) {
    return;
  }
  if (m.loader.step(kLoadBytesPerPass) == module::ModuleLoader::State::Running) {
    return;
  }
  m.manual = false;
  char line[120];
  audio::diag::Text t(line, sizeof line);
  if (m.loader.state() == module::ModuleLoader::State::Done) {
    const module::LoadedModule& lm = m.loader.module();
    t.str("mod: ").str(m.manual_name).str(" loaded: hot ").dec(lm.header.hot_size).str(lm.hot_in_sdram ? " B in SDRAM (the slab had no room), cold " : " B in the slab, cold ")
        .dec(lm.header.image_size - lm.header.hot_size).str(" B, state ").dec(lm.header.state_bytes).str(" B, from ").str(lm.builtin ? "the image" : "the card");
    if (lm.builtin && lm.card_status == module::Status::Older) {
      t.str(" (card ");
      put_older(t, lm);
      t.chr(')');
    } else if (lm.builtin && lm.card_status != module::Status::NotFound) {
      t.str(" (the card's copy: ").str(module::status_name(lm.card_status)).chr(')');
    }
  } else {
    t.str("mod: ").str(m.manual_name).str(" NOT loaded: ").str(module::status_name(m.loader.status()));
    if (module::is_validation_failure(m.loader.status())) {
      t.str(" (quarantined until reboot)");
    }
  }
  console::log(line);
}
#endif

// The LEDs of the pads that are down at the theme's front level; on the INPUT TEST screen the other pads at its back level. Handed to the
// driver only when something changed; flush() then sends one bounded piece a pass.
void update_pad_leds() noexcept {
  input::PadLeds& leds = input::device::leds();
  const std::uint64_t held = input::device::scanner().pads_held();
  const bool test = g_app != nullptr && g_app->state().screen == app::ScreenId::InputTest;
  const ui::ThemeId theme_id = g_app != nullptr ? g_app->theme() : ui::ThemeId::Amiga;
  const unsigned brightness = g_app != nullptr ? app::brightness_index(g_app->device_settings().get(model::device_setting::PadsBrightness)) : 2u;  // the setting PAD LIGHTS (OS-CFGWIRE)
  static_assert(hal::kPadLedCount == 48, "the screens give the roles of 48 pads (Screen::pad_roles)");
  app::PadRole roles[hal::kPadLedCount] = {};
  const bool lit = g_app != nullptr && g_app->pad_lights(roles);  // the screen's own roles (OS-011a): the slices' pads on Chop
  const bool roles_changed = lit != g_led_lit || (lit && std::memcmp(roles, g_led_roles, sizeof roles) != 0);
  if (held != g_led_pads || theme_id != g_led_theme || test != g_led_test || roles_changed || brightness != g_led_brightness) {
    g_led_brightness = brightness;
    g_led_pads = held;
    g_led_theme = theme_id;
    g_led_test = test;
    g_led_lit = lit;
    if (lit) {
      std::memcpy(g_led_roles, roles, sizeof roles);
    }
    const ui::Theme& theme = ui::theme(theme_id);
    const auto level = [&](app::PadRole role) { return app::scale_pad_level(app::pad_level(theme, role), brightness); };
    const std::uint8_t front = level(app::PadRole::Front);
    const std::uint8_t back = test ? level(app::PadRole::Back) : level(app::PadRole::Off);
    for (unsigned i = 0; i < hal::kPadLedCount; ++i) {
      const std::uint8_t idle = lit && !test ? level(roles[i]) : back;
      leds.set(i, ((held >> i) & 1u) != 0u ? front : idle);
    }
  }
  (void)leds.flush();
}

// ---- The card slot (OS-SDSWAP) -------------------------------------------------------------------------------------------------------
// A card taken out is released the moment its detect level has been settled for 250 ms (storage::CardSwap, polled here, never from an interrupt): every card call answers NotMounted
// from then on, a save or a load that was running finds that out at its next step and ends with its own message, and the strip says CARD REMOVED. A card put in is announced first
// (READING CARD) and mounted kCardMountDelayMs later, so that the announcement has been drawn before the mount blocks the main loop (the card's own initialisation: tens of ms, at most
// about 3 s for a card that does not answer; the display stays serviced from the idle hook, as at boot). The mount waits while a save, a load or the firmware update path has the card
// (a pull ends them within a pass or two), and a card that does not mount is not tried again until it has been taken out and put back. Audio never touches the card (the samples are in
// SDRAM, the modules in RAM), so nothing here reaches the render. Main loop only.
constexpr std::uint32_t kCardMountDelayMs = 2 * kFrameMs;
bool g_card_mount_pending = false;
std::uint32_t g_card_mount_at = 0;

bool card_mount_pending() noexcept { return g_card_mount_pending; }


void poll_card(std::uint32_t now) noexcept {
  if (g_boot_disk || g_power_latched) {
    return;  // (OS-POWER P1: the shutdown has the card; a slot that changes meanwhile mounts nothing)
  }
  switch (storage::device::poll(now)) {
    case storage::CardSwap::Event::Removed:
      g_card_mount_pending = false;
      console::log("sd: card removed, volume released");
      if (g_app != nullptr) {
        g_app->card_changed(app::CardEvent::Removed);
      }
      break;
    case storage::CardSwap::Event::Inserted:
      g_card_mount_pending = true;
      g_card_mount_at = now;
      console::log("sd: card inserted");
      if (g_app != nullptr) {
        g_app->card_changed(app::CardEvent::Reading);
      }
      break;
    case storage::CardSwap::Event::None:
      break;
  }
  if (!g_card_mount_pending || now - g_card_mount_at < kCardMountDelayMs) {
    return;
  }
  g_card_mount_pending = false;  // (the pending mount is itself a claim: it must not count against its own gate)
  if (storage::device::card_busy()) {
    g_card_mount_pending = true;  // a save, a load, a firmware step or an app job holds the card: ask again next pass
    return;
  }
  if (storage::device::owner() == storage::CardOwner::Host) {
    return;  // OS-USBDISK F1: the computer has the card; nothing mounts here (device::mount() would refuse as well): the eject's take_back() mounts it
  }
  const std::uint64_t started = system_clock().uptime_ms();
  service_while_blocked(true);
  const hal::storage::Status status = storage::device::mount();
  service_while_blocked(false);
  const auto took_ms = static_cast<std::uint32_t>(system_clock().uptime_ms() - started);  // how long the main loop stood still (the unit's figure for OS-SDSWAP)
  char line[56];
  audio::diag::Text text(line, sizeof line);
  if (status == hal::storage::Status::Ok) {
    const hal::storage::VolumeInfo volume = storage::device::storage().volume();
    text.str("sd: card mounted, ").str(hal::storage::fs_kind_name(volume.kind)).str(", ").dec(volume.capacity_mb).str(" MB, ").dec(took_ms).str(" ms");
  } else {
    text.str("sd: card NOT mounted: ").str(hal::storage::status_name(status)).str(", ").dec(took_ms).str(" ms");
  }
  console::log(line);
  if (g_app != nullptr) {
    g_app->card_changed(status == hal::storage::Status::Ok ? app::CardEvent::Ready : app::CardEvent::NotReadable);
  }
}

// ---- The boot disk (OS-USBDISK U1c, mode (b)) --------------------------------------------------------------------------------------------------
// The unit came up with the chord Shift + F1 + F7: no card mounted, no project, the outputs never opened, the DISK descriptor set on the bus. The main loop runs the MSC pump (usb0::disk_pass)
// and keeps one strip up that says what to do; a key or pad (after every key has been up once, so the chord that is still held does not count) restarts the unit: leaving is a reset, so there
// is no remount and no cache to invalidate. Since the disk is writable the key is two presses while a computer may be writing, and the restart waits for the command in flight (DiskRestart).
// The screen is the app's own with a persistent notice: a dedicated bare screen is a follow-up. Main loop only.
DiskRestart g_disk_restart;
const char* g_disk_text = nullptr;
std::uint32_t g_disk_text_at = 0;

bool any_key_down() noexcept {
  input::Scanner& scan = input::device::scanner();
  if (scan.pads_held() != 0u) {
    return true;
  }
  for (unsigned b = 0; b < app::kButtonCount; ++b) {
    if (scan.button_held(b)) {
      return true;
    }
  }
  return false;
}

void disk_boot_pass(std::uint32_t now) noexcept {
  usb0::disk_pass(now);
  if (!g_input_up) {
    return;
  }
  const usb0::DiskState d = usb0::disk_state();
  DiskRestart::Input in;
  in.key_down = any_key_down();
  in.at_risk = d.configured && d.present && !d.ejected && d.writable;  // a computer has the card and may be in the middle of a copy (d.writable: exported writable, so a card protected by itself is one press)
  in.busy = d.busy;
  if (!g_power_latched && g_disk_restart.update(now, in)) {  // (OS-POWER P1: a key pressed during the shutdown is ignored, not a restart in the middle of it)
    console::log(in.at_risk ? "boot: USB DISK left by a key (second press), restarting" : "boot: USB DISK left by a key, restarting");
    boot::reset_chip();
  }
  if (g_app == nullptr || g_power_latched) {
    return;
  }
  const DiskRestart::Prompt prompt = g_disk_restart.prompt(now);
  // OS-KEYROWDLG S3: the key row says what a key does: every key RESTART, AGAIN? inverted while the computer may still be writing, blank while the restart waits (the app's own Pattern row, PATTERN .. UNDO, used to stay).
  g_app->set_disk_keys(prompt == DiskRestart::Prompt::Confirm ? app::DiskKeys::Confirm : (prompt == DiskRestart::Prompt::Finishing ? app::DiskKeys::Finishing : app::DiskKeys::Restart));
  const char* text = prompt == DiskRestart::Prompt::Confirm     ? "THE COMPUTER MAY STILL BE WRITING: KEY AGAIN"
                     : prompt == DiskRestart::Prompt::Finishing ? "FINISHING THE LAST WRITE - RESTARTING"
                     : d.ejected                                ? "DISK EJECTED - PRESS ANY KEY TO RESTART"
                     : !d.present                               ? "USB DISK - NO CARD - PRESS ANY KEY TO RESTART"
                                                                : "USB DISK - EJECT ON THE COMPUTER OR PRESS A KEY";
  if (text != g_disk_text || now - g_disk_text_at >= 1000u) {
    g_disk_text = text;
    g_disk_text_at = now;
    g_app->disk_notice(text, 2500, prompt == DiskRestart::Prompt::Confirm);
  }
}

// ---- The running mode (OS-USBDISK U2a, mode (a)) -----------------------------------------------------------------------------------------------------
// The card goes to the computer while the unit plays on: DiskShare (disk_share.h) is the state machine, this is its port over storage::device and usb0, and disk_share_pass() the one call a pass makes. Entry for the
// unit: the console's `disk share [ro]` / `disk eject` (the Config row is U2b). Everything here is the main loop; the USB interrupt is untouched.
class DiskPort final : public DiskShare::Port {
 public:
  storage::CardOwner owner() const noexcept override { return storage::device::owner(); }
  bool card_busy() const noexcept override { return storage::device::card_busy(); }
  bool save_pending() const noexcept override {
#if !O3OS_AUDIO_TEST
    return g_seq != nullptr && save_host()->service.write_due();
#else
    return false;
#endif
  }
  DiskShare::Layout layout() const noexcept override { return DiskShare::Layout{usb0::mode(), usb0::profile(), usb0::return_wanted()}; }  // (the wish: leaving DISK hands it back)
  bool layout_changing() const noexcept override { return usb0::change_phase() != 0u; }
  bool enter_disk_layout(const DiskShare::Layout& to) noexcept override { return usb0::disk_layout_enter(to.profile, to.usb_return); }
  bool leave_disk_layout(const DiskShare::Layout& to) noexcept override { return usb0::disk_layout_leave(to.mode, to.profile, to.usb_return); }
  bool change_layout(const DiskShare::Layout& to) noexcept override { return usb0::request_layout(to.mode, to.profile, to.usb_return); }  // (DISK is not presented: the attach fell back to STANDARD)
  bool configured() const noexcept override { return usb0::disk_state().configured; }
  storage::CardSwap::HandOver hand_to_host() noexcept override {
    service_while_blocked(true);  // (reads the volume's dirty flag before the card goes: three sectors)
    const storage::CardSwap::HandOver r = storage::device::hand_to_host();
    service_while_blocked(false);
    return r;
  }
  bool disk_open(bool writable) noexcept override {
    service_while_blocked(true);  // (the card's initialisation blocks: the display stays serviced)
    const bool opened = usb0::disk_open(writable);
    service_while_blocked(false);
    return opened;
  }
  void disk_close() noexcept override { usb0::disk_close(); }
  void pump(std::uint32_t now_ms) noexcept override { usb0::disk_pass(now_ms); }  // the SCSI state machine (and the card slot) for the computer
  void hold_background(bool on) noexcept override {
#if !O3OS_AUDIO_TEST
    if (g_seq != nullptr) {
      save_host()->service.hold_background(on);  // 4.2: no settings or recent write starts while the card is being handed over; Host holds them by the gate (Q9)
    }
#else
    (void)on;
#endif
  }
  hal::storage::Status take_back() noexcept override {
    service_while_blocked(true);
    const hal::storage::Status s = storage::device::take_back();
    service_while_blocked(false);
    return s;
  }
  bool engine_busy() const noexcept override { return usb0::disk_state().busy; }
  bool take_eject_request() noexcept override { return usb0::disk_take_eject_request(); }
  void unit_eject() noexcept override { usb0::disk_unit_eject(); }
  void log(const char* line) noexcept override { console::log(line); }
};
DiskPort g_disk_port;
DiskShare g_disk_share(g_disk_port);
std::uint32_t g_disk_strip_at = 0;

void disk_share_pass(std::uint32_t now) noexcept {
#if !O3OS_AUDIO_TEST
  if (g_seq != nullptr) {
    const audio::StatusSnapshot st = audio_status().snapshot();
    usb0::set_render_load(g_power_latched ? 0u : static_cast<unsigned>(st.render_last / (hal::audio::kCyclesPerBlock / 100u)));  // Q11: the pump is held above 85 % of the block budget (not by a render that the shutdown mutes)
  }
#endif
  if (g_app != nullptr && !g_power_latched) {  // the Config > USB page's CARD AS DISK row (U2b): the second F7 left a request; a refusal the machine makes comes back as the strip's own text (the row already checked the first press)
    using Request = app::ViewState::DiskView::Request;
    const Request request = g_app->take_disk_request();
    switch (request) {
      case Request::ShareReadOnly:
      case Request::ShareWritable:  // the policy is the one READ ONLY had at the second F7, not what the row says when this pass runs
      {
        const DiskShare::Refusal refusal = g_disk_share.share(now, request == Request::ShareWritable);
        if (refusal != DiskShare::Refusal::None) {
          g_app->disk_notice(DiskShare::refusal_text(refusal), 4000, true);
        }
        break;
      }
      case Request::Eject:
        (void)g_disk_share.eject(now);
        break;
      case Request::None:
        break;
    }
  }
  g_disk_share.pass(now, g_power_latched);  // the pump, the step and the save service's hold; under the power latch the pump of a card that is out goes on (the shutdown waits for the command in flight, stage C)
  if (g_power_latched) {
    return;
  }
  const DiskShare::Event e = g_disk_share.poll_event();
  if (g_app == nullptr) {
    return;
  }
  g_app->set_card_on_computer(g_disk_share.out());  // 4.3: the listings and the read refusals say CARD ON COMPUTER while it is out
  g_app->set_disk_state(g_disk_share.state() == DiskShare::State::Out ? app::ViewState::DiskView::Phase::Out
                                                                      : (g_disk_share.idle() ? app::ViewState::DiskView::Phase::Idle : app::ViewState::DiskView::Phase::Working),
                        storage::device::card_busy(), g_disk_port.layout().mode == usb::Mode::Audio, usb0::disk_state().writable);  // the row's state (an eject in progress is Working: the keys wait); writable: what the computer has
  switch (e.kind) {
    case DiskShare::Event::Kind::None:
      break;
    case DiskShare::Event::Kind::Shared:
      g_disk_strip_at = now - 1000u;  // the persistent strip, below
      break;
    case DiskShare::Event::Kind::Ejected:
      g_app->card_changed(app::CardEvent::Ready);  // hazard 13: the remount ends with the event that revalidates what the OS believes about the card
      if (storage::device::swap().remount_newly_dirty()) {  // 6.3 (F5): a warning after a Host session only, and only when the session left the flag set (a card that was dirty at the share does not warn each time); everything works
        g_app->disk_notice("CARD NOT EJECTED PROPERLY: CHECK THE COMPUTER", 8000, true);
      }
      break;
    case DiskShare::Event::Kind::EjectedNotReadable:
      g_app->card_changed(app::CardEvent::NotReadable);
      break;
    case DiskShare::Event::Kind::Failed:
      g_app->disk_notice(DiskShare::fail_text(e.fail), 4000, true);
      if (e.fail == DiskShare::Fail::Open) {  // the card did not start after the flip: it was given back, so it was remounted
        g_app->card_changed(storage::device::storage().mounted() ? app::CardEvent::Ready : app::CardEvent::NotReadable);
      }
      break;
  }
  if (g_disk_share.out() && now - g_disk_strip_at >= 1000u) {
    g_disk_strip_at = now;
    g_app->disk_notice(app::App::kCardOnComputerStrip, 2500);
  }
}

// OS-POWER P1 (docs/specs/OS-POWER.md section 3): the Power switch, once a pass: the pin into core::PowerHold, the shutdown stepped, the line and the strip told to the app. Called after the key
// scan (Shift is read from it) and before anything that can return early. The hold counts only once the app has the screen (the boot gate has let go) and no project is loading.
void power_pass(std::uint32_t now) noexcept {
  const bool shift = g_input_up && input::device::scanner().button_held(static_cast<unsigned>(app::Button::Shift));
  const bool ready = g_app != nullptr && !g_start_failed && !g_gate.active() && g_load_phase == LoadPhase::Idle;
  const core::PowerShow show = power::device::pass(now, shift, ready);
  if (g_app != nullptr) {
    g_app->set_power(show);
  }
}

#if !O3OS_AUDIO_TEST
// OS-SCREENSHOT: the Power switch's Shift-then-Power event arms the service; the service is stepped every pass. true: hold the app's frames this pass. Not while the shutdown runs (D2).
bool shot_pass(std::uint32_t now) noexcept {
  const bool requested = power::device::take_screenshot_request();
  if (g_seq == nullptr || g_app == nullptr) {  // (the sound, and with it the save layer, is built)
    return false;
  }
  ShotStore& s = *g_shot;
  if (!g_shot_attached) {
    s.service.attach(s.host, storage::device::storage(), save_host()->writer, s.buffer, ui::plot::kRamGShot, ui::plot::kRamGShotBytes);
    g_shot_attached = true;
  }
  if (requested && !g_power_latched) {
    s.service.request(now);
  }
  const bool keys = (g_input_up && input::device::scanner().button_held(static_cast<unsigned>(app::Button::Shift))) || power::device::pressed();
  return s.service.step(now, keys, console::screenshot_port());
}
#endif

}  // namespace

// OS-USBDISK U2a, the console's `disk` (internal.h): nullptr when the request went through, else the refusal in words.
const char* disk_share_request(bool read_only) noexcept {
  if (g_boot_disk) {
    return "the boot disk is already out (restart to leave it)";
  }
  if (g_power_latched) {
    return "the unit is shutting down";  // 7: the shutdown has the card
  }
  switch (g_disk_share.share(static_cast<std::uint32_t>(system_clock().uptime_ms()), !read_only)) {
    case DiskShare::Refusal::None: return nullptr;
    case DiskShare::Refusal::NotIdle: return "a hand-over is already running or the disk is out";
    case DiskShare::Refusal::NoCard: return "no card for the unit to give";
    case DiskShare::Refusal::Busy: return "the card is in use (a load, a save, an export, an upload)";
    case DiskShare::Refusal::SaveBusy: return "a save is running";
  }
  return "refused";
}
const char* disk_eject_request() noexcept {
  return g_disk_share.eject(static_cast<std::uint32_t>(system_clock().uptime_ms())) == DiskShare::Refusal::None ? nullptr : "the disk is not out";
}
const char* disk_share_state_name() noexcept {
  switch (g_disk_share.state()) {
    case DiskShare::State::Idle: return g_boot_disk ? "boot disk" : (g_disk_share.layout_stuck() ? "idle (layout not restored)" : "idle");
    case DiskShare::State::Quiesce: return "waiting for a save";
    case DiskShare::State::Layout: return "switching the USB set";
    case DiskShare::State::Configure: return "waiting for the computer";
    case DiskShare::State::Out: return "out";
    case DiskShare::State::Draining: return "ejecting";
    case DiskShare::State::Returning: return "restoring the USB set";
  }
  return "?";
}

// OS-BUG-PADS: what the pad bus said, in `diag log`: three short lines (the headline, the chips' results, then the bus clears and the lines) at the first answer or refusal of each chip and for the
// first changes of the pad scanner's online state, then one line per input::PadBusLogPolicy::kSummaryEvery changes (a flapping chip must not wrap the log over the boot lines). Read only. Main thread.
// Every pass looks at the record alone (no pin is read); the levels are read, once, for a full entry.
input::PadBusLogPolicy g_pad_bus_log;
void log_pad_bus(const hal::PadBusStats& bus) noexcept {
  char line[64];
  console::format_pad_bus_chips(bus, line, sizeof line);
  console::log(line);
  console::format_pad_bus_lines(bus, line, sizeof line);
  console::log(line);
}
void watch_pad_bus() noexcept {
  const hal::InputStats seen = input::device::scanner().stats_without_levels();
  const bool online = (seen.online & hal::InputStats::kPadsOnline) != 0u;
  switch (g_pad_bus_log.update(online, seen.pad_bus.scanner.count != 0u, seen.pad_bus.leds.count != 0u)) {
    case input::PadBusLogPolicy::Entry::None:
      break;
    case input::PadBusLogPolicy::Entry::Full:
      console::log(online ? "pads: scanner answers" : "pads: scanner not answering");
      log_pad_bus(input::device::scanner().stats().pad_bus);
      break;
    case input::PadBusLogPolicy::Entry::Summary: {
      char line[64];
      audio::diag::Text text(line, sizeof line);
      text.str("pads: ").dec(g_pad_bus_log.flips()).str(" changes, now ").str(online ? "answering" : "not answering");
      console::log(line);
      break;
    }
  }
}

std::uint8_t pass() noexcept {
  poll_card(static_cast<std::uint32_t>(system_clock().uptime_ms()));  // before the driver's scope: the mount's waits keep the display serviced through the idle hook
  if (g_input_up) {  // a headless run (the display did not come up) never starts the input: its I2C clocks are off, a register access would fault
    input::device::scanner().service();  // before anything that can return early: the keys are scanned whatever the app does
    update_pad_leds();
    watch_pad_bus();
  }
  power_pass(static_cast<std::uint32_t>(system_clock().uptime_ms()));
  if (g_boot_disk) {
    disk_boot_pass(static_cast<std::uint32_t>(system_clock().uptime_ms()));
  } else {
    disk_share_pass(static_cast<std::uint32_t>(system_clock().uptime_ms()));  // OS-USBDISK U2a: the running mode
  }
  if (g_display_off) {
    return 0;  // OS-POWER P1, the end of stage E: the display is dark and held and the chip is in power-down: nothing below touches it
  }
  display::Ft812Display& screen = display::device::display();
  BootGate::DriverScope in_driver(g_gate);  // the service, the app's frames and a recovery all enter the driver, which may wait
  screen.service();  // also with no app: a display that is up still raises its backlight
  if (screen.failed()) {
    shot_cancel();  // (a capture in flight would keep its file and the card claim: this pass returns before shot_pass)
    return blink::kDisplayStalled;  // service() gave the display up (held dark): the swap stalled, a re-init did not help
  }
  if (g_start_failed) {
    shot_cancel();
    return blink::kAppFailure;
  }
  if (g_app == nullptr) {
    return 0;
  }
  const std::uint32_t backlight_generation = g_app->device_settings().generation(model::device_setting::DisplayBrightness);
  if (backlight_generation != g_backlight_generation) {  // the setting DISPLAY (OS-CFGWIRE: Config, a bound controller, the settings file): the backlight goes to its level in the driver's ramp
    g_backlight_generation = backlight_generation;
    screen.set_backlight_target(app::backlight_duty(app::brightness_index(g_app->device_settings().get(model::device_setting::DisplayBrightness))));
  }
  if (screen.needs_assets()) {  // the display was re-initialised after a stall: its RAM_G is empty, the app starts over
    console::log("boot: display re-initialised, the app starts over (the screen is the Pattern's again)");  // OS-009b: INP-8 evidence
    g_gate.assets_lost();  // the logo went with the chip's memory: the app takes the screen now (no frame waits behind a hold)
    boot::set_idle_hook(nullptr);
    g_boot_chord = g_boot_chord && g_app != nullptr && g_app->state().screen == app::ScreenId::InputTest;  // only a unit still on INPUT TEST comes back to it
    if (!start_app(screen, false)) {
      g_start_failed = true;
      screen.hold_dark();
      return blink::kAppFailure;
    }
    if (g_boot_chord) {
      open_input_test();  // OS-DIAG1: the rebuilt app is on the Pattern screen; the chord asked for INPUT TEST
    }
    if (g_power_latched) {
      g_app->power_begin();  // OS-POWER P1: a display that stalled during the shutdown gave a new app: it is in its shutdown mode too (its save layer is the same object)
    }
    screen.assets_loaded();
  }
  // OS-007e/f: a project is being loaded: a slice of it now (the settle, the project, or the samples), and no app frame until it has ended (the screen keeps
  // its last picture; the display is serviced above, every pass).
  const std::uint64_t now = system_clock().uptime_ms();
  if (g_load_phase != LoadPhase::Idle) {
    advance_load(now);
  }
#if !O3OS_AUDIO_TEST
  sync_midi_settings();
  sync_usb_return();  // OS-USBRET
  sync_metro();       // OS-METRO
  sync_project_platform_state(g_load_phase != LoadPhase::Idle);  // OS-004d
  if (g_seq != nullptr) {  // (the sound, and with it the capture device, is built)
    capture_at()->service(now);  // OS-011c: the radio's state machine, one bounded step (nothing while it is off)
  }
  poll_midi_cc(g_load_phase != LoadPhase::Idle);
  step_manual_module();
  if (g_probe_ms != 0u && now >= g_probe_ms) {
    g_probe_ms = 0;
    log_play_probe();
  }
  if (g_seq != nullptr) {
    log_song_row();
    log_switch_landing();  // OS-PATSWITCH
  }
#endif
  const bool loading = g_load_phase != LoadPhase::Idle;
  bool frames_held = false;  // OS-SCREENSHOT: a capture is under way: no new frame, so that both bands are one picture
#if !O3OS_AUDIO_TEST
  frames_held = shot_pass(static_cast<std::uint32_t>(now));
  console::hold_input(frames_held);  // OS-SCREENSHOT, spec Q4: presses and encoder turns during the capture are dropped, the releases of keys already down are kept for after
#endif
  switch (g_gate.pass(screen, now)) {
    case BootGate::Verdict::Hold:
      return 0;  // the boot screen stays; keys pressed meanwhile wait in the scanner's queue (its full-queue policy loses no state)
    case BootGate::Verdict::Released:
      // The minimum has passed (and the boot work with it): the app takes the screen with its first frame, this pass.
      boot::set_idle_hook(nullptr);
      g_last_tick_ms = now - kFrameMs;
#if O3OS_DEMO_CYCLE
      g_last_demo_ms = now;
#endif
      break;
    case BootGate::Verdict::Run:
      break;
  }
  if (now - g_last_tick_ms >= kFrameMs) {
    g_last_tick_ms = now;
    if (!loading && !frames_held) {
      g_app->run_once();
    }
  }
#if O3OS_DEMO_CYCLE
  if (now - g_last_demo_ms >= kDemoStepMs) {
    g_last_demo_ms = now;
    demo_step();
  }
#endif
  return 0;
}


// The clock follow's state for a status line (locked, tempo, clocks, error), a snapshot a block old at most.
midi::SyncStatus midi_status() noexcept {
#if !O3OS_AUDIO_TEST
  return g_midi != nullptr ? g_midi->status() : midi::SyncStatus{};
#else
  return midi::SyncStatus{};
#endif
}

// OS-HIL (usb/console.cpp: `state`, `key`, `shot`). The App is the main loop's and so is the console: nothing here needs a lock.
std::size_t state_line(char* out, std::size_t cap) noexcept {
  if (g_app == nullptr) {
    static const char kDown[] = "app=down";
    const std::size_t n = cap != 0u && sizeof kDown - 1u > cap - 1u ? cap - 1u : sizeof kDown - 1u;
    if (cap != 0u) {
      std::memcpy(out, kDown, n);
      out[n] = '\0';
    }
    return cap != 0u ? n : 0u;
  }
  return app::describe_state(*g_app, out, cap);
}

bool enc_accel_get(EncAccelNumbers& out) noexcept {
  if (g_app == nullptr) {
    return false;
  }
  const app::EncAccel::Config& c = g_app->enc_accel().cfg;
  out = EncAccelNumbers{c.on, c.t1, c.t2, c.gap_ms};
  return true;
}

bool enc_accel_set(const EncAccelNumbers& in) noexcept {
  if (g_app == nullptr) {
    return false;
  }
  app::EncAccel::Config c;
  c.on = in.on;
  c.t1 = in.t1;
  c.t2 = in.t2;
  c.gap_ms = in.gap_ms;
  if (in.on && !app::EncAccel::valid(c)) {
    return false;
  }
  if (!in.on) {  // `off` keeps the numbers it had
    c = g_app->enc_accel().cfg;
    c.on = false;
  }
  g_app->enc_accel().cfg = c;
  g_app->enc_accel().reset();
  return true;
}

int button_code(const char* name) noexcept { return app::button_code(name); }

bool screenshot_active() noexcept {
#if !O3OS_AUDIO_TEST
  return g_shot_attached && g_shot->service.active();
#else
  return false;
#endif
}

void shot_region(std::uint32_t& addr, std::uint32_t& bytes) noexcept {
  static_assert(ui::plot::kRamGSelfTestBytes == display::kSelfTestBytes && ui::plot::kRamGShot + ui::plot::kRamGShotBytes == display::kSelfTestAddr,
                "the map's screenshot region ends where the display driver's DMA self-test scratch begins");
  addr = ui::plot::kRamGShot;
  bytes = ui::plot::kRamGShotBytes;
}

}  // namespace o3os::platform::mk66::appimg

// OS-SRAMU: the plots' working memory (ui::plot::CurveScratch, 4,480 B). It is written and read only by the screens' draw, which runs from the main loop (never the render, an ISR or
// a DMA), and every use fills it before it reads it; so it lives in the SDRAM slack window, 4.5 KB off SRAM_U (SdramBox zeroes it, as .bss did). app/CMakeLists.txt sets
// O3OS_CURVE_SCRATCH_EXTERNAL for this image, which drops app's own copy.
namespace o3os::app {
namespace {
O3OS_SDRAM_NOINIT platform::mk66::SdramBox<ui::plot::CurveScratch> g_curve_scratch;
}  // namespace
ui::plot::CurveScratch& params_curve_scratch() noexcept { return *g_curve_scratch; }
}  // namespace o3os::app
