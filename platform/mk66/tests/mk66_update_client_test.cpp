// SPDX-License-Identifier: GPL-3.0-or-later
//
// The client API of FwService (update/fw_service.h, OS-FWCONFIG) and the App's firmware port over it (update/fw_sink.h), against the fake store and host of the update tests:
// the listing, a check that passes, fails or is dropped (and writes nothing), every refusal, the install (its re-check, the 30 s of a check, the order of the hand-over, the 5 s
// fallback), and the one-owner rule against the console. The central test runs the console's `fw arm` + `fw update` and the client's check + install on the same fakes and
// requires the very same calls to the store and to the board: there is one hand-over, not two. Runs on the host and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"
#include "mk66_update_fakes.h"

#include <o3os/platform/mk66/update/fw_sink.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace upd_test;
using up::Fault;
using State = up::UiStatus::State;
namespace fwp = o3os::hal::firmware;

namespace {

constexpr const char* kName = "community-0.9.ptf";

struct Img {
  std::string file;
  std::uint32_t crc;
  std::uint32_t length;
  explicit Img(std::size_t code = 3000) : file(to_hex(good_image(code))), crc(file_crc(file)), length(static_cast<std::uint32_t>(good_image(code).size())) {}
  std::vector<std::uint8_t> bytes() const { return std::vector<std::uint8_t>(file.begin(), file.end()); }
};

// The service alone: no shell, no terminal.
struct Bare {
  FakeStore store;
  FakeHost host;
  up::FwService svc{store, host};
  Img img;
  Bare() { store.put_file(kName, img.bytes()); }

  // ui_slice() until the state leaves Running (or the wait for the bootloader begins when `to_handover`); returns the last status and counts the slices.
  up::UiStatus drive(unsigned* slices = nullptr, bool to_handover = false) {
    up::UiStatus s;
    unsigned n = 0;
    do {
      s = svc.ui_slice();
      ++n;
    } while (s.state == State::Running && n < 4000u && !(to_handover && svc.phase() == up::FwPhase::Handover));
    if (slices != nullptr) {
      *slices = n;
    }
    return s;
  }
  // The whole check; an idle status when it could not even begin.
  up::UiStatus checked() { return svc.ui_check_begin(kName) == Fault::Ok ? drive() : up::UiStatus{}; }
};

std::string token_of(const Rig& r) {
  const std::string t = r.text();
  const std::size_t at = t.rfind("armed ");
  return at == std::string::npos ? std::string() : t.substr(at + 6, 8);
}

}  // namespace

TEST_CASE("fw client: the list is the .ptf files in directory order, the first is the reset target, the temporary and hand-over files are hidden") {
  Bare b;  // PolyendTracker_1.9.0.ptf (the fake's first file), community-0.9.ptf
  b.store.put_file(up::kFwInfoName, std::vector<std::uint8_t>(5, 'x'));
  b.store.put_file(up::kTempName, std::vector<std::uint8_t>(9, 'y'));
  b.store.put_file("notes.txt", std::vector<std::uint8_t>(3, 'z'));
  b.store.put_file("LAST.PTF", std::vector<std::uint8_t>(77, 'w'));
  NEED(b.svc.ui_list_begin() == Fault::Ok);
  up::FwEntry e;
  bool first = false;
  std::vector<std::string> names;
  std::vector<bool> firsts;
  std::vector<std::uint32_t> sizes;
  while (b.svc.ui_list_next(e, first)) {
    names.push_back(e.name);
    firsts.push_back(first);
    sizes.push_back(e.size);
  }
  b.svc.ui_list_end();
  const std::vector<std::string> expected = {"PolyendTracker_1.9.0.ptf", kName, "LAST.PTF"};
  CHECK(names == expected);
  CHECK(firsts == std::vector<bool>{true, false, false});
  CHECK(sizes[0] == 1500u);
  CHECK(sizes[2] == 77u);
  // A second listing starts over, with the first flag again.
  NEED(b.svc.ui_list_begin() == Fault::Ok);
  NEED(b.svc.ui_list_next(e, first));
  CHECK(first);
  b.svc.ui_list_end();
  // An empty folder: nothing, and not an error.
  Bare empty;
  empty.store.order.clear();
  empty.store.files.clear();
  NEED(empty.svc.ui_list_begin() == Fault::Ok);
  CHECK(!empty.svc.ui_list_next(e, first));
  empty.svc.ui_list_end();
  // Only temporary and hand-over files: no .ptf, so nothing to show.
  Bare none;
  none.store.order.clear();
  none.store.files.clear();
  none.store.put_file(up::kTempName, std::vector<std::uint8_t>(9, 'y'));
  none.store.put_file(up::kFwInfoName, std::vector<std::uint8_t>(5, 'x'));
  NEED(none.svc.ui_list_begin() == Fault::Ok);
  CHECK(!none.svc.ui_list_next(e, first));
  none.svc.ui_list_end();
  // A listing is refused where a check is: the card on a computer, storage busy.
  b.host.on_host = true;
  CHECK(b.svc.ui_list_begin() == Fault::OnHost);
  b.host.on_host = false;
  b.host.busy = true;
  CHECK(b.svc.ui_list_begin() == Fault::CardBusy);
}

TEST_CASE("fw client: a check reads the file in slices, reports progress, the image length and the CRC-32, and writes nothing") {
  Bare b;
  NEED(b.svc.ui_check_begin(kName) == Fault::Ok);
  CHECK(b.svc.phase() == up::FwPhase::Arming);
  CHECK(up::fw_phase_holds_card(b.svc.phase()));  // OS-007o: the card claim of the image (app_main.cpp firmware_has_card) covers the client check, not only the console's
  std::uint32_t last_done = 0;
  unsigned running = 0;
  up::UiStatus s;
  do {
    s = b.svc.ui_slice();
    if (s.state == State::Running) {
      ++running;
      CHECK(s.total == b.img.file.size());
      CHECK(s.done >= last_done);
      CHECK(s.done <= s.total);
      CHECK(!s.installing);
      last_done = s.done;
    }
  } while (s.state == State::Running && running < 100u);
  CHECK(running >= 3u);  // 2 KiB a slice: a file of several KiB takes several
  CHECK(last_done > 0u);
  CHECK(s.state == State::Checked);
  CHECK(s.image == b.img.length);
  CHECK(s.crc == b.img.crc);  // the whole file's, as `fw arm` prints it
  CHECK(!s.installing);
  CHECK(b.svc.phase() == up::FwPhase::Idle);
  // Reads only: nothing was created, written, renamed or removed, and the board was not touched.
  const std::vector<std::string> expected_store = {std::string("open_read ") + kName};
  CHECK(b.store.calls == expected_store);
  CHECK(b.host.calls.empty());
  CHECK(!b.host.pin_low);
  CHECK(!b.store.has(up::kFwInfoName));
  CHECK(!b.svc.armed());  // there is no console token for it
  CHECK(b.svc.last().kind == up::FwLast::Kind::Arm);
  // The result stays until something else happens.
  CHECK(b.svc.ui_slice().state == State::Checked);
}

TEST_CASE("fw client: an image not padded to a 4 KiB sector fails the check (NotPadded), the boundary exactly passes, and nothing is armed or handed over (OS-GATE-BOOTCRASH)") {
  const std::size_t sector = 4096;
  Bare b;
  const std::string short_by_one = to_hex(good_image(sector - 0x410 - 1, false));
  const std::string exact = to_hex(good_image(sector - 0x410, false));
  b.store.put_file("short1.ptf", std::vector<std::uint8_t>(short_by_one.begin(), short_by_one.end()));
  b.store.put_file("exact.ptf", std::vector<std::uint8_t>(exact.begin(), exact.end()));
  NEED(b.svc.ui_check_begin("short1.ptf") == Fault::Ok);
  const up::UiStatus s = b.drive();
  CHECK(s.state == State::Failed);
  CHECK(s.fault == Fault::NotPadded);
  CHECK(s.offset == short_by_one.size());
  CHECK(!s.installing);
  CHECK(b.svc.ui_install() == Fault::NotArmed);  // no good check: the install refuses before the bootloader is asked
  CHECK(b.host.calls.empty());
  CHECK(!b.host.pin_low);
  CHECK(!b.store.has(up::kFwInfoName));
  NEED(b.svc.ui_check_begin("exact.ptf") == Fault::Ok);
  const up::UiStatus ok = b.drive();
  CHECK(ok.state == State::Checked);
  CHECK(ok.image == sector);
  {  // through the port: the screen is told the offset is not a place in the file (the stream's byte count is no image address)
    Bare p;
    p.store.put_file("short1.ptf", std::vector<std::uint8_t>(short_by_one.begin(), short_by_one.end()));
    up::FwServiceSink sink(p.svc);
    NEED(sink.begin_check("short1.ptf") == fwp::Refusal::None);
    fwp::CheckStatus ps;
    unsigned n = 0;
    do {
      ps = sink.poll_check();
    } while (ps.state == fwp::CheckStatus::State::Running && ++n < 100u);
    CHECK(ps.state == fwp::CheckStatus::State::Failed);
    CHECK(!ps.offset_known);
  }
}

TEST_CASE("fw client: a check that fails says why and at which byte, and a dropped check leaves the card as it was") {
  {  // the flash configuration field is wrong: the very rule that protects the MCU
    Bare b;
    std::vector<std::uint8_t> image = good_image(2000);
    image[0x40D] = 0x00;
    const std::string bad = to_hex(image);
    b.store.put_file("locked.ptf", std::vector<std::uint8_t>(bad.begin(), bad.end()));
    NEED(b.svc.ui_check_begin("locked.ptf") == Fault::Ok);
    const up::UiStatus s = b.drive();
    CHECK(s.state == State::Failed);
    CHECK(s.fault == Fault::BadConfig);
    CHECK(s.offset == validate(bad).offset);
    CHECK(!s.installing);
    CHECK(b.svc.phase() == up::FwPhase::Idle);
    CHECK(b.svc.ui_slice().state == State::Failed);  // sticky until the next check
    CHECK(b.store.calls.size() == 1u);
    CHECK(b.host.calls.empty());
    // ... and a check of a good file afterwards works.
    NEED(b.svc.ui_check_begin(kName) == Fault::Ok);
    CHECK(b.drive().state == State::Checked);
  }
  {  // not HEX at all
    Bare b;
    NEED(b.svc.ui_check_begin("PolyendTracker_1.9.0.ptf") == Fault::Ok);
    const up::UiStatus s = b.drive();
    CHECK(s.state == State::Failed);
    CHECK(s.fault == Fault::HexChar);
    CHECK(s.offset == 0u);
  }
  {  // the card fails in the middle of the read
    Bare b;
    NEED(b.svc.ui_check_begin(kName) == Fault::Ok);
    (void)b.svc.ui_slice();
    b.store.close_read();  // the fake's read() answers Io when no file is open: the card went away
    const up::UiStatus s = b.drive();
    CHECK(s.state == State::Failed);
    CHECK(s.fault == Fault::Io);
    CHECK(b.svc.phase() == up::FwPhase::Idle);
  }
  {  // dropped in the middle
    Bare b;
    NEED(b.svc.ui_check_begin(kName) == Fault::Ok);
    (void)b.svc.ui_slice();
    CHECK(b.svc.ui_slice().state == State::Running);
    b.svc.ui_cancel();
    CHECK(b.svc.phase() == up::FwPhase::Idle);
    CHECK(b.svc.ui_slice().state == State::Idle);
    const std::vector<std::string> expected_store = {std::string("open_read ") + kName};
    CHECK(b.store.calls == expected_store);
    CHECK(b.host.calls.empty());
    // the service is free again
    NEED(b.svc.ui_check_begin(kName) == Fault::Ok);
    CHECK(b.drive().state == State::Checked);
    // a result that waits for install() is dropped too
    b.svc.ui_cancel();
    CHECK(b.svc.ui_slice().state == State::Idle);
    CHECK(b.svc.ui_install() == Fault::NotArmed);
  }
}

// OS-BUG-FWNAMELEN: a stored image whose name is over 30 characters is listed but never checked, armed or handed over (the SD bootloader flashes another file for it).
TEST_CASE("fw client: a file name over 30 characters is listed, refused by the check, and nothing is armed") {
  const std::string at30 = std::string(up::kUpdateNameMax - 4, 'b') + ".ptf";
  const std::string at31 = std::string(up::kUpdateNameMax - 3, 'b') + ".ptf";
  Bare b;
  b.store.put_file(at30, b.img.bytes());
  b.store.put_file(at31, b.img.bytes());
  unsigned seen = 0;
  NEED(b.svc.ui_list_begin() == Fault::Ok);
  bool first = false;
  up::FwEntry e;
  while (b.svc.ui_list_next(e, first)) {
    seen += (e.name == at30 || e.name == at31) ? 1u : 0u;
  }
  b.svc.ui_list_end();
  CHECK(seen == 2u);  // both are shown
  b.store.calls.clear();
  CHECK(b.svc.ui_check_begin(at31.c_str()) == Fault::NameLong);
  CHECK(b.svc.phase() == up::FwPhase::Idle);
  CHECK(b.store.calls.empty());  // refused before the card was asked anything
  CHECK(b.svc.ui_install() == Fault::NotArmed);
  CHECK(!b.store.has(up::kFwInfoName));
  CHECK(up::refusal_of(Fault::NameLong) == fwp::Refusal::NameTooLong);
  // 30 characters pass the check, and a stale armed check is not replaced by a refused one
  CHECK(b.svc.ui_check_begin(at30.c_str()) == Fault::Ok);
  CHECK(b.drive().state == State::Checked);
  CHECK(b.svc.ui_check_begin(at31.c_str()) == Fault::NameLong);
  CHECK(b.svc.ui_install() == Fault::Ok);  // the earlier good check still stands
}

TEST_CASE("fw client: every refusal of a check names its reason and touches nothing") {
  {
    Bare b;
    CHECK(b.svc.ui_check_begin("a b.ptf") == Fault::BadName);
    CHECK(b.svc.ui_check_begin(up::kTempName) == Fault::BadName);  // the upload's temporary file is not an image
    CHECK(b.svc.ui_check_begin("..ptf") == Fault::BadName);
    CHECK(b.svc.ui_check_begin("missing.ptf") == Fault::NotFound);
    CHECK(b.svc.phase() == up::FwPhase::Idle);
  }
  {
    Bare b;
    b.host.on_host = true;
    CHECK(b.svc.ui_check_begin(kName) == Fault::OnHost);
    CHECK(b.store.calls.empty());
    b.host.on_host = false;
    b.host.busy = true;
    CHECK(b.svc.ui_check_begin(kName) == Fault::CardBusy);
    CHECK(b.store.calls.empty());
    b.host.busy = false;
    b.store.set_state(false, Fault::WriteProtected);
    CHECK(b.svc.ui_check_begin(kName) == Fault::WriteProtected);
    b.store.set_state(false, Fault::NoDir);
    CHECK(b.svc.ui_check_begin(kName) == Fault::NoDir);
    b.store.set_state(false, Fault::NoCard);
    CHECK(b.svc.ui_check_begin(kName) == Fault::NoCard);
    CHECK(b.store.calls.empty());
    CHECK(b.svc.phase() == up::FwPhase::Idle);
    b.store.set_state(true, Fault::Ok);
    CHECK(b.svc.ui_check_begin(kName) == Fault::Ok);  // none of them left anything behind
  }
  {  // a check is under way: a second one is refused, and so is a listing
    Bare b;
    NEED(b.svc.ui_check_begin(kName) == Fault::Ok);
    CHECK(b.svc.ui_check_begin(kName) == Fault::Busy);
    CHECK(b.svc.ui_list_begin() == Fault::Busy);
    CHECK(b.drive().state == State::Checked);
  }
}

TEST_CASE("fw client: the console and the screen never run at once") {
  Rig r;
  const Img img;
  r.store.put_file(kName, img.bytes());
  {  // a console operation is in progress: the client is refused, and the console's operation is not disturbed
    r.send("fw arm " + std::string(kName) + " " + hex8s(img.crc) + "\r");
    for (unsigned i = 0; i < 6 && r.svc.phase() == up::FwPhase::Idle; ++i) {
      r.step();
    }
    NEED(r.svc.phase() != up::FwPhase::Idle);
    CHECK(r.svc.ui_check_begin(kName) == Fault::Busy);
    CHECK(r.svc.ui_list_begin() == Fault::Busy);
    r.run();
    CHECK(token_of(r).size() == 8u);
    CHECK(r.svc.armed());
  }
  {  // the screen drops the console's token when it starts a check; and while it runs, the console says busy
    NEED(r.svc.ui_check_begin(kName) == Fault::Ok);
    CHECK(!r.svc.armed());
    r.clear();
    r.command("fw");
    CHECK(r.said("ERR busy"));
    r.clear();
    r.command("fw arm " + std::string(kName) + " " + hex8s(img.crc));
    CHECK(r.said("ERR busy"));
    CHECK(!r.said("armed "));
    // the check was not disturbed; a terminal that closes does not abort it either
    r.svc.terminal_closed();
    unsigned n = 0;
    up::UiStatus s;
    do {
      s = r.svc.ui_slice();
    } while (s.state == State::Running && ++n < 1000u);
    CHECK(s.state == State::Checked);
    CHECK(s.crc == img.crc);
  }
  {  // a passed check cannot be used by a console token, and any console command drops it
    r.clear();
    r.command("fw update 00000001");
    CHECK(r.said("ERR not armed for that token"));
    CHECK(!r.host.called("quiesce"));
    CHECK(r.svc.ui_slice().state == State::Failed);  // dropped by that command
    CHECK(r.svc.ui_install() == Fault::NotArmed);
    CHECK(!r.host.called("quiesce"));
    CHECK(!r.host.pin_low);
  }
  {  // the stale end of a failed console operation (phase Done, its stream still to end) does not take over a client check
    Rig q;
    q.store.put_file(kName, img.bytes());
    q.send("fw arm PolyendTracker_1.9.0.ptf 00000000\r");  // not HEX: the arm fails
    for (unsigned i = 0; i < 2000 && q.svc.phase() != up::FwPhase::Done; ++i) {
      q.step();
    }
    NEED(q.svc.phase() == up::FwPhase::Done);
    CHECK(q.svc.ui_check_begin(kName) == Fault::Ok);  // Done is not busy
    unsigned n = 0;
    up::UiStatus s;
    do {
      q.step();  // the console's pump and stream run meanwhile
      s = q.svc.ui_slice();
    } while (s.state == State::Running && ++n < 1000u);
    CHECK(s.state == State::Checked);
    CHECK(s.crc == img.crc);
    CHECK(!q.said("armed "));
    CHECK(q.said("o3> "));  // the console is back at its prompt
  }
}

TEST_CASE("fw client: the install re-validates, hands over in the console's order, and the 5 s fallback releases the pin") {
  Bare b;
  const up::UiStatus checked = b.checked();
  NEED(checked.state == State::Checked);
  b.store.put_file(up::kFwInfoName, std::vector<std::uint8_t>(3, 'x'));  // a stale one
  b.store.calls.clear();
  NEED(b.svc.ui_install() == Fault::Ok);
  // Nothing is touched until the file has been read and validated once more.
  CHECK(b.svc.phase() == up::FwPhase::Rechecking);
  CHECK(up::fw_phase_holds_card(b.svc.phase()));  // the re-check and the hand-over hold the card too: a save or a mount waits for them
  CHECK(b.host.calls.empty());
  up::UiStatus s = b.svc.ui_slice();
  CHECK(s.state == State::Running);
  CHECK(s.installing);
  CHECK(s.total == b.img.file.size());
  CHECK(b.host.calls.empty());
  s = b.drive(nullptr, true);
  CHECK(b.svc.phase() == up::FwPhase::Handover);
  CHECK(up::fw_phase_holds_card(b.svc.phase()));
  CHECK(s.state == State::Running);
  CHECK(s.installing);
  CHECK(s.done == s.total);
  CHECK(b.host.pin_low);
  const std::vector<std::string> expected_host = {"quiesce", "drain 40", "pin low"};
  CHECK(b.host.calls == expected_host);
  const std::vector<std::string> expected_store = {std::string("open_read ") + kName, "remove_fwinfo", std::string("write_fwinfo ") + kName};
  CHECK(b.store.calls == expected_store);
  CHECK(b.store.text_of(up::kFwInfoName) == kName);  // the bare name, no folder, no newline
  CHECK(b.svc.last().kind == up::FwLast::Kind::Update);
  CHECK(b.svc.last().fault == Fault::Ok);
  // The chip has not reset: until 5 s are over the pin stays low, and the screen is told it is still going on.
  b.host.now += 4900;
  s = b.svc.ui_slice();
  CHECK(s.state == State::Running);
  CHECK(s.installing);
  CHECK(b.host.pin_low);
  CHECK(b.svc.ui_check_begin(kName) == Fault::Busy);  // nothing else starts meanwhile
  CHECK(b.svc.ui_install() == Fault::Busy);           // ... and it cannot be installed twice (nothing of the job is touched: see the next test)
  CHECK(b.svc.ui_slice().state == State::Running);
  b.svc.ui_cancel();                                   // the hand-over cannot be taken back
  CHECK(b.svc.phase() == up::FwPhase::Handover);
  CHECK(b.host.pin_low);
  b.host.now += 100;
  s = b.svc.ui_slice();
  CHECK(s.state == State::DidNotStart);
  CHECK(s.installing);
  CHECK(!b.host.pin_low);
  CHECK(b.host.called("pin release"));
  CHECK(!b.store.has(up::kFwInfoName));
  CHECK(b.svc.phase() == up::FwPhase::Idle);
  CHECK(b.svc.ui_slice().state == State::DidNotStart);  // sticky until the next check
  unsigned lows = 0;
  for (const std::string& c : b.host.calls) {
    lows += c == "pin low" ? 1u : 0u;
  }
  CHECK(lows == 1u);
  // The service is free again: the file can be checked and installed once more.
  CHECK(b.checked().state == State::Checked);
}

TEST_CASE("fw client: poll() ends a hand-over by its clock even if nobody asks for the status, and a client hand-over prints nothing") {
  Rig r;
  const Img img;
  r.store.put_file(kName, img.bytes());
  NEED(r.svc.ui_check_begin(kName) == Fault::Ok);
  up::UiStatus s;
  for (unsigned i = 0; i < 1000 && (s = r.svc.ui_slice()).state == State::Running; ++i) {
  }
  NEED(s.state == State::Checked);
  NEED(r.svc.ui_install() == Fault::Ok);
  for (unsigned i = 0; i < 1000 && r.svc.phase() != up::FwPhase::Handover; ++i) {
    (void)r.svc.ui_slice();
  }
  NEED(r.svc.phase() == up::FwPhase::Handover);
  CHECK(r.host.pin_low);
  r.svc.poll(r.shell);
  CHECK(r.host.pin_low);
  r.host.now += 5000;
  r.svc.poll(r.shell);  // the main loop's pass, with the console's shell: it must not print the console's error for a client's job
  CHECK(!r.host.pin_low);
  CHECK(!r.store.has(up::kFwInfoName));
  CHECK(r.svc.phase() == up::FwPhase::Idle);
  CHECK(r.text().find("ERR") == std::string::npos);
  CHECK(r.svc.ui_slice().state == State::DidNotStart);
}

TEST_CASE("fw client: an install is refused, and hands over nothing, unless a check passed a moment ago on this card and the file is the same") {
  {  // no check
    Bare b;
    CHECK(b.svc.ui_install() == Fault::NotArmed);
    CHECK(b.store.calls.empty());
    CHECK(b.host.calls.empty());
  }
  {  // the 30 s: 29.999 s is good, 30 s is not
    Bare b;
    NEED(b.checked().state == State::Checked);
    b.host.now += 29999;
    CHECK(b.svc.ui_slice().state == State::Checked);
    CHECK(b.svc.ui_install() == Fault::Ok);
    Bare c;
    NEED(c.checked().state == State::Checked);
    c.host.now += 30000;
    CHECK(c.svc.ui_install() == Fault::Expired);
    CHECK(c.host.calls.empty());
    Bare d;  // the status says so before install() is tried
    NEED(d.checked().state == State::Checked);
    d.host.now += 30000;
    const up::UiStatus s = d.svc.ui_slice();
    CHECK(s.state == State::Failed);
    CHECK(s.fault == Fault::Expired);
    CHECK(d.svc.ui_install() == Fault::NotArmed);
    CHECK(d.host.calls.empty());
  }
  {  // one use
    Bare b;
    NEED(b.checked().state == State::Checked);
    CHECK(b.svc.ui_install() == Fault::Ok);
    b.svc.ui_cancel();  // before the hand-over: dropped
    CHECK(b.svc.ui_install() == Fault::NotArmed);
    CHECK(b.host.calls.empty());
  }
  {  // the card went to a computer and came back: it may have been rewritten
    Bare b;
    NEED(b.checked().state == State::Checked);
    ++b.host.sessions;
    CHECK(b.svc.ui_install() == Fault::NotArmed);
    Bare c;
    NEED(c.checked().state == State::Checked);
    ++c.host.sessions;
    const up::UiStatus s = c.svc.ui_slice();  // and the status says it, so the screen can cancel its question
    CHECK(s.state == State::Failed);
    CHECK(s.fault == Fault::NotArmed);
    CHECK(c.host.calls.empty());
  }
  {  // the card is on the computer now, storage is busy, or the card is another one
    Bare b;
    NEED(b.checked().state == State::Checked);
    b.host.on_host = true;
    CHECK(b.svc.ui_install() == Fault::OnHost);
    Bare c;
    NEED(c.checked().state == State::Checked);
    c.host.busy = true;
    CHECK(c.svc.ui_install() == Fault::CardBusy);
    Bare d;
    NEED(d.checked().state == State::Checked);
    d.store.set_serial("9999-ZZZZ");
    CHECK(d.svc.ui_install() == Fault::NotArmed);
    Bare e;
    NEED(e.checked().state == State::Checked);
    e.store.set_state(false, Fault::WriteProtected);
    CHECK(e.svc.ui_install() == Fault::WriteProtected);
    for (Bare* x : {&b, &c, &d, &e}) {
      CHECK(x->host.calls.empty());
      CHECK(!x->store.has(up::kFwInfoName));
      CHECK(x->svc.phase() == up::FwPhase::Idle);
    }
  }
  {  // the file changed after the check: longer, other bytes with the same size, or no longer an image
    Bare b;
    NEED(b.checked().state == State::Checked);
    b.store.reported_size_delta = 1;
    CHECK(b.svc.ui_install() == Fault::Readback);
    CHECK(b.host.calls.empty());
    Bare c;
    NEED(c.checked().state == State::Checked);
    std::vector<std::uint8_t> other = good_image(3000);
    other[0x500] = static_cast<std::uint8_t>(other[0x500] ^ 0x55u);  // a code byte: still an image, another CRC
    const std::string text = to_hex(other);
    NEED(text.size() == c.img.file.size());
    c.store.put_file(kName, std::vector<std::uint8_t>(text.begin(), text.end()));
    NEED(c.svc.ui_install() == Fault::Ok);  // it starts: the file is only found out by reading it ...
    const up::UiStatus s = c.drive();
    CHECK(s.state == State::Failed);  // ... and it is, before anything is touched
    CHECK(s.fault == Fault::Crc);
    CHECK(s.installing);
    CHECK(c.host.calls.empty());
    CHECK(!c.store.has(up::kFwInfoName));
    CHECK(c.svc.phase() == up::FwPhase::Idle);
    Bare d;
    NEED(d.checked().state == State::Checked);
    std::vector<std::uint8_t> locked = good_image(3000);
    locked[0x40D] = 0x00;
    const std::string bad = to_hex(locked);
    d.store.put_file(kName, std::vector<std::uint8_t>(bad.begin(), bad.end()));
    NEED(d.svc.ui_install() == Fault::Ok);
    const up::UiStatus t = d.drive();
    CHECK(t.state == State::Failed);
    CHECK(t.fault == Fault::BadConfig);  // the flash configuration field is checked again at install, never trusted from the first check
    CHECK(t.installing);
    CHECK(d.host.calls.empty());
    CHECK(!d.host.pin_low);
  }
  {  // _fwinfo cannot be written: the pin is never driven
    Bare b;
    NEED(b.checked().state == State::Checked);
    b.store.fwinfo_fault = Fault::Io;
    NEED(b.svc.ui_install() == Fault::Ok);
    const up::UiStatus s = b.drive();
    CHECK(s.state == State::Failed);
    CHECK(s.fault == Fault::Io);
    CHECK(s.installing);
    CHECK(!b.host.pin_low);
    CHECK(!b.host.called("pin low"));
    const std::vector<std::string> expected_host = {"quiesce"};
    CHECK(b.host.calls == expected_host);
    CHECK(b.svc.phase() == up::FwPhase::Idle);
  }
  {  // a card pulled during the re-check
    Bare b;
    NEED(b.checked().state == State::Checked);
    NEED(b.svc.ui_install() == Fault::Ok);
    (void)b.svc.ui_slice();
    b.store.close_read();
    const up::UiStatus s = b.drive();
    CHECK(s.state == State::Failed);
    CHECK(s.fault == Fault::Io);
    CHECK(b.host.calls.empty());
    // ... or the install is dropped by the user in the re-check
    Bare c;
    NEED(c.checked().state == State::Checked);
    NEED(c.svc.ui_install() == Fault::Ok);
    (void)c.svc.ui_slice();
    c.svc.ui_cancel();
    CHECK(c.svc.phase() == up::FwPhase::Idle);
    CHECK(c.svc.ui_slice().state == State::Idle);
    CHECK(c.host.calls.empty());
  }
}

TEST_CASE("fw client: the console's update and the screen's install make the very same calls to the store and to the board") {
  const Img img;
  // The console: arm, then update with the token.
  Rig r;
  r.store.put_file(kName, img.bytes());
  r.command("fw arm " + std::string(kName) + " " + hex8s(img.crc));
  const std::string token = token_of(r);
  NEED(token.size() == 8u);
  r.store.put_file(up::kFwInfoName, std::vector<std::uint8_t>(3, 'x'));
  r.store.calls.clear();
  r.send("fw update " + token + "\r");
  for (unsigned i = 0; i < 300; ++i) {
    r.step();
  }
  NEED(r.host.pin_low);
  // The screen: check, then install, on its own fakes.
  Bare b;
  NEED(b.checked().state == State::Checked);
  b.store.put_file(up::kFwInfoName, std::vector<std::uint8_t>(3, 'x'));
  b.store.calls.clear();
  NEED(b.svc.ui_install() == Fault::Ok);
  (void)b.drive(nullptr, true);
  NEED(b.host.pin_low);
  CHECK(b.host.calls == r.host.calls);
  CHECK(b.store.calls == r.store.calls);
  CHECK(b.store.text_of(up::kFwInfoName) == r.store.text_of(up::kFwInfoName));
  CHECK(b.svc.last().kind == r.svc.last().kind);
  CHECK(b.svc.phase() == r.svc.phase());
  // The console's wait ends the same way: the pin is released, _fwinfo removed.
  r.host.now += 5000;
  r.step();
  r.step();
  b.host.now += 5000;
  (void)b.svc.ui_slice();
  CHECK(b.host.calls == r.host.calls);
  CHECK(b.store.calls == r.store.calls);
  CHECK(!b.store.has(up::kFwInfoName));
  CHECK(!r.store.has(up::kFwInfoName));
}

TEST_CASE("fw client: the port's adapter lists, refuses, reports and installs through the same service") {
  Bare b;
  b.store.put_file(up::kTempName, std::vector<std::uint8_t>(9, 'y'));
  up::FwServiceSink sink(b.svc);
  CHECK(sink.available());
  // the list
  NEED(sink.list_begin() == fwp::Refusal::None);
  fwp::File f{};
  std::vector<std::string> names;
  std::vector<bool> firsts;
  while (sink.list_next(f)) {
    names.push_back(f.name);
    firsts.push_back(f.reset_target);
    if (names.back() == kName) {
      CHECK(f.size == b.img.file.size());
    }
  }
  sink.list_end();
  CHECK(names == std::vector<std::string>{"PolyendTracker_1.9.0.ptf", kName});
  CHECK(firsts == std::vector<bool>{true, false});
  b.host.on_host = true;
  CHECK(sink.list_begin() == fwp::Refusal::OnComputer);
  CHECK(sink.begin_check(kName) == fwp::Refusal::OnComputer);
  b.host.on_host = false;
  // the refusals, in the port's words
  CHECK(sink.install() == fwp::Refusal::NotChecked);
  CHECK(sink.begin_check("missing.ptf") == fwp::Refusal::NotFound);
  CHECK(sink.begin_check("a b.ptf") == fwp::Refusal::BadName);
  b.host.busy = true;
  CHECK(sink.begin_check(kName) == fwp::Refusal::StorageBusy);
  b.host.busy = false;
  b.store.set_state(false, Fault::WriteProtected);
  CHECK(sink.begin_check(kName) == fwp::Refusal::NotWritable);
  b.store.set_state(false, Fault::NoDir);
  CHECK(sink.begin_check(kName) == fwp::Refusal::NoFolder);
  b.store.set_state(false, Fault::NoCard);
  CHECK(sink.begin_check(kName) == fwp::Refusal::NoCard);
  b.store.set_state(true, Fault::Ok);
  // a failing check
  CHECK(sink.begin_check("PolyendTracker_1.9.0.ptf") == fwp::Refusal::None);
  CHECK(sink.begin_check(kName) == fwp::Refusal::Busy);
  fwp::CheckStatus s;
  unsigned n = 0;
  do {
    s = sink.poll_check();
  } while (s.state == fwp::CheckStatus::State::Running && ++n < 100u);
  CHECK(s.state == fwp::CheckStatus::State::Failed);
  NEED(s.fault != nullptr);
  CHECK(std::strcmp(s.fault, up::fault_name(Fault::HexChar)) == 0);
  CHECK(s.offset == 0u);
  // a passing check, progress, the result
  CHECK(sink.begin_check(kName) == fwp::Refusal::None);
  bool saw_running = false;
  n = 0;
  do {
    s = sink.poll_check();
    if (s.state == fwp::CheckStatus::State::Running) {
      saw_running = true;
      CHECK(s.total == b.img.file.size());
      CHECK(s.fault == nullptr);
    }
  } while (s.state == fwp::CheckStatus::State::Running && ++n < 100u);
  CHECK(saw_running);
  CHECK(s.state == fwp::CheckStatus::State::Checked);
  CHECK(s.image == b.img.length);
  CHECK(s.crc == b.img.crc);
  CHECK(s.fault == nullptr);
  // install: it starts and returns; the status carries the job to the wait for the bootloader and the fallback
  CHECK(sink.install() == fwp::Refusal::None);
  n = 0;
  do {
    s = sink.poll_check();
  } while (s.state == fwp::CheckStatus::State::Running && b.svc.phase() != up::FwPhase::Handover && ++n < 100u);
  CHECK(s.state == fwp::CheckStatus::State::Running);
  CHECK(s.installing);
  CHECK(b.host.pin_low);
  b.host.now += 5000;
  s = sink.poll_check();
  CHECK(s.state == fwp::CheckStatus::State::DidNotStart);
  CHECK(!b.host.pin_low);
  // cancel through the port
  CHECK(sink.begin_check(kName) == fwp::Refusal::None);
  sink.cancel();
  CHECK(sink.poll_check().state == fwp::CheckStatus::State::Idle);
}

TEST_CASE("fw client: every fault maps to its port refusal (the whole table), and every fault has a name") {
  struct Row {
    Fault fault;
    fwp::Refusal refusal;
  };
  // One row per Fault, in the enumeration's order: a fault added later fails the size and order checks until it is given its place here, so the `default` of refusal_of() never
  // decides one unseen.
  constexpr Row kTable[] = {
      {Fault::Ok, fwp::Refusal::None},
      {Fault::HexChar, fwp::Refusal::Io},
      {Fault::HexRecord, fwp::Refusal::Io},
      {Fault::HexDigits, fwp::Refusal::Io},
      {Fault::HexChecksum, fwp::Refusal::Io},
      {Fault::HexType, fwp::Refusal::Io},
      {Fault::HexStart, fwp::Refusal::Io},
      {Fault::HexAfterEof, fwp::Refusal::Io},
      {Fault::HexNoEof, fwp::Refusal::Io},
      {Fault::HexOrder, fwp::Refusal::Io},
      {Fault::HexRange, fwp::Refusal::Io},
      {Fault::HexShort, fwp::Refusal::Io},
      {Fault::BadSp, fwp::Refusal::Io},
      {Fault::BadReset, fwp::Refusal::Io},
      {Fault::BadVector, fwp::Refusal::Io},
      {Fault::BadConfig, fwp::Refusal::Io},
      {Fault::Idle, fwp::Refusal::Io},
      {Fault::Crc, fwp::Refusal::Io},
      {Fault::Readback, fwp::Refusal::NotChecked},  // the file changed since the check (firmware.h: NotChecked)
      {Fault::Disabled, fwp::Refusal::NotWritable},
      {Fault::NoCard, fwp::Refusal::NoCard},
      {Fault::WriteProtected, fwp::Refusal::NotWritable},
      {Fault::NoDir, fwp::Refusal::NoFolder},
      {Fault::BadName, fwp::Refusal::BadName},
      {Fault::Exists, fwp::Refusal::Io},
      {Fault::NotFound, fwp::Refusal::NotFound},
      {Fault::Busy, fwp::Refusal::Busy},
      {Fault::CardBusy, fwp::Refusal::StorageBusy},
      {Fault::NoSpace, fwp::Refusal::Io},
      {Fault::Io, fwp::Refusal::Io},
      {Fault::Unsupported, fwp::Refusal::NotWritable},
      {Fault::LastPtf, fwp::Refusal::Io},
      {Fault::OnHost, fwp::Refusal::OnComputer},
      {Fault::NotArmed, fwp::Refusal::NotChecked},
      {Fault::Expired, fwp::Refusal::Expired},
      {Fault::NameLong, fwp::Refusal::NameTooLong},
      {Fault::NotPadded, fwp::Refusal::Io},  // found by the check (state Failed, the fault text on the screen), like the other image faults
  };
  static_assert(sizeof kTable / sizeof kTable[0] == static_cast<unsigned>(Fault::Count), "one row per Fault");
  for (unsigned i = 0; i < static_cast<unsigned>(Fault::Count); ++i) {
    CAPTURE(i);
    CHECK(static_cast<unsigned>(kTable[i].fault) == i);
    CHECK(up::refusal_of(kTable[i].fault) == kTable[i].refusal);
    CHECK(std::strcmp(up::fault_name(kTable[i].fault), "?") != 0);
  }
}

TEST_CASE("fw client: a second install during the check, the re-check or the hand-over is Busy and changes nothing") {
  {  // during the check
    Bare b;
    NEED(b.svc.ui_check_begin(kName) == Fault::Ok);
    NEED(b.svc.ui_slice().state == State::Running);
    CHECK(b.svc.ui_install() == Fault::Busy);
    up::UiStatus s = b.svc.ui_slice();
    CHECK(s.state == State::Running);  // not Failed: the check goes on, and it is still a check
    CHECK(!s.installing);
    s = b.drive();
    CHECK(s.state == State::Checked);
    CHECK(s.crc == b.img.crc);
    CHECK(b.host.calls.empty());
  }
  {  // during the re-check
    Bare b;
    NEED(b.checked().state == State::Checked);
    NEED(b.svc.ui_install() == Fault::Ok);
    NEED(b.svc.ui_slice().state == State::Running);
    for (unsigned i = 0; i < 3; ++i) {
      CHECK(b.svc.ui_install() == Fault::Busy);
      const up::UiStatus s = b.svc.ui_slice();
      CHECK(s.state == State::Running);
      CHECK(s.installing);  // the screen keeps its "do not power off"
      CHECK(s.fault == Fault::Ok);
    }
    CHECK(b.host.calls.empty());  // nothing touched by the extra calls: the re-check is still reading
    const up::UiStatus s = b.drive(nullptr, true);
    CHECK(b.svc.phase() == up::FwPhase::Handover);
    CHECK(s.state == State::Running);
    CHECK(s.installing);
    CHECK(b.host.pin_low);
  }
  {  // during the hand-over (the pin is low)
    Bare b;
    NEED(b.checked().state == State::Checked);
    NEED(b.svc.ui_install() == Fault::Ok);
    (void)b.drive(nullptr, true);
    NEED(b.svc.phase() == up::FwPhase::Handover);
    const std::vector<std::string> host_calls = b.host.calls;
    const std::vector<std::string> store_calls = b.store.calls;
    for (unsigned i = 0; i < 3; ++i) {
      CHECK(b.svc.ui_install() == Fault::Busy);
      const up::UiStatus s = b.svc.ui_slice();
      CHECK(s.state == State::Running);
      CHECK(s.installing);
      CHECK(s.fault == Fault::Ok);
      CHECK(s.done == s.total);
      CHECK(b.host.pin_low);
    }
    CHECK(b.host.calls == host_calls);
    CHECK(b.store.calls == store_calls);
    CHECK(b.svc.phase() == up::FwPhase::Handover);
    b.host.now += 5000;  // and the 5 s fallback is still the job's outcome
    CHECK(b.svc.ui_slice().state == State::DidNotStart);
    CHECK(!b.host.pin_low);
  }
  {  // the same through the port: Busy, and the status the screen reads is the job's
    Bare b;
    up::FwServiceSink sink(b.svc);
    NEED(sink.begin_check(kName) == fwp::Refusal::None);
    CHECK(sink.install() == fwp::Refusal::Busy);
    fwp::CheckStatus s;
    unsigned n = 0;
    do {
      s = sink.poll_check();
    } while (s.state == fwp::CheckStatus::State::Running && ++n < 100u);
    NEED(s.state == fwp::CheckStatus::State::Checked);
    NEED(sink.install() == fwp::Refusal::None);
    CHECK(sink.install() == fwp::Refusal::Busy);
    s = sink.poll_check();
    CHECK(s.state == fwp::CheckStatus::State::Running);
    CHECK(s.installing);
    CHECK(s.fault == nullptr);
  }
}

TEST_CASE("fw client: a refused ui_check_begin leaves a waiting check and a console token as they were") {
  {  // a passed check that waits for its INSTALL question survives every refusal of a new check
    Bare b;
    NEED(b.checked().state == State::Checked);
    CHECK(b.svc.blocks_injection());
    CHECK(b.svc.ui_check_begin("a b.ptf") == Fault::BadName);
    CHECK(b.svc.ui_check_begin("missing.ptf") == Fault::NotFound);
    b.host.on_host = true;
    CHECK(b.svc.ui_check_begin(kName) == Fault::OnHost);
    b.host.on_host = false;
    b.host.busy = true;
    CHECK(b.svc.ui_check_begin(kName) == Fault::CardBusy);
    b.host.busy = false;
    b.store.set_state(false, Fault::WriteProtected);
    CHECK(b.svc.ui_check_begin(kName) == Fault::WriteProtected);
    b.store.set_state(true, Fault::Ok);
    CHECK(b.svc.blocks_injection());
    const up::UiStatus s = b.svc.ui_slice();
    CHECK(s.state == State::Checked);
    CHECK(s.crc == b.img.crc);
    CHECK(b.svc.ui_install() == Fault::Ok);  // still good: nothing was lost
  }
  {  // a check that starts replaces the waiting one
    Bare b;
    NEED(b.checked().state == State::Checked);
    NEED(b.svc.ui_check_begin(kName) == Fault::Ok);
    CHECK(b.svc.ui_slice().state == State::Running);
    CHECK(b.svc.ui_install() == Fault::Busy);
    CHECK(b.drive().state == State::Checked);
  }
  {  // a console token survives a refused call, and does not survive one that starts
    Rig r;
    const Img img;
    r.store.put_file(kName, img.bytes());
    r.command("fw arm " + std::string(kName) + " " + hex8s(img.crc));
    const std::string token = token_of(r);
    NEED(token.size() == 8u);
    NEED(r.svc.armed());
    CHECK(r.svc.ui_check_begin("a b.ptf") == Fault::BadName);
    r.host.on_host = true;
    CHECK(r.svc.ui_check_begin(kName) == Fault::OnHost);
    r.host.on_host = false;
    CHECK(r.svc.armed());
    r.host.calls.clear();
    r.send("fw update " + token + "\r");
    for (unsigned i = 0; i < 300; ++i) {
      r.step();
    }
    CHECK(r.host.pin_low);  // the token was still good
    Rig q;
    q.store.put_file(kName, img.bytes());
    q.command("fw arm " + std::string(kName) + " " + hex8s(img.crc));
    NEED(q.svc.armed());
    CHECK(q.svc.ui_check_begin(kName) == Fault::Ok);
    CHECK(!q.svc.armed());
  }
}

TEST_CASE("fw client: the HIL injection is refused exactly while the update path is mid-flight, and an abandoned check lapses by the clock") {
  {  // idle, token, check, passed check, hand-over, DidNotStart
    Rig r;
    const Img img;
    r.store.put_file(kName, img.bytes());
    CHECK(!r.svc.blocks_injection());
    r.command("fw arm " + std::string(kName) + " " + hex8s(img.crc));
    CHECK(r.svc.armed());
    CHECK(r.svc.blocks_injection());  // a console token
    r.command("fw");                  // any other command drops it
    CHECK(!r.svc.blocks_injection());
    NEED(r.svc.ui_check_begin(kName) == Fault::Ok);
    CHECK(r.svc.blocks_injection());  // the check is running
    up::UiStatus s;
    for (unsigned i = 0; i < 1000 && (s = r.svc.ui_slice()).state == State::Running; ++i) {
    }
    NEED(s.state == State::Checked);
    CHECK(r.svc.blocks_injection());  // the INSTALL question waits for a pressed key
    r.svc.ui_cancel();
    CHECK(!r.svc.blocks_injection());  // leaving the screen frees it
    NEED(r.svc.ui_check_begin(kName) == Fault::Ok);
    for (unsigned i = 0; i < 1000 && (s = r.svc.ui_slice()).state == State::Running; ++i) {
    }
    NEED(s.state == State::Checked);
    NEED(r.svc.ui_install() == Fault::Ok);
    for (unsigned i = 0; i < 1000 && r.svc.phase() != up::FwPhase::Handover; ++i) {
      (void)r.svc.ui_slice();
    }
    NEED(r.svc.phase() == up::FwPhase::Handover);
    CHECK(r.svc.blocks_injection());
    r.host.now += 5000;
    r.svc.poll(r.shell);
    CHECK(r.svc.ui_slice().state == State::DidNotStart);
    CHECK(!r.svc.blocks_injection());
  }
  {  // a screen that never asks again: poll() lapses the check at 30 s, not before
    Rig r;
    const Img img;
    r.store.put_file(kName, img.bytes());
    NEED(r.svc.ui_check_begin(kName) == Fault::Ok);
    up::UiStatus s;
    for (unsigned i = 0; i < 1000 && (s = r.svc.ui_slice()).state == State::Running; ++i) {
    }
    NEED(s.state == State::Checked);
    r.host.now += 29999;
    r.svc.poll(r.shell);
    CHECK(r.svc.blocks_injection());
    r.host.now += 1;
    r.svc.poll(r.shell);
    CHECK(!r.svc.blocks_injection());
    s = r.svc.ui_slice();
    CHECK(s.state == State::Failed);
    CHECK(s.fault == Fault::Expired);
    CHECK(r.svc.ui_install() == Fault::NotArmed);
    CHECK(!r.host.called("quiesce"));
    CHECK(!r.host.pin_low);
  }
  {  // the card went to a computer: poll() drops the check too
    Rig r;
    const Img img;
    r.store.put_file(kName, img.bytes());
    NEED(r.svc.ui_check_begin(kName) == Fault::Ok);
    up::UiStatus s;
    for (unsigned i = 0; i < 1000 && (s = r.svc.ui_slice()).state == State::Running; ++i) {
    }
    NEED(s.state == State::Checked);
    ++r.host.sessions;
    r.svc.poll(r.shell);
    CHECK(!r.svc.blocks_injection());
  }
}
