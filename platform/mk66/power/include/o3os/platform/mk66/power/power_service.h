// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-POWER P1 (docs/specs/OS-POWER.md section 4): the shutdown that a hold of Power (core/power_hold.h) or `diag power off` starts, as a pure state machine over a PowerHost. The shape of
// update/fw_service.h: bounded steps, deadlines by the millisecond clock, nothing waits. step() is called once per pass of the main loop, after PowerHold::update().
//
//   Idle     waiting for a commit. At the commit the refusals (4.2) are asked and, if none, the stages begin. A refusal changes nothing: the line empties, the strip says why for 3 s.
//   Quiet    A   the engine is stopped (the transport, the live notes, the metronome), the power-off latch is set, the input is locked; wait kQuietMs for the Stop to reach the render.
//   Mute     A2  the codec is muted through its soft ramp, the amplifier key goes LOW, the I2S and the eDMA stop (PowerHost::audio_down, bounded, blocks the main loop for about 570 ms (OS-BUG-POWERPOP: 50 ms soft mute, 500 ms VAG ramp)).
//   Card     B   the device settings are flushed; a save in flight is waited for up to kSaveWaitMs, then cancelled (unless it is committing); a job that is committing is waited for up to kCardHardCapMs.
//   Unmount  C   the card is released once card_busy() is false, or at kCardHardCapMs (counted from the start of B).
//   Usb      D   the log line, kUsbDrainMs for the DIN out ring and the CDC to leave, then USB off.
//   Lights   E   the pad LEDs, the backlight and the display chip go dark (PowerHost::lights_out, polled).
//   Off      F   the amplifier key LOW, then F0: wait for a stable release of Power (the user is usually still holding it), then PowerHost::enter_off (slice 1: the restart).
//
// After the commit the keys, pads, MIDI and the console are locked (locked()) and nothing else ends a stage that hangs: kGlobalCapMs from the start of stage A abandons every stage that
// has not finished and goes to F (the card is released only if it is not busy). F0 is outside it: it waits for the user. A host call must return on its own; the deadline bounds this machine, not a
// driver. Main loop only.

#include <o3os/core/power_hold.h>
#include <o3os/platform/mk66/power/power_record.h>

#include <cstdint>

namespace o3os::platform::mk66::power {

inline constexpr std::uint32_t kQuietMs = 10;           // more than 6 render blocks of 1.45 ms: the Stop reaches the render
inline constexpr std::uint32_t kSaveWaitMs = 5000;      // a save in flight is waited for this long (from the start of B), then cancelled
inline constexpr std::uint32_t kCardHardCapMs = 8000;   // B and C together, from the start of B; a job that is committing is waited for up to here, the card is released at it
inline constexpr std::uint32_t kUsbDrainMs = 100;       // the MIDI DIN out ring (16 note-offs at 31.25 kbaud: 50 ms) and the CDC (50 ms) leave in parallel
inline constexpr std::uint32_t kLightsCapMs = 600;      // the backlight's fade (about 150 ms) with room; a display that never finishes does not hold the stage
inline constexpr std::uint32_t kGlobalCapMs = 10000;    // from the start of stage A to the jump to F
inline constexpr std::uint32_t kRefusalShowMs = 3000;   // the strip text of a refusal
inline constexpr std::uint32_t kSecondHoldMs = 10000;   // a second completed hold within this stops an export and runs the stages
inline constexpr std::uint32_t kWaitLogMs = 5000;       // F0: the log line "waiting for Power to be released"

enum class Stage : std::uint8_t { Idle, Quiet, Mute, Card, Unmount, Usb, Lights, Off, Done };
const char* stage_name(Stage s) noexcept;

// What the image does for the shutdown, and the tests fake. Calls with a side effect are made at most once each; the polled questions may be asked every pass. Main loop only.
class PowerHost {
 public:
  // Stage 0: the refusals.
  virtual bool firmware_active() noexcept = 0;  // the firmware update path holds the card or waits for the bootloader
  virtual bool export_running() noexcept = 0;   // a WAV export, a set of stems, an OG project export
  virtual bool disk_writing() noexcept = 0;     // the boot disk is writable (a computer may be writing); read-only: false
  virtual void export_cancel() noexcept = 0;    // the second hold: the export's normal cancel
  // A: sets the power-off latch FIRST (nothing reopens the outputs, the audio stall detector stands down), locks the input, stops the transport, the live notes, the metronome and the app's jobs.
  virtual void quiesce_engine() noexcept = 0;
  // A2: the click-free mute and the audio stop. Blocks the main loop for about 570 ms (I2C and the two ramps), the display serviced meanwhile.
  virtual void audio_down() noexcept = 0;
  // B: the device settings are written now rather than 1.5 s after the last change (asked once, at the start of B). save_busy(): a save job (or the settings flush) runs. save_committing(): it has begun to
  // read back or rename and cannot be cancelled. save_cancel(): the job in flight is cancelled (its Temp removed; the old file stays whole) and no settings flush follows.
  virtual void settings_flush() noexcept = 0;
  virtual bool save_busy() noexcept = 0;
  virtual bool save_committing() noexcept = 0;
  virtual void save_cancel() noexcept = 0;
  // C: card_busy() is the OR of every claim on the card (storage::device::card_busy()); card_release() is the unmount (the MSC close in the boot disk mode).
  virtual bool card_busy() noexcept = 0;
  virtual void card_release() noexcept = 0;
  // D: usb_drain() writes the `power: off` line and asks the CDC to flush; usb_down() detaches (pull-up off, streams and the stem tap stopped, the interrupt masked).
  virtual void usb_drain() noexcept = 0;
  virtual void usb_down() noexcept = 0;
  // E: lights_out() begins (pad LEDs dark and in shutdown, the backlight to 0 through the driver's ramp, then held dark and the FT812 in power-down); lights_done() polls it.
  virtual void lights_out() noexcept = 0;
  virtual bool lights_done() noexcept = 0;
  // F: the amplifier key LOW (a GPIO write, cannot hang), then enter_off(): slice 1 the restart (SYSRESETREQ, never returns on the image); `dry` is `diag power off dry`.
  virtual void amp_key_low() noexcept = 0;
  virtual void enter_off(bool dry) noexcept = 0;
  virtual void log(const char* text) noexcept = 0;  // one line (55 characters) into the diag log

 protected:
  PowerHost() = default;
  ~PowerHost() = default;
  PowerHost(const PowerHost&) = delete;
  PowerHost& operator=(const PowerHost&) = delete;
};

// Why a console request (`diag power off`) did not start.
enum class Refusal : std::uint8_t { None, Firmware, Export, Disk, Running, Held };  // Held: Power is not stably released (a finger on it, a switch that reads pressed: a wrong polarity constant)

class PowerService {
 public:
  // `record` (optional): where the run is written down as it goes (power_record.h), in memory that survives the reset that ends the shutdown. The image passes its noinit record; nullptr: none.
  PowerService(PowerHost& host, core::PowerHold& hold, ShutdownRecord* record = nullptr) noexcept : host_(host), hold_(hold), rec_(record) {}

  // Once per pass, after hold.update(): `hr` is its result. A commit starts the stages (or is refused); the stage in force is stepped.
  void step(std::uint32_t now_ms, const core::PowerHold::Result& hr) noexcept;

  // `diag power off [dry]`: the stages without a hold. The refusals apply, with no second-hold rule (the console says why), and so does Held: F0 would wait for a release that a wrong polarity
  // constant never gives, leaving the unit dark and awake with the cable as the only way out (the first run on the unit). None: it started.
  Refusal request(std::uint32_t now_ms, bool dry) noexcept;

  // The line and the strip the app is told this pass: the shutdown's own once it runs, a refusal's for 3 s, else the hold's.
  core::PowerShow show(std::uint32_t now_ms, const core::PowerHold::Result& hr) const noexcept;

  // The one-way commit (D2): the stages are running. The input and the console are locked from here.
  bool locked() const noexcept { return stage_ != Stage::Idle; }
  Stage stage() const noexcept { return stage_; }
  bool dry() const noexcept { return dry_; }
  bool deadline_hit() const noexcept { return deadline_hit_; }
  bool cancelled_save() const noexcept { return save_cancelled_; }

 private:
  void on_commit(std::uint32_t now_ms) noexcept;
  Refusal check(std::uint32_t now_ms, bool console) noexcept;
  void begin(std::uint32_t now_ms, bool dry) noexcept;
  void enter(Stage s, std::uint32_t now_ms) noexcept;
  void refuse(core::PowerText text, std::uint32_t now_ms) noexcept;
  void abandon(std::uint32_t now_ms) noexcept;
  void release_card(std::uint32_t now_ms) noexcept;
  void note_refusal(Refusal why) noexcept {
    if (rec_ != nullptr) {
      rec_->refused(static_cast<std::uint8_t>(why));
    }
  }
  void note(std::uint8_t bits) noexcept {
    if (rec_ != nullptr) {
      rec_->note(bits);
    }
  }

  PowerHost& host_;
  core::PowerHold& hold_;
  ShutdownRecord* rec_;
  Stage stage_ = Stage::Idle;
  bool dry_ = false;
  std::uint32_t t_a_ = 0;        // the start of stage A
  std::uint32_t t_stage_ = 0;    // the start of the stage in force
  std::uint32_t t_b_ = 0;        // the start of B (the card's caps count from it)
  bool saving_ = false;          // a save job was in flight when the shutdown began (before the settings flush is asked for: that one is quick and quiet): the strip says so in B
  bool save_cancelled_ = false;
  bool card_released_ = false;
  bool deadline_hit_ = false;
  bool wait_logged_ = false;
  bool off_done_ = false;
  bool export_cancelled_ = false;  // the second hold stopped an export: the record of the run says so
  // The refusal on show, and the refusal that a second hold within kSecondHoldMs turns into a go.
  core::PowerText refusal_text_ = core::PowerText::None;
  std::uint32_t refusal_until_ = 0;
  core::PowerText armed_text_ = core::PowerText::None;
  std::uint32_t armed_at_ = 0;
};

}  // namespace o3os::platform::mk66::power
