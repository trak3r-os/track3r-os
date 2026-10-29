// SPDX-License-Identifier: GPL-3.0-or-later
// OS-USBRET: the device setting USB RETURN kept in step with the USB driver's return flag (app/usb_return.h), against a fake driver and the real settings block: the
// settings file's value applied at the first pass, a busy driver asked again, a console change copied back to the setting, and a Config change carried to the driver.
// OS-USBAUDIO: the boot decision (setting, layout, return -> the descriptor set of the first enumeration) and the setting USB AUDIO kept in step with the driver's mode, with USB RETURN beside it.
#include <doctest/doctest.h>

#include <o3os/app/usb_return.h>
#include <o3os/audio/graph_params.h>
#include <o3os/core/param.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>
#include <o3os/usb/usb_types.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

namespace ds = o3os::model::device_setting;
using o3os::app::UsbReturnSync;

// The driver as the MK66's usb0 behaves: it presents one return flag; a request for another value starts a change that takes `busy_passes` passes (during which target() is the new
// value and further requests are refused), or is refused while a change is already in flight.
struct FakeDriver {
  bool attached = true;
  bool presented = true;   // what the descriptors carry now
  bool changing = false;
  bool pending = true;
  unsigned busy_left = 0;
  unsigned busy_passes = 0;
  unsigned requests = 0;   // calls of request()
  unsigned accepted = 0;   // the ones that took effect (a different value or the same one, not refused)
  unsigned reenumerations = 0;

  bool ready() const { return attached; }
  bool disk() const { return false; }
  bool target() const { return changing ? pending : presented; }
  bool request(bool on) {
    ++requests;
    if (changing) {
      return false;
    }
    ++accepted;
    if (on != presented) {
      ++reenumerations;
      if (busy_passes != 0u) {
        changing = true;
        pending = on;
        busy_left = busy_passes;
      } else {
        presented = on;
      }
    }
    return true;
  }
  // The driver's own progress, once a pass (usb0::service()).
  void tick() {
    if (changing && --busy_left == 0u) {
      changing = false;
      presented = pending;
    }
  }
  // The console's `usb return on|off`: a request that does not go through the setting.
  bool console(bool on) { return request(on); }
};

o3os::model::Settings device() {
  o3os::model::Settings s;
  s.init(o3os::core::ParamKind::Device);
  return s;
}

void pass(UsbReturnSync& sync, o3os::model::Settings& s, FakeDriver& d) {
  o3os::app::sync_usb_return(sync, s, d);
  d.tick();
}

}  // namespace

TEST_CASE("usb return sync: the settings file's value is applied at the first pass, and nothing is asked when they already agree") {
  // The file said Off (the setting differs from the driver's boot value On): the first pass asks the driver, once.
  o3os::model::Settings s = device();
  CHECK(s.set(ds::UsbReturn, 0.0f) == o3os::model::Status::Ok);
  FakeDriver d;
  UsbReturnSync sync;
  pass(sync, s, d);
  CHECK(d.requests == 1u);
  CHECK_FALSE(d.presented);
  for (int i = 0; i < 5; ++i) {
    pass(sync, s, d);
  }
  CHECK(d.requests == 1u);  // settled: no further request, no write back
  CHECK(s.get(ds::UsbReturn) == 0.0f);

  // The file said On (the default, or a file from before the setting): driver and setting agree, so nothing is asked, ever.
  o3os::model::Settings on = device();
  FakeDriver d2;
  UsbReturnSync sync2;
  for (int i = 0; i < 5; ++i) {
    pass(sync2, on, d2);
  }
  CHECK(d2.requests == 0u);
  CHECK(d2.presented);
  CHECK(on.get(ds::UsbReturn) == 1.0f);

  // A driver that is not running decides nothing and the setting is not consumed: the first pass after it starts applies it.
  o3os::model::Settings late = device();
  CHECK(late.set(ds::UsbReturn, 0.0f) == o3os::model::Status::Ok);
  FakeDriver d3;
  d3.attached = false;
  UsbReturnSync sync3;
  pass(sync3, late, d3);
  pass(sync3, late, d3);
  CHECK(d3.requests == 0u);
  d3.attached = true;
  pass(sync3, late, d3);
  CHECK(d3.requests == 1u);
  CHECK_FALSE(d3.presented);
}

TEST_CASE("usb return sync: a driver that is busy refuses, and the request is made again until it is taken, once") {
  o3os::model::Settings s = device();
  CHECK(s.set(ds::UsbReturn, 0.0f) == o3os::model::Status::Ok);
  FakeDriver d;
  d.changing = true;  // a project load's mode change is in flight (target On, going to stay On)
  d.pending = true;
  d.busy_left = 3;
  UsbReturnSync sync;
  pass(sync, s, d);  // refused (and the change completes after 3 ticks)
  pass(sync, s, d);
  CHECK(d.accepted == 0u);
  CHECK(d.requests == 2u);
  CHECK(d.changing);
  pass(sync, s, d);
  CHECK_FALSE(d.changing);
  CHECK(d.presented);
  pass(sync, s, d);  // free now: taken
  CHECK(d.accepted == 1u);
  CHECK_FALSE(d.target());
  const unsigned requests = d.requests;
  for (int i = 0; i < 6; ++i) {
    pass(sync, s, d);
  }
  CHECK(d.requests == requests);  // and not asked again
  CHECK(d.reenumerations == 1u);
  CHECK(s.get(ds::UsbReturn) == 0.0f);  // the setting was never overwritten by a transient value of the driver
}

TEST_CASE("usb return sync: a console change is copied to the setting, once, and a Config change goes to the driver") {
  o3os::model::Settings s = device();
  FakeDriver d;
  UsbReturnSync sync;
  for (int i = 0; i < 3; ++i) {
    pass(sync, s, d);
  }
  CHECK(d.requests == 0u);
  const std::uint32_t generation = s.generation(ds::UsbReturn);

  // `usb return off` on the console: the driver changes by itself, the setting follows within a pass (and only once).
  CHECK(d.console(false));
  CHECK_FALSE(d.target());
  pass(sync, s, d);
  CHECK(s.get(ds::UsbReturn) == 0.0f);
  CHECK(s.generation(ds::UsbReturn) == generation + 1u);
  const unsigned requests = d.requests;
  for (int i = 0; i < 4; ++i) {
    pass(sync, s, d);
  }
  CHECK(s.generation(ds::UsbReturn) == generation + 1u);  // not written again
  CHECK(d.requests == requests);                          // and the write was not taken for a Config change: nothing was asked back

  // A change in flight on the driver: the setting follows the TARGET, not the descriptors still presented.
  FakeDriver slow;
  slow.busy_passes = 3;
  o3os::model::Settings s2 = device();
  UsbReturnSync sync2;
  pass(sync2, s2, slow);
  CHECK(slow.console(false));
  CHECK(slow.changing);
  CHECK(slow.presented);  // still On on the bus
  pass(sync2, s2, slow);
  CHECK(s2.get(ds::UsbReturn) == 0.0f);

  // The Config row, turned back On: the generation moves, the driver is asked (once its change is done), the setting stays On.
  CHECK(s2.set(ds::UsbReturn, 1.0f) == o3os::model::Status::Ok);
  for (int i = 0; i < 8; ++i) {
    pass(sync2, s2, slow);
  }
  CHECK(slow.presented);
  CHECK_FALSE(slow.changing);
  CHECK(s2.get(ds::UsbReturn) == 1.0f);
  CHECK(slow.reenumerations == 2u);  // Off, then On again: one re-enumeration each

  // A Config change on the pair the console had moved: the driver follows once and nothing is asked again.
  CHECK(s.set(ds::UsbReturn, 1.0f) == o3os::model::Status::Ok);
  pass(sync, s, d);
  CHECK(d.target());
  const unsigned reenum = d.reenumerations;
  for (int i = 0; i < 4; ++i) {
    pass(sync, s, d);
  }
  CHECK(d.reenumerations == reenum);
  CHECK(s.get(ds::UsbReturn) == 1.0f);
}

TEST_CASE("usb return sync: the decision table") {
  UsbReturnSync a;
  using Act = UsbReturnSync::Act;
  // First pass: agree -> nothing (and settled); differ -> Request until accepted.
  CHECK(a.step(true, 7, true) == Act::None);
  CHECK(a.step(true, 7, false) == Act::Store);  // settled at generation 7: a differing driver is a console change
  UsbReturnSync b;
  CHECK(b.step(false, 3, true) == Act::Request);
  CHECK(b.step(false, 3, true) == Act::Request);  // refused: asked again
  b.accepted(3);
  CHECK(b.step(false, 3, false) == Act::None);
  CHECK(b.step(false, 3, true) == Act::Store);
  b.stored(4);
  CHECK(b.step(true, 4, true) == Act::None);
  CHECK(b.step(false, 5, true) == Act::Request);  // the setting moved: the driver follows
}

// OS-006e: the device setting RETURN LEVEL carried to the engine as the ParamChange master::UsbReturnLevel: at the first pass (the settings file's value), whenever the setting's generation
// moves (Config, a bound controller), never while it does not, and again on a later pass when the ring was full.
TEST_CASE("usb return level: the setting reaches the engine at the first pass and at every change, and a full ring is asked again") {
  namespace gp = o3os::audio::graph_param;
  o3os::model::Settings device;
  std::memset(&device, 0, sizeof device);
  device.init(o3os::core::ParamKind::Device);
  static o3os::core::ParamRing ring;
  o3os::core::ParamChange c;
  while (ring.pop(c)) {
  }
  o3os::app::UsbReturnLevelSync sync;
  CHECK(device.get(ds::UsbReturnLevel) == 100.0f);
  CHECK(sync.step(device, ring));  // the first pass: the file's (here the default) value, unity
  CHECK(ring.pop(c));
  CHECK(c.id == o3os::audio::master_param(gp::master::UsbReturnLevel));
  CHECK(c.value == 100.0f);
  CHECK(c.ramp_samples == 0u);  // the engine's own smoothing
  CHECK_FALSE(sync.step(device, ring));  // nothing changed: nothing sent
  CHECK_FALSE(sync.step(device, ring));
  CHECK_FALSE(ring.pop(c));
  CHECK(device.set(ds::UsbReturnLevel, 55.0f) == o3os::model::Status::Ok);
  CHECK(sync.step(device, ring));
  CHECK(ring.pop(c));
  CHECK(c.value == 55.0f);
  CHECK(device.set(ds::UsbReturnLevel, 55.0f) == o3os::model::Status::Ok);  // the same value: no change of generation, nothing sent
  CHECK_FALSE(sync.step(device, ring));
  // A full ring: not sent, not marked seen; sent once there is room.
  CHECK(device.set(ds::UsbReturnLevel, 30.0f) == o3os::model::Status::Ok);
  unsigned filler = 0;
  while (ring.push(o3os::core::ParamChange{o3os::core::ParamId::None, 0.0f, 1})) {
    ++filler;
  }
  CHECK(filler != 0u);
  CHECK_FALSE(sync.step(device, ring));
  CHECK(ring.pop(c));  // room for one
  CHECK(sync.step(device, ring));
  while (ring.pop(c)) {
  }
  CHECK(c.value == 30.0f);  // (the last entry popped is the level)
  CHECK_FALSE(sync.step(device, ring));
}

// ---- OS-USBAUDIO ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

using o3os::usb::Mode;
using o3os::usb::Profile;

// usb0 as it behaves for a mode, a profile and a return: one descriptor set presented; a request for another set starts a change (a re-enumeration) that takes `busy_passes` passes, during which
// the target is the new set and further requests are refused; in STANDARD (whose descriptors depend on neither the profile nor the return) a return is only remembered. `enumerations` counts the
// sets the host has been shown, the boot's included.
struct FakeUsb {
  bool attached = true;
  Mode mode = Mode::Standard;
  bool usb_return = true;
  bool changing = false;
  Mode pending_mode = Mode::Standard;
  bool pending_return = true;
  unsigned busy_left = 0;
  unsigned busy_passes = 2;
  unsigned enumerations = 0;
  unsigned requests = 0;

  // usb0::start(): the first enumeration, in the set the caller chose.
  void start(const o3os::app::UsbBootLayout& b) {
    mode = b.mode;
    usb_return = b.usb_return;
    enumerations = 1;
  }
  bool request_layout(Mode m, bool ret) {
    ++requests;
    if (changing) {
      return false;
    }
    if (mode == Mode::Disk) {  // the DISK set is final until a restart: the same layout is accepted, anything else refused (the rule usb0 applies, o3os::usb::disk_layout_accepts)
      return o3os::usb::disk_layout_accepts(Profile::P8, usb_return, m, Profile::P8, ret);
    }
    if (m == Mode::Standard && mode == Mode::Standard) {
      usb_return = ret;  // remembered for the next AUDIO attach: nothing re-enumerates
      return true;
    }
    if (m == mode && ret == usb_return) {
      return true;
    }
    ++enumerations;
    changing = true;
    pending_mode = m;
    pending_return = ret;
    busy_left = busy_passes;
    return true;
  }
  void tick() {
    if (changing && --busy_left == 0u) {
      changing = false;
      mode = pending_mode;
      usb_return = pending_return;
    }
  }
};

struct AudioAdapter {
  FakeUsb& usb;
  const o3os::model::Settings& device;
  bool ready() const { return usb.attached; }
  bool disk() const { return (usb.changing ? usb.pending_mode : usb.mode) == Mode::Disk; }
  bool audio() const { return (usb.changing ? usb.pending_mode : usb.mode) == Mode::Audio; }
  bool request(bool on) { return usb.request_layout(on ? Mode::Audio : Mode::Standard, device.get(ds::UsbReturn) != 0.0f); }
};

struct ReturnAdapter {
  FakeUsb& usb;
  bool ready() const { return usb.attached; }
  bool disk() const { return (usb.changing ? usb.pending_mode : usb.mode) == Mode::Disk; }
  bool target() const { return usb.changing ? usb.pending_return : usb.usb_return; }
  bool request(bool on) { return usb.request_layout(usb.mode, on); }
};

// The main loop's pass (app_main.cpp sync_usb_return): the mode first, then the return, then the driver's own progress.
struct Loop {
  o3os::model::Settings settings = device();
  FakeUsb usb;
  o3os::app::UsbAudioSync audio_sync;
  UsbReturnSync return_sync;

  // Boots as main.cpp does: the settings are known, the layout is decided from them, the port enumerates once.
  void boot() { usb.start(o3os::app::usb_boot_layout(settings.get(ds::UsbAudio) != 0.0f, Profile::P8, settings.get(ds::UsbReturn) != 0.0f)); }
  // The boot disk (OS-USBDISK (b), chord Shift + F1 + F7): main.cpp starts the port with the DISK set whatever the settings say, and the settings file was never read (the card is the computer's), so
  // `settings` is the defaults: USB AUDIO On, USB RETURN On.
  void boot_disk() {
    usb.start(o3os::app::usb_boot_layout(false, Profile::P8, settings.get(ds::UsbReturn) != 0.0f));
    usb.mode = Mode::Disk;
  }
  void pass() {
    AudioAdapter a{usb, settings};
    o3os::app::sync_usb_audio(audio_sync, settings, a);
    ReturnAdapter r{usb};
    o3os::app::sync_usb_return(return_sync, settings, r);
    usb.tick();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
};

}  // namespace

// The boot decision: a function of the setting, the layout (the profile) and USB RETURN. AUDIO exactly when the setting is On and the engine can feed the layout; the layout and the return are carried
// either way (STANDARD remembers them for a later `usb mode audio`).
TEST_CASE("usb boot layout: AUDIO when USB AUDIO is On and the layout can be fed, STANDARD otherwise; the layout and the return are carried either way") {
  for (const Profile p : {Profile::P2, Profile::P8, Profile::P10, Profile::P11}) {
    for (const bool ret : {false, true}) {
      const o3os::app::UsbBootLayout on = o3os::app::usb_boot_layout(true, p, ret);
      CHECK(on.mode == Mode::Audio);
      CHECK(on.profile == p);
      CHECK(on.usb_return == ret);
      const o3os::app::UsbBootLayout off = o3os::app::usb_boot_layout(false, p, ret);
      CHECK(off.mode == Mode::Standard);
      CHECK(off.profile == p);
      CHECK(off.usb_return == ret);
    }
  }
  // A layout the engine cannot feed (4 stereo post-pan stems) is never offered as AUDIO.
  const o3os::app::UsbBootLayout p4s = o3os::app::usb_boot_layout(true, Profile::P4S, true);
  CHECK(p4s.mode == Mode::Standard);
  // The setting's default is On: a unit with no file, or a file from before the row, comes up as the audio device with the boot layout (profile 8, "8 OUT / 2 IN").
  const o3os::model::Settings d = device();
  const o3os::app::UsbBootLayout boot = o3os::app::usb_boot_layout(d.get(ds::UsbAudio) != 0.0f, Profile::P8, d.get(ds::UsbReturn) != 0.0f);
  CHECK(boot.mode == Mode::Audio);
  CHECK(boot.profile == Profile::P8);
  CHECK(boot.usb_return);
}

TEST_CASE("usb audio sync: the port enumerates once at boot in the mode the setting says, and the first passes ask for nothing") {
  for (const bool audio : {true, false}) {
    for (const bool ret : {true, false}) {
      Loop l;
      CHECK(l.settings.set(ds::UsbAudio, audio ? 1.0f : 0.0f) == o3os::model::Status::Ok);
      CHECK(l.settings.set(ds::UsbReturn, ret ? 1.0f : 0.0f) == o3os::model::Status::Ok);
      l.boot();
      CHECK(l.usb.mode == (audio ? Mode::Audio : Mode::Standard));
      CHECK(l.usb.usb_return == ret);
      l.passes(12);
      CHECK(l.usb.enumerations == 1u);  // one device shown to the host, never STANDARD first
      CHECK(l.usb.requests == 0u);      // the driver already is what the settings say
      CHECK(l.settings.get(ds::UsbAudio) == (audio ? 1.0f : 0.0f));
      CHECK(l.settings.get(ds::UsbReturn) == (ret ? 1.0f : 0.0f));
    }
  }
}

TEST_CASE("usb audio sync: Off re-enumerates once as STANDARD and the setting stays Off; On goes back; the console's mode is runtime only") {
  Loop l;
  l.boot();
  l.passes(4);
  CHECK(l.usb.mode == Mode::Audio);
  CHECK(l.usb.enumerations == 1u);
  // Config: USB AUDIO Off applied.
  CHECK(l.settings.set(ds::UsbAudio, 0.0f) == o3os::model::Status::Ok);
  l.passes(10);
  CHECK(l.usb.mode == Mode::Standard);
  CHECK(l.usb.enumerations == 2u);
  CHECK(l.settings.get(ds::UsbAudio) == 0.0f);
  CHECK(l.usb.requests == 1u);
  // On again: back to AUDIO, one more enumeration.
  CHECK(l.settings.set(ds::UsbAudio, 1.0f) == o3os::model::Status::Ok);
  l.passes(10);
  CHECK(l.usb.mode == Mode::Audio);
  CHECK(l.usb.enumerations == 3u);
  // The console's `usb mode standard` (runtime only): the driver changes, the setting is left alone, and nothing fights it back.
  const std::uint32_t generation = l.settings.generation(ds::UsbAudio);
  CHECK(l.usb.request_layout(Mode::Standard, true));
  const unsigned requests = l.usb.requests;
  l.passes(12);
  CHECK(l.usb.mode == Mode::Standard);
  CHECK(l.settings.get(ds::UsbAudio) == 1.0f);
  CHECK(l.settings.generation(ds::UsbAudio) == generation);  // not written
  CHECK(l.usb.requests == requests);                         // and not asked back: a reboot, not a pass, restores the setting's mode
  CHECK(l.usb.enumerations == 4u);
  // And `usb mode audio` the other way, the setting still On.
  CHECK(l.usb.request_layout(Mode::Audio, true));
  l.passes(12);
  CHECK(l.usb.mode == Mode::Audio);
  CHECK(l.settings.get(ds::UsbAudio) == 1.0f);
  CHECK(l.usb.enumerations == 5u);
}

TEST_CASE("usb audio sync: applying USB AUDIO and USB RETURN together is one re-enumeration") {
  // STANDARD to AUDIO with the return Off at the same time (capture only): one new set shown, not AUDIO with the return and then without it.
  Loop l;
  CHECK(l.settings.set(ds::UsbAudio, 0.0f) == o3os::model::Status::Ok);
  l.boot();
  l.passes(4);
  CHECK(l.usb.enumerations == 1u);
  CHECK(l.usb.mode == Mode::Standard);
  CHECK(l.settings.set(ds::UsbAudio, 1.0f) == o3os::model::Status::Ok);  // (Config's APPLY writes both in one action, before the next pass)
  CHECK(l.settings.set(ds::UsbReturn, 0.0f) == o3os::model::Status::Ok);
  l.passes(12);
  CHECK(l.usb.mode == Mode::Audio);
  CHECK_FALSE(l.usb.usb_return);
  CHECK(l.usb.enumerations == 2u);
  // AUDIO to STANDARD with the return back On: one.
  CHECK(l.settings.set(ds::UsbAudio, 0.0f) == o3os::model::Status::Ok);
  CHECK(l.settings.set(ds::UsbReturn, 1.0f) == o3os::model::Status::Ok);
  l.passes(12);
  CHECK(l.usb.mode == Mode::Standard);
  CHECK(l.usb.usb_return);
  CHECK(l.usb.enumerations == 3u);
  // The return alone in AUDIO: its own, one.
  CHECK(l.settings.set(ds::UsbAudio, 1.0f) == o3os::model::Status::Ok);
  l.passes(12);
  CHECK(l.usb.enumerations == 4u);
  CHECK(l.settings.set(ds::UsbReturn, 0.0f) == o3os::model::Status::Ok);
  l.passes(12);
  CHECK(l.usb.mode == Mode::Audio);
  CHECK_FALSE(l.usb.usb_return);
  CHECK(l.usb.enumerations == 5u);
  // The return alone in STANDARD changes nothing on the bus.
  CHECK(l.settings.set(ds::UsbAudio, 0.0f) == o3os::model::Status::Ok);
  l.passes(12);
  CHECK(l.usb.enumerations == 6u);
  CHECK(l.settings.set(ds::UsbReturn, 1.0f) == o3os::model::Status::Ok);
  l.passes(12);
  CHECK(l.usb.mode == Mode::Standard);
  CHECK(l.usb.enumerations == 6u);
}

TEST_CASE("usb audio sync: a busy driver is asked again and a driver that is not running decides nothing") {
  Loop l;
  l.boot();
  l.usb.attached = false;
  CHECK(l.settings.set(ds::UsbAudio, 0.0f) == o3os::model::Status::Ok);
  l.passes(3);
  CHECK(l.usb.requests == 0u);
  l.usb.attached = true;
  l.usb.changing = true;  // a project load's profile change is in flight
  l.usb.pending_mode = Mode::Audio;
  l.usb.pending_return = true;
  l.usb.busy_left = 3;
  l.passes(2);
  CHECK(l.usb.mode == Mode::Audio);
  CHECK(l.usb.enumerations == 1u);  // refused while it runs ...
  l.passes(10);
  CHECK(l.usb.mode == Mode::Standard);  // ... and taken once it is done, once
  CHECK(l.usb.enumerations == 2u);
}

// The v0.6.8 bug (QA-GLB-015/019 FAIL): the boot disk comes up in DISK with the settings at their defaults (the file is not read: nothing is mounted), USB AUDIO defaults to On, and the first pass of the
// sync saw "the setting says AUDIO, the driver presents DISK" and re-enumerated the unit as AUDIO about 120 ms after the host's first reset, in the middle of its enumeration (the host: "device not
// responding to setup address", error -71). The DISK mode is final until a restart: neither sync asks, whatever the settings say or later become.
TEST_CASE("usb boot disk: the syncs never take the unit out of DISK, whatever the settings say") {
  Loop l;
  CHECK(l.settings.get(ds::UsbAudio) != 0.0f);  // the default is On: the case that went wrong
  l.boot_disk();
  CHECK(l.usb.mode == Mode::Disk);
  l.passes(40);
  CHECK(l.usb.requests == 0u);
  CHECK(l.usb.enumerations == 1u);  // one device shown to the host
  CHECK(l.usb.mode == Mode::Disk);
  CHECK_FALSE(l.usb.changing);
  // The settings move (a Learn binding, a Config row that is reachable): still nothing.
  CHECK(l.settings.set(ds::UsbAudio, 0.0f) == o3os::model::Status::Ok);
  CHECK(l.settings.set(ds::UsbReturn, 0.0f) == o3os::model::Status::Ok);
  l.passes(40);
  CHECK(l.settings.set(ds::UsbAudio, 1.0f) == o3os::model::Status::Ok);
  CHECK(l.settings.set(ds::UsbReturn, 1.0f) == o3os::model::Status::Ok);
  l.passes(40);
  CHECK(l.usb.requests == 0u);
  CHECK(l.usb.enumerations == 1u);
  CHECK(l.usb.mode == Mode::Disk);
  // The setting is not touched either (the sync never writes in DISK).
  CHECK(l.settings.get(ds::UsbAudio) == 1.0f);
  CHECK(l.settings.get(ds::UsbReturn) == 1.0f);
}

// The driver's own refusal (usb0::request_layout), for the callers that do not look at disk() first: the console's `usb mode` / `usb profile` / `usb return` and the project's USB apply.
TEST_CASE("usb boot disk: a direct request to leave DISK or change its layout is refused, the same layout is accepted and changes nothing") {
  Loop l;
  l.boot_disk();
  CHECK(l.usb.mode == Mode::Disk);
  CHECK_FALSE(l.usb.request_layout(Mode::Audio, true));
  CHECK_FALSE(l.usb.request_layout(Mode::Standard, true));
  CHECK_FALSE(l.usb.request_layout(Mode::Audio, false));
  CHECK_FALSE(l.usb.request_layout(Mode::Disk, false));  // DISK itself with another return: a change, refused
  CHECK(l.usb.request_layout(Mode::Disk, true));         // the layout it presents: nothing to do
  CHECK(l.usb.mode == Mode::Disk);
  CHECK_FALSE(l.usb.changing);
  CHECK(l.usb.enumerations == 1u);
  l.passes(20);
  CHECK(l.usb.mode == Mode::Disk);
  CHECK(l.usb.enumerations == 1u);
}

TEST_CASE("usb disk_layout_accepts: only the layout DISK presents") {
  using o3os::usb::disk_layout_accepts;
  CHECK(disk_layout_accepts(Profile::P8, true, Mode::Disk, Profile::P8, true));
  CHECK(disk_layout_accepts(Profile::P10, false, Mode::Disk, Profile::P10, false));
  CHECK_FALSE(disk_layout_accepts(Profile::P8, true, Mode::Disk, Profile::P10, true));  // another profile
  CHECK_FALSE(disk_layout_accepts(Profile::P8, true, Mode::Disk, Profile::P8, false));  // another return
  CHECK_FALSE(disk_layout_accepts(Profile::P8, true, Mode::Audio, Profile::P8, true));  // another mode
  CHECK_FALSE(disk_layout_accepts(Profile::P8, true, Mode::Standard, Profile::P8, true));
}

TEST_CASE("usb boot disk: a mode change that is already in flight towards DISK is held too, and the other modes still sync") {
  Loop l;
  l.boot();  // AUDIO (the default)
  CHECK(l.usb.mode == Mode::Audio);
  l.usb.changing = true;  // a running-mode hand-over has asked for DISK and the re-enumeration is running
  l.usb.pending_mode = Mode::Disk;
  l.usb.pending_return = true;
  l.usb.busy_left = 3;
  l.passes(3);
  CHECK(l.usb.mode == Mode::Disk);
  CHECK(l.usb.requests == 0u);
  l.passes(20);
  CHECK(l.usb.requests == 0u);
  CHECK(l.usb.mode == Mode::Disk);
  // A unit that is not in DISK is unchanged by all this: Off re-enumerates as STANDARD once.
  Loop m;
  m.boot();
  CHECK(m.settings.set(ds::UsbAudio, 0.0f) == o3os::model::Status::Ok);
  m.passes(10);
  CHECK(m.usb.mode == Mode::Standard);
  CHECK(m.usb.enumerations == 2u);
}

// OS-USBOUTS D3: the descriptors carry the return only when the setting is On and the profile has fewer than 10 channels; the setting itself (the wish) is never changed by it.
TEST_CASE("usb return presented: the wish counts below 10 channels only, and the boot layout carries the wish, not the presented return, for 4 and 6 as for the rest") {
  for (const Profile p : {Profile::P2, Profile::P4, Profile::P6, Profile::P8}) {
    CHECK(o3os::usb::return_presented(p, true));
    CHECK_FALSE(o3os::usb::return_presented(p, false));
  }
  for (const Profile p : {Profile::P10, Profile::P11}) {
    CHECK_FALSE(o3os::usb::return_presented(p, true));  // 10 OUT / 2 IN does not open behind a hub with a transaction translator (ADR 0012 amendment 5)
    CHECK_FALSE(o3os::usb::return_presented(p, false));
  }
  for (const Profile p : {Profile::P4, Profile::P6, Profile::P10}) {
    const o3os::app::UsbBootLayout b = o3os::app::usb_boot_layout(true, p, true);
    CHECK(b.mode == Mode::Audio);
    CHECK(b.profile == p);
    CHECK(b.usb_return);  // the wish: it survives a stay at 10 and comes back at fewer channels
  }
}
