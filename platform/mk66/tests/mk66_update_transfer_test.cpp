// SPDX-License-Identifier: GPL-3.0-or-later
//
// `fw put`, `fw arm` and `fw update` (update/fw_service.h, OS-006c) through the real shell, the real console pump and a receive ring, against a fake store: a good
// upload with its acknowledgements and its read-back, every error path (bad name, existing target, a write-protected card, size limits, a HEX defect with the discard of
// the rest of the declared bytes, a wrong CRC, a stream cut short, an idle host, a full volume, a failing write, a corrupted or short read-back, a rename that
// loses), back-pressure, the token (arm, expiry, reuse, wrong token, a card or file that changed) and the hand-over (the order of what happens, the exact
// _fwinfo bytes, the 5 s fallback). Runs on the host and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"
#include "mk66_update_fakes.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace upd_test;
using up::Fault;

namespace {

constexpr const char* kName = "PolyendTracker_0.7.ptf";

// The host side of `fw put`: sends the line, waits for `ready` (or an ERR), then the file with a window of 4096 bytes past the last acknowledgement (what the
// o3console tool does), and runs until the device is quiet. `max_unacked` is the most the host ever had out beyond an acknowledgement.
struct PutRun {
  bool ready = false;
  std::size_t max_unacked = 0;
  std::size_t last_ack = 0;
};

PutRun do_put(Rig& r, const std::string& name, const std::string& file, std::uint32_t announced_crc, std::size_t declared_bytes = 0, bool stop_at_error = false) {
  PutRun run;
  const std::size_t declared = declared_bytes != 0 ? declared_bytes : file.size();
  r.send("fw put " + name + " " + std::to_string(declared) + " " + hex8s(announced_crc) + "\r");
  for (unsigned i = 0; i < 3000 && !r.said("\nready\n") && !r.said("ERR"); ++i) {
    r.step();
  }
  if (!r.said("\nready\n")) {
    r.run();
    return run;
  }
  run.ready = true;
  std::size_t sent = 0;
  std::size_t scanned = 0;
  while (sent < file.size() && r.steps < 400000) {
    const std::string t = r.text();
    for (std::size_t at = t.find('+', scanned); at != std::string::npos; at = t.find('+', at + 1)) {
      const std::size_t eol = t.find('\n', at);
      if (eol == std::string::npos) {
        break;
      }
      run.last_ack = static_cast<std::size_t>(std::strtoul(t.substr(at + 1, eol - at - 1).c_str(), nullptr, 10));
      scanned = eol;
    }
    if (stop_at_error && t.find("ERR") != std::string::npos) {
      break;
    }
    const std::size_t window = run.last_ack + 4096;
    std::size_t allow = window > sent ? window - sent : 0;
    allow = allow > 700 ? 700 : allow;
    if (allow != 0 && r.pending_at >= r.pending.size()) {
      const std::size_t n = file.size() - sent < allow ? file.size() - sent : allow;
      r.send(file.substr(sent, n));
      sent += n;
    }
    run.max_unacked = sent - run.last_ack > run.max_unacked ? sent - run.last_ack : run.max_unacked;
    r.step();
  }
  r.run();
  return run;
}

std::string good_text(std::size_t code = 3000) { return to_hex(good_image(code)); }

}  // namespace

TEST_CASE("fw put: a good upload is acknowledged, validated, read back and renamed") {
  Rig r;
  const std::string file = good_text(9000);  // about 28 KB of HEX: several acknowledgements
  const PutRun run = do_put(r, kName, file, file_crc(file));
  NEED(run.ready);
  const std::string t = r.text();
  CHECK(t.find("ok " + std::string(kName) + " " + std::to_string(file.size()) + " " + hex8s(file_crc(file)) + " image " + std::to_string(good_image(9000).size())) != std::string::npos);  // 0x410 + 9000, padded to 12288
  CHECK(r.said("first-ptf "));
  CHECK(r.said("o3> "));  // the prompt is back
  CHECK(r.store.has(kName));
  CHECK(!r.store.has(up::kTempName));
  const std::vector<std::uint8_t>& stored = r.store.files[kName];
  CHECK(std::string(stored.begin(), stored.end()) == file);  // the bytes on the card are the file's, nothing more
  CHECK(r.svc.phase() == up::FwPhase::Idle);
  CHECK(r.svc.last().kind == up::FwLast::Kind::Put);
  CHECK(r.svc.last().fault == Fault::Ok);
  // The order of the store's calls: create, writes, close, open the temporary file for the read-back, rename. Nothing else.
  CHECK(r.store.calls.front() == std::string("begin ") + kName);
  CHECK(r.store.calls.back() == "commit");
  CHECK(r.store.has_call("close"));
  CHECK(r.store.has_call("open_read _o3up.tmp"));
  CHECK(!r.store.has_call("abort"));
  // Acknowledgements: at least every 4096 bytes, increasing, and the host never had more than 4096 + one packet outstanding; no packet was lost.
  CHECK(run.last_ack > 0u);
  CHECK(run.max_unacked <= 4096u + 700u);
  CHECK(r.rx.dropped() == 0u);
  std::size_t previous = 0;
  std::size_t acks = 0;
  const std::string text = r.text();
  for (std::size_t at = text.find("\n+"); at != std::string::npos; at = text.find("\n+", at + 1)) {
    const std::size_t value = static_cast<std::size_t>(std::strtoul(text.c_str() + at + 2, nullptr, 10));
    CHECK(value > previous);
    CHECK(value - previous <= 4096u);
    previous = value;
    ++acks;
  }
  CHECK(acks >= file.size() / 4096u);
  CHECK(file.size() - previous <= 4096u);
  // One write per pass of the main loop.
  CHECK(r.store.max_appends_per_pass <= 1u);
  // The terminal is the shell's again: the next command runs.
  r.clear();
  r.command("fw");
  CHECK(r.said("fw: write path ENABLED"));
}

TEST_CASE("fw put: the verdict does not depend on the packet size, and nothing is lost when the host is read slowly") {
  const std::string file = good_text(2000);
  for (const std::size_t packet : {std::size_t{1}, std::size_t{7}, std::size_t{63}, std::size_t{64}}) {
    Rig r;
    r.packet = packet;
    const PutRun run = do_put(r, kName, file, file_crc(file));
    NEED(run.ready);
    CHECK_MESSAGE(r.store.has(kName), "packet " << packet);
    CHECK(r.rx.dropped() == 0u);
  }
  {  // the host does not read for a long time: the transmit ring has no room for an acknowledgement, the receiver takes nothing more, nothing is lost
    Rig r;
    const std::string big = good_text(9000);
    r.send("fw put " + std::string(kName) + " " + std::to_string(big.size()) + " " + hex8s(file_crc(big)) + "\r");
    for (unsigned i = 0; i < 400 && !r.said("\nready\n"); ++i) {
      r.step();
    }
    NEED(r.said("\nready\n"));
    r.room_cap = 40;  // room for one short line, and the host reads nothing
    r.host_reads = false;
    r.send(big.substr(0, 6000));
    for (unsigned i = 0; i < 400; ++i) {
      r.step();
    }
    CHECK(r.store.files[up::kTempName].size() < 6000u);  // it stopped when the first acknowledgement used up the room
    CHECK(r.svc.phase() == up::FwPhase::Receiving);
    r.host_reads = true;
    r.room_cap = 3900;
    r.send(big.substr(6000));
    r.run();
    CHECK(r.store.has(kName));
    CHECK(r.rx.dropped() == 0u);
    CHECK(r.naks > 0u);  // the host was held off while the receiver waited
  }
}

TEST_CASE("fw put: refusals are one ERR line and the terminal stays a shell") {
  const std::string file = good_text(500);
  struct Row {
    const char* label;
    std::string line;
    const char* expect;
  };
  const std::string good_args = " " + std::to_string(file.size()) + " " + hex8s(file_crc(file));
  const Row rows[] = {
      {"a bad name", "fw put a..b.ptf" + good_args, "ERR name refused"},
      {"a path in the name", "fw put ../PolyendTracker_0.7.ptf" + good_args, "ERR name refused"},
      {"an existing target", "fw put PolyendTracker_1.9.0.ptf" + good_args, "ERR already exists"},
      {"size 0", "fw put PolyendTracker_0.7.ptf 0 00000000", "ERR unsupported"},
      {"size over 4 MiB", "fw put PolyendTracker_0.7.ptf 4194305 00000000", "ERR unsupported"},
      {"a size that is not a number", "fw put PolyendTracker_0.7.ptf 12x 00000000", "usage: fw put"},
      {"a crc that is not hex", "fw put PolyendTracker_0.7.ptf 100 xyz", "usage: fw put"},
      {"a crc of nine digits", "fw put PolyendTracker_0.7.ptf 100 000000000", "usage: fw put"},
      {"a size that wraps 32 bits", "fw put PolyendTracker_0.7.ptf 4294967299 00000000", "usage: fw put"},
      {"a size of eleven digits", "fw put PolyendTracker_0.7.ptf 99999999999 00000000", "usage: fw put"},
      {"too few arguments", "fw put PolyendTracker_0.7.ptf 100", "usage: fw"},
  };
  for (const Row& row : rows) {
    Rig r;
    r.command(row.line);
    INFO(row.label);
    CHECK(r.said(row.expect));
    CHECK(!r.said("\nready\n"));
    CHECK(!r.shell.raw_active());
    CHECK(!r.store.has(up::kTempName));
    CHECK(r.svc.phase() == up::FwPhase::Idle);
    CHECK(r.said("o3> "));
  }
  {  // a write-protected card, no folder: the store says why (no marker file is needed: OS-FWANY, OS-ANYCARD, see the store test)
    for (const Fault why : {Fault::WriteProtected, Fault::NoDir, Fault::NoCard, Fault::Unsupported}) {
      Rig r;
      r.store.set_state(false, why);
      r.command("fw put" + std::string(" ") + kName + good_args);
      INFO(up::fault_name(why));
      CHECK(r.said((std::string("ERR ") + up::fault_name(why)).c_str()));
      CHECK(!r.store.has(up::kTempName));
      CHECK(!r.shell.raw_active());
    }
  }
  {  // a project is loading: the card is not ours
    Rig r;
    r.host.busy = true;
    r.command(std::string("fw put ") + kName + good_args);
    CHECK(r.said("ERR busy"));
    CHECK(!r.store.has_call("begin"));
  }
  {  // a full volume: said before `ready`
    Rig r;
    r.store.space_left = 100;
    r.command(std::string("fw put ") + kName + good_args);
    CHECK(r.said("ERR no space"));
    CHECK(!r.said("\nready\n"));
    CHECK(r.svc.last().fault == Fault::NoSpace);
  }
}

TEST_CASE("fw put: a defect in the HEX ends the upload at once and the rest of the declared bytes are discarded, not run as commands") {
  std::string file = good_text(3000);
  const std::size_t bad_at = 5000;
  NEED(file.size() >= bad_at + 100);
  file[bad_at] = 'Z';
  // After the defect the file carries text that would be a command if the shell read it.
  const std::string trap = "\rfw\rfw\r";
  file.replace(bad_at + 20, trap.size(), trap);
  Rig r;
  const PutRun run = do_put(r, kName, file, file_crc(file));
  NEED(run.ready);
  CHECK(r.said("ERR bad character at 5000"));
  CHECK(!r.store.has(kName));
  CHECK(!r.store.has(up::kTempName));  // deleted
  CHECK(r.store.has_call("abort"));
  CHECK(!r.said("fw: write path"));    // the trap was not run
  CHECK(r.said("o3> "));               // and the prompt is back after the declared bytes were all in
  CHECK(r.svc.last().fault == Fault::HexChar);
  CHECK(r.svc.last().offset == bad_at);
  CHECK(r.rx.dropped() == 0u);
  // An ERR is printed once.
  CHECK(r.count("ERR ") == 1u);
}

TEST_CASE("fw put: every kind of failure after the bytes arrive leaves nothing on the card") {
  const std::string file = good_text(3000);
  {  // a CRC that is not the file's
    Rig r;
    NEED(do_put(r, kName, file, file_crc(file) ^ 1u).ready);
    CHECK(r.said("ERR crc mismatch"));
    CHECK(!r.store.has(kName));
    CHECK(!r.store.has(up::kTempName));
    CHECK(r.said("o3> "));
  }
  {  // a file without its EOF record
    const std::string cut = to_hex(good_image(3000), "\n", 16, false);
    Rig r;
    NEED(do_put(r, kName, cut, file_crc(cut)).ready);
    CHECK(r.said(("ERR no EOF record at " + std::to_string(cut.size())).c_str()));
    CHECK(!r.store.has(up::kTempName));
  }
  {  // a flash configuration field that could lock the MCU: refused at the end, with its own reason
    std::vector<std::uint8_t> image = good_image(3000);
    image[0x40C] = 0xDC;
    const std::string locked = to_hex(image);
    Rig r;
    NEED(do_put(r, kName, locked, file_crc(locked)).ready);
    CHECK(r.said("ERR flash configuration field is not the required one"));
    CHECK(!r.store.has(kName));
    CHECK(!r.store.has(up::kTempName));
    CHECK(!r.store.has_call("commit"));
    CHECK(!r.store.has_call("open_read"));
  }
  {  // OS-GATE-BOOTCRASH: an image that does not end on a 4 KiB boundary (the SD bootloader would leave stale bytes in its last sector) is refused at the end
    const std::string unpadded = to_hex(good_image(3000, false));
    Rig r;
    NEED(do_put(r, kName, unpadded, file_crc(unpadded)).ready);
    CHECK(r.said(("ERR image not padded: may not boot at " + std::to_string(unpadded.size())).c_str()));
    CHECK(!r.store.has(kName));
    CHECK(!r.store.has(up::kTempName));
    CHECK(!r.store.has_call("commit"));
  }
  {  // a write that fails half way
    Rig r;
    r.store.fail_append_after = 2;
    NEED(do_put(r, kName, file, file_crc(file)).ready);
    CHECK(r.said("ERR i/o error at"));
    CHECK(!r.store.has(up::kTempName));
    CHECK(!r.store.has(kName));
    CHECK(r.said("o3> "));
  }
  {  // a close (sync) that fails
    Rig r;
    r.store.fail_close = true;
    NEED(do_put(r, kName, file, file_crc(file)).ready);
    CHECK(r.said("ERR i/o error"));
    CHECK(!r.store.has(kName));
    CHECK(!r.store.has(up::kTempName));
  }
  {  // what the card gives back differs in one byte
    Rig r;
    r.store.corrupt_readback = true;
    r.store.corrupt_at = 321;
    NEED(do_put(r, kName, file, file_crc(file)).ready);
    CHECK(r.said("ERR read-back mismatch"));
    CHECK(!r.store.has(kName));
    CHECK(!r.store.has(up::kTempName));
    CHECK(!r.store.has_call("commit"));
  }
  {  // what the card gives back is one byte short
    Rig r;
    r.store.short_readback = true;
    NEED(do_put(r, kName, file, file_crc(file)).ready);
    CHECK(r.said("ERR read-back mismatch"));
    CHECK(!r.store.has_call("commit"));
  }
  {  // the name appeared while the upload ran: the rename refuses and the temporary file goes
    Rig r;
    r.store.commit_fault = Fault::Exists;
    NEED(do_put(r, kName, file, file_crc(file)).ready);
    CHECK(r.said("ERR already exists"));
    CHECK(!r.store.has(up::kTempName));
    CHECK(!r.store.has(kName));
  }
  {  // Ctrl-C in the stream is a bad byte like any other
    std::string with_ctrl_c = file;
    with_ctrl_c[300] = '\x03';
    Rig r;
    NEED(do_put(r, kName, with_ctrl_c, file_crc(with_ctrl_c)).ready);
    CHECK(r.said("ERR bad character at 300"));
    CHECK(!r.store.has(up::kTempName));
  }
}

TEST_CASE("fw put: a host that goes quiet for 5 seconds aborts the upload and the prompt returns") {
  const std::string file = good_text(3000);
  Rig r;
  r.send("fw put " + std::string(kName) + " " + std::to_string(file.size()) + " " + hex8s(file_crc(file)) + "\r");
  for (unsigned i = 0; i < 400 && !r.said("\nready\n"); ++i) {
    r.step();
  }
  NEED(r.said("\nready\n"));
  r.send(file.substr(0, 2000));
  for (unsigned i = 0; i < 200; ++i) {
    r.step();
  }
  CHECK(r.store.has(up::kTempName));
  CHECK(r.svc.phase() == up::FwPhase::Receiving);
  r.host.now += 4000;  // 4 s of silence: still waiting
  r.step();
  CHECK(r.svc.phase() == up::FwPhase::Receiving);
  CHECK(!r.said("ERR"));
  r.host.now += 1500;  // past 5 s
  r.step();
  r.step();
  CHECK(r.said("ERR idle timeout at 2000"));
  CHECK(!r.store.has(up::kTempName));
  CHECK(r.svc.phase() == up::FwPhase::Idle);
  CHECK(!r.shell.raw_active());
  CHECK(!r.shell.streaming());
  CHECK(r.said("o3> "));
  // The same silence while discarding after an error also ends it.
  Rig q;
  std::string bad = file;
  bad[200] = '!';
  q.send("fw put " + std::string(kName) + " " + std::to_string(bad.size()) + " " + hex8s(file_crc(bad)) + "\r");
  for (unsigned i = 0; i < 400 && !q.said("\nready\n"); ++i) {
    q.step();
  }
  q.send(bad.substr(0, 1000));
  for (unsigned i = 0; i < 100; ++i) {
    q.step();
  }
  CHECK(q.said("ERR bad character at 200"));
  CHECK(q.svc.phase() == up::FwPhase::Discarding);
  CHECK(!q.said("o3> \r\no3> "));
  q.host.now += 6000;
  q.step();
  q.step();
  CHECK(q.svc.phase() == up::FwPhase::Idle);
  CHECK(!q.shell.raw_active());
}

TEST_CASE("fw rm and the status of `fw`") {
  Rig r;
  r.store.put_file("PolyendTracker_0.1.ptf", std::vector<std::uint8_t>(900, 1));
  r.store.put_file("readme.txt", std::vector<std::uint8_t>(5, 1));
  r.command("fw");
  CHECK(r.said("fw: write path ENABLED"));
  CHECK(r.said("volume 1234-ABCD, FAT32, Firmware/ yes, write-protect no"));  // (no marker line: OS-ANYCARD)
  CHECK_FALSE(r.said("marker"));
  CHECK(r.said("  *      1500  PolyendTracker_1.9.0.ptf"));  // the first .ptf in directory order is the reset button's target
  CHECK(r.said("          900  PolyendTracker_0.1.ptf"));
  CHECK(r.said("upload: idle"));
  CHECK(r.said("last: nothing yet"));
  r.clear();
  r.command("fw rm PolyendTracker_0.1.ptf");
  CHECK(r.said("removed PolyendTracker_0.1.ptf"));
  CHECK(!r.store.has("PolyendTracker_0.1.ptf"));
  r.clear();
  r.command("fw rm PolyendTracker_1.9.0.ptf");
  CHECK(r.said("ERR name refused"));  // an official name
  CHECK(r.store.has("PolyendTracker_1.9.0.ptf"));
  r.clear();
  r.command("fw rm bogus");
  CHECK(r.said("ERR name refused"));
  // OS-007o: while the app uses the card (a project loads, a save runs) nothing is removed, and the message says why
  r.store.put_file("PolyendTracker_0.2.ptf", std::vector<std::uint8_t>(900, 1));
  r.host.busy = true;
  r.clear();
  r.command("fw rm PolyendTracker_0.2.ptf");
  CHECK(r.said("ERR busy: the card is in use (a project is loading or a save is running)"));
  CHECK(r.store.has("PolyendTracker_0.2.ptf"));
  r.host.busy = false;
  r.clear();
  r.store.set_state(false, Fault::WriteProtected);  // a write-protected card: the status says why
  r.command("fw");
  CHECK(r.said("fw: write path OFF: write protected"));
  CHECK(r.said("o3> "));
  r.clear();
  r.store.set_state(true, Fault::Ok);
  r.command("fw");
  CHECK(r.said("fw: write path ENABLED"));
  r.clear();
  r.command("fw frobnicate");
  CHECK(r.said("usage: fw"));
}

namespace {

// A stored image that arms: validated HEX, with its file CRC.
struct Armed {
  std::string file = good_text(2000);
  std::uint32_t crc = file_crc(file);
};

std::string token_of(const Rig& r) {
  const std::string t = r.text();
  const std::size_t at = t.rfind("armed ");
  return at == std::string::npos ? std::string() : t.substr(at + 6, 8);
}

}  // namespace

// OS-BUG-FWNAMELEN: the console never arms a name the SD bootloader mishandles (over 30 characters with .ptf); the file may be on the card, `fw arm` says why and arms nothing.
TEST_CASE("fw arm: a name over 30 characters is refused with a reason and nothing is armed") {
  Armed a;
  const std::string at30 = std::string(up::kUpdateNameMax - 4, 'c') + ".ptf";
  const std::string at31 = std::string(up::kUpdateNameMax - 3, 'c') + ".ptf";
  Rig r;
  r.store.put_file(at30, std::vector<std::uint8_t>(a.file.begin(), a.file.end()));
  r.store.put_file(at31, std::vector<std::uint8_t>(a.file.begin(), a.file.end()));
  r.command("fw arm " + at31 + " " + hex8s(a.crc));
  CHECK(r.said("ERR name over 30 characters"));
  CHECK(!r.svc.armed());
  CHECK(token_of(r).empty());
  CHECK(!r.store.has(up::kFwInfoName));
  r.clear();
  r.command("fw arm " + at30 + " " + hex8s(a.crc));  // the longest proven name arms as before
  CHECK(token_of(r).size() == 8u);
  CHECK(r.svc.armed());
}

// OS-GATE-BOOTCRASH: a file already on the card (copied there by a computer, or stored by an older build) that does not end on a 4 KiB boundary is never armed:
// 1 byte short is refused with its own reason, the boundary exactly arms, and a refused arm hands nothing over.
TEST_CASE("fw arm: an image not padded to a 4 KiB sector is refused with a reason, nothing is armed or handed over") {
  const std::size_t sector = 4096;
  const std::string short_by_one = to_hex(good_image(sector - 0x410 - 1, false));
  const std::string exact = to_hex(good_image(sector - 0x410, false));
  const std::string v100_like = to_hex(good_image(3000, false));
  Rig r;
  r.store.put_file("short1.ptf", std::vector<std::uint8_t>(short_by_one.begin(), short_by_one.end()));
  r.store.put_file("exact.ptf", std::vector<std::uint8_t>(exact.begin(), exact.end()));
  r.store.put_file("v100.ptf", std::vector<std::uint8_t>(v100_like.begin(), v100_like.end()));
  for (const char* name : {"short1.ptf", "v100.ptf"}) {
    r.clear();
    r.command(std::string("fw arm ") + name + " " + hex8s(file_crc(name[0] == 's' ? short_by_one : v100_like)));
    CHECK_MESSAGE(r.said("ERR image not padded: may not boot"), name);
    CHECK(!r.svc.armed());
    CHECK(token_of(r).empty());
    CHECK(!r.store.has(up::kFwInfoName));
    CHECK(!r.host.called("quiesce"));
    CHECK(!r.host.pin_low);
  }
  r.clear();
  r.command("fw arm exact.ptf " + hex8s(file_crc(exact)));
  CHECK(token_of(r).size() == 8u);
  CHECK(r.svc.armed());
}

TEST_CASE("fw arm: the file is re-read and fully re-validated, and the token is single use") {
  Armed a;
  Rig r;
  r.store.put_file(kName, std::vector<std::uint8_t>(a.file.begin(), a.file.end()));
  r.command("fw arm " + std::string(kName) + " " + hex8s(a.crc));
  const std::string token = token_of(r);
  NEED(token.size() == 8u);
  CHECK(r.svc.armed());
  CHECK(r.said("o3> "));
  CHECK(!r.host.called("quiesce"));  // arming changes nothing
  CHECK(!r.store.has(up::kFwInfoName));
  // The wrong CRC, a bad name, a missing file, a file that is not an image, a card that is not writable.
  {
    Rig q;
    q.store.put_file(kName, std::vector<std::uint8_t>(a.file.begin(), a.file.end()));
    q.command("fw arm " + std::string(kName) + " " + hex8s(a.crc ^ 1u));
    CHECK(q.said("ERR crc mismatch"));
    CHECK(!q.svc.armed());
    q.clear();
    q.command("fw arm PolyendTracker_0.8.ptf " + hex8s(a.crc));
    CHECK(q.said("ERR not found"));
    q.clear();
    q.command("fw arm a..b.ptf " + hex8s(a.crc));
    CHECK(q.said("usage: fw arm"));
    q.clear();
    std::vector<std::uint8_t> locked(a.file.begin(), a.file.end());
    q.store.put_file("PolyendTracker_0.9.ptf", locked);
    // a file on the card whose flash configuration is wrong (written there by something else)
    std::vector<std::uint8_t> image = good_image(2000);
    image[0x40D] = 0x00;
    const std::string bad = to_hex(image);
    q.store.put_file("PolyendTracker_0.9.ptf", std::vector<std::uint8_t>(bad.begin(), bad.end()));
    q.command("fw arm PolyendTracker_0.9.ptf " + hex8s(file_crc(bad)));
    CHECK(q.said("ERR flash configuration field is not the required one"));
    CHECK(!q.svc.armed());
    q.clear();
    q.store.set_state(false, Fault::WriteProtected);
    q.command("fw arm " + std::string(kName) + " " + hex8s(a.crc));
    CHECK(q.said("ERR write protected"));
  }
  // Any other fw command cancels the token.
  r.clear();
  r.command("fw");
  CHECK(!r.svc.armed());
  r.clear();
  r.command("fw update " + token);
  CHECK(r.said("ERR not armed"));
  CHECK(!r.host.called("quiesce"));
  CHECK(!r.host.pin_low);
}

TEST_CASE("fw update: a wrong token, an old token, a token used twice, a changed card or file hand over nothing") {
  Armed a;
  auto armed_rig = [&](Rig& r) {
    r.store.put_file(kName, std::vector<std::uint8_t>(a.file.begin(), a.file.end()));
    r.command("fw arm " + std::string(kName) + " " + hex8s(a.crc));
    return token_of(r);
  };
  {  // a wrong token (and the right one afterwards no longer works)
    Rig r;
    const std::string token = armed_rig(r);
    NEED(token.size() == 8u);
    const std::string wrong = token == "00000000" ? "00000001" : "00000000";
    r.clear();
    r.command("fw update " + wrong);
    CHECK(r.said("ERR not armed for that token"));
    r.clear();
    r.command("fw update " + token);
    CHECK(r.said("ERR not armed"));
    CHECK(r.host.calls.empty());
    CHECK(!r.store.has(up::kFwInfoName));
  }
  {  // expired: 30 s
    Rig r;
    const std::string token = armed_rig(r);
    r.host.now += 30000;
    r.clear();
    r.command("fw update " + token);
    CHECK(r.said("ERR token expired"));
    CHECK(r.host.calls.empty());
  }
  {  // just inside 30 s works
    Rig r;
    const std::string token = armed_rig(r);
    r.host.now += 29000;
    r.clear();
    r.command("fw update " + token);
    CHECK(r.said("update: handing over to the bootloader"));
  }
  {  // the file changed after arm (same size): the update re-validates it and refuses, touching nothing
    Rig r;
    const std::string token = armed_rig(r);
    NEED(token.size() == 8u);
    std::string altered = a.file;
    altered[200] = altered[200] == '0' ? '1' : '0';  // a data digit: the checksum of that record no longer adds up
    r.store.put_file(kName, std::vector<std::uint8_t>(altered.begin(), altered.end()));
    r.clear();
    r.command("fw update " + token);
    CHECK(r.said("ERR"));
    CHECK(!r.said("handing over"));
    CHECK(r.host.calls.empty());  // no quiesce, no pin
    CHECK(!r.host.pin_low);
    CHECK(!r.store.has(up::kFwInfoName));
    CHECK(!r.store.has_call("write_fwinfo"));
    CHECK(r.said("o3> "));
    CHECK(r.svc.phase() == up::FwPhase::Idle);
  }
  {  // a project is loading
    Rig r;
    const std::string token = armed_rig(r);
    r.host.busy = true;
    r.clear();
    r.command("fw update " + token);
    CHECK(r.said("ERR busy"));
    CHECK(r.host.calls.empty());
  }
  {  // not a token at all
    Rig r;
    const std::string token = armed_rig(r);
    r.clear();
    r.command("fw update zz");
    CHECK(r.said("usage: fw update"));
    CHECK(!r.svc.armed());  // a use of `fw update`, parsed or not, spends the token
  }
  {  // the card is a different one
    Rig r;
    const std::string token = armed_rig(r);
    r.store.set_serial("DEAD-BEEF");
    r.clear();
    r.command("fw update " + token);
    CHECK(r.said("ERR the card changed since arm"));
    CHECK(r.host.calls.empty());
  }
  {  // the file is not the size it was
    Rig r;
    const std::string token = armed_rig(r);
    r.store.reported_size_delta = 1;
    r.clear();
    r.command("fw update " + token);
    CHECK(r.said("ERR"));
    CHECK(r.host.calls.empty());
    CHECK(!r.store.has(up::kFwInfoName));
  }
  {  // the card stopped being writable
    Rig r;
    const std::string token = armed_rig(r);
    r.store.set_state(false, Fault::WriteProtected);
    r.clear();
    r.command("fw update " + token);
    CHECK(r.said("ERR write protected"));
    CHECK(r.host.calls.empty());
  }
  {  // _fwinfo cannot be written: the outputs were muted but the pin is never touched
    Rig r;
    const std::string token = armed_rig(r);
    r.store.fwinfo_fault = Fault::Io;
    r.clear();
    r.command("fw update " + token);
    CHECK(r.said("ERR i/o error"));
    CHECK(r.host.called("quiesce"));
    CHECK(!r.host.called("pin low"));
    CHECK(!r.host.pin_low);
    CHECK(r.said("o3> "));
  }
}

TEST_CASE("fw update: the hand-over happens in the documented order, _fwinfo is the bare name, and the 5 s fallback releases the pin") {
  Armed a;
  Rig r;
  r.store.put_file(kName, std::vector<std::uint8_t>(a.file.begin(), a.file.end()));
  r.store.put_file(up::kFwInfoName, std::vector<std::uint8_t>(3, 'x'));  // a stale one
  r.command("fw arm " + std::string(kName) + " " + hex8s(a.crc));
  const std::string token = token_of(r);
  NEED(token.size() == 8u);
  r.clear();
  r.store.calls.clear();
  r.send("fw update " + token + "\r");
  for (unsigned i = 0; i < 200; ++i) {
    r.step();
  }
  // The pin is low, the hand-over line was printed before that, and _fwinfo is exactly the name.
  CHECK(r.host.pin_low);
  CHECK(r.said(("update: handing over to the bootloader (" + std::string(kName) + ")").c_str()));
  CHECK(r.store.text_of(up::kFwInfoName) == kName);  // no folder, no newline, no NUL
  CHECK(r.store.files[up::kFwInfoName].size() == std::strlen(kName));
  const std::vector<std::string> expected_host = {"quiesce", "drain 40", "pin low"};
  CHECK(r.host.calls == expected_host);
  const std::vector<std::string> expected_store = {"open_read " + std::string(kName), "remove_fwinfo", std::string("write_fwinfo ") + kName};
  CHECK(r.store.calls == expected_store);
  CHECK(r.host.out_size_at_pin_low >= r.text().find("update: handing over") + 20u);  // the line was out before the pin went low
  // Until the 5 s are over the console is held (a stream owns it) and the pin stays low.
  CHECK(r.svc.phase() == up::FwPhase::Handover);
  CHECK(r.shell.streaming());
  r.host.now += 4700;  // about 4.9 s since the request
  r.step();
  CHECK(r.host.pin_low);
  CHECK(!r.said("ERR"));
  // The chip is still running 5 s after the request: the pin is released, _fwinfo deleted, and it is said.
  r.host.now += 200;
  r.step();
  r.step();
  CHECK(!r.host.pin_low);
  CHECK(r.host.called("pin release"));
  CHECK(!r.store.has(up::kFwInfoName));
  CHECK(r.said("ERR bootloader did not take over"));
  CHECK(r.said("o3> "));
  CHECK(!r.shell.streaming());
  CHECK(r.svc.phase() == up::FwPhase::Idle);
  // The pin is driven low exactly once per update and never by anything else.
  unsigned lows = 0;
  for (const std::string& c : r.host.calls) {
    lows += c == "pin low" ? 1u : 0u;
  }
  CHECK(lows == 1u);
}

TEST_CASE("fw: a terminal that goes away mid-upload abandons it; a started hand-over still ends by its own clock") {
  {  // the cable is pulled in the middle of a put: DTR drops, the console stops the shell's stream and tells the service
    const std::string file = good_text(3000);
    Rig r;
    r.send("fw put " + std::string(kName) + " " + std::to_string(file.size()) + " " + hex8s(file_crc(file)) + "\r");
    for (unsigned i = 0; i < 400 && !r.said("\nready\n"); ++i) {
      r.step();
    }
    r.send(file.substr(0, 2500));
    for (unsigned i = 0; i < 200; ++i) {
      r.step();
    }
    NEED(r.store.has(up::kTempName));
    NEED(r.shell.raw_active());
    r.shell.stop_stream();       // what console::service() does when DTR drops
    r.svc.terminal_closed();
    CHECK(!r.shell.raw_active());
    CHECK(!r.shell.streaming());
    CHECK(r.svc.phase() == up::FwPhase::Idle);
    CHECK(!r.store.has(up::kTempName));
    CHECK(!r.store.has(kName));
    // The next terminal gets a shell again, and a new put works.
    r.clear();
    r.shell.begin("test");
    r.command("fw");
    CHECK(r.said("fw: write path ENABLED"));
    r.clear();
    NEED(do_put(r, kName, file, file_crc(file)).ready);
    CHECK(r.store.has(kName));
  }
  {  // the terminal is closed during the wait for the bootloader: the shell's stream is gone, the pin is still released after 5 s and _fwinfo deleted
    Armed a;
    Rig r;
    r.store.put_file(kName, std::vector<std::uint8_t>(a.file.begin(), a.file.end()));
    r.command("fw arm " + std::string(kName) + " " + hex8s(a.crc));
    const std::string token = token_of(r);
    NEED(token.size() == 8u);
    r.send("fw update " + token + "\r");
    for (unsigned i = 0; i < 100; ++i) {
      r.step();
    }
    NEED(r.host.pin_low);
    r.shell.stop_stream();
    r.svc.terminal_closed();     // a hand-over in progress is not abandoned by this
    CHECK(r.svc.phase() == up::FwPhase::Handover);
    r.host.now += 3000;
    r.svc.poll(r.shell);
    CHECK(r.host.pin_low);
    r.host.now += 2500;
    r.svc.poll(r.shell);         // the main loop's poll, no stream involved
    CHECK(!r.host.pin_low);
    CHECK(!r.store.has(up::kFwInfoName));
    CHECK(r.svc.phase() == up::FwPhase::Idle);
    CHECK(r.host.called("pin release"));
  }
}

// ---- OS-USBDISK U1b: the card on a computer ------------------------------------------------------------------------------------------

TEST_CASE("fw on a card handed to a computer: put, rm, arm and update answer ERR card on host and touch nothing; a token armed before is dead after") {
  Armed a;
  Rig r;
  r.store.put_file(kName, std::vector<std::uint8_t>(a.file.begin(), a.file.end()));
  r.command("fw arm " + std::string(kName) + " " + hex8s(a.crc));
  const std::string token = token_of(r);
  NEED(token.size() == 8u);
  NEED(r.svc.armed());

  // the card goes to the computer and comes back (an eject) before the token is used
  r.host.on_host = true;
  ++r.host.sessions;
  for (const char* cmd : {"fw put PolyendTracker_0.8.ptf 100 0", "fw rm PolyendTracker_0.8.ptf", "fw arm PolyendTracker_0.7.ptf 0", "fw update 00000000"}) {
    r.clear();
    r.command(cmd);
    CHECK(r.said("ERR card on host"));
  }
  CHECK(!r.svc.armed());
  CHECK(r.svc.phase() == up::FwPhase::Idle);
  r.host.on_host = false;  // the eject
  r.clear();
  r.command("fw update " + token);
  CHECK(r.said("ERR not armed"));
  CHECK(!r.host.called("quiesce"));
  CHECK(!r.host.pin_low);

  // a token armed, then a hand-over and an eject with NO other fw command in between: `fw update` alone finds it dead (the loop above revoked its own token)
  r.clear();
  r.command("fw arm " + std::string(kName) + " " + hex8s(a.crc));
  const std::string token2 = token_of(r);
  NEED(token2.size() == 8u);
  NEED(r.svc.armed());
  ++r.host.sessions;  // the hand-over happened and the card is back (on_host false)
  r.clear();
  r.command("fw update " + token2);
  CHECK(r.said("ERR not armed"));
  CHECK(!r.svc.armed());
  CHECK(!r.host.called("quiesce"));

  // poll() alone also kills a token when a hand-over happened since arm (the status shows it)
  r.clear();
  r.command("fw arm " + std::string(kName) + " " + hex8s(a.crc));
  NEED(r.svc.armed());
  ++r.host.sessions;
  r.svc.poll(r.shell);
  CHECK(!r.svc.armed());
}

TEST_CASE("fw phases: the update path holds the card in every phase but Idle and Done") {
  using P = up::FwPhase;
  CHECK_FALSE(up::fw_phase_holds_card(P::Idle));
  CHECK(up::fw_phase_holds_card(P::Receiving));
  CHECK(up::fw_phase_holds_card(P::Discarding));
  CHECK(up::fw_phase_holds_card(P::Finalizing));
  CHECK(up::fw_phase_holds_card(P::Verifying));
  CHECK(up::fw_phase_holds_card(P::Arming));
  CHECK(up::fw_phase_holds_card(P::Rechecking));
  CHECK(up::fw_phase_holds_card(P::Handover));
  CHECK_FALSE(up::fw_phase_holds_card(P::Done));
}
