// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/mk66/update/fw_service.h>

#include <cstring>

namespace o3os::platform::mk66::update {
namespace {

namespace shell = o3os::usb::diag;

constexpr std::size_t kAckRoom = 32;       // bytes of output room an upload wants before it takes more input (an acknowledgement line)
constexpr unsigned kListMax = 16;          // entries `fw` prints (the reply must stay under the shell's 1.6 KB)

bool same(const char* a, const char* b) noexcept { return std::strcmp(a, b) == 0; }

// Decimal digits only, 1..max.
bool parse_dec(const char* s, std::uint32_t max, std::uint32_t& out) noexcept {
  if (s[0] == '\0') {
    return false;
  }
  std::uint32_t v = 0;
  for (; *s != '\0'; ++s) {
    if (*s < '0' || *s > '9') {
      return false;
    }
    const std::uint32_t d = static_cast<std::uint32_t>(*s - '0');
    if (v > (max - d) / 10u) {
      return false;  // v * 10 + d would pass max (and cannot wrap: it is checked before the multiplication)
    }
    v = v * 10u + d;
  }
  out = v;
  return true;
}

// 1..8 hexadecimal digits, no prefix.
bool parse_hex(const char* s, std::uint32_t& out) noexcept {
  std::uint32_t v = 0;
  unsigned digits = 0;
  for (; *s != '\0'; ++s, ++digits) {
    unsigned d;
    if (*s >= '0' && *s <= '9') {
      d = static_cast<unsigned>(*s - '0');
    } else if (*s >= 'a' && *s <= 'f') {
      d = static_cast<unsigned>(*s - 'a' + 10);
    } else if (*s >= 'A' && *s <= 'F') {
      d = static_cast<unsigned>(*s - 'A' + 10);
    } else {
      return false;
    }
    if (digits == 8u) {
      return false;
    }
    v = (v << 4) | d;
  }
  if (digits == 0u) {
    return false;
  }
  out = v;
  return true;
}

void hex8(shell::Output& out, std::uint32_t v) noexcept {
  char text[8];
  for (unsigned i = 0; i < 8; ++i) {
    const unsigned nibble = (v >> (28u - 4u * i)) & 0xFu;
    text[i] = static_cast<char>(nibble < 10u ? '0' + nibble : 'A' + (nibble - 10u));
  }
  out.text(text, sizeof text);
}

void copy_name(char* dst, const char* src) noexcept {
  std::size_t n = 0;
  while (src[n] != '\0' && n < kImageNameMax) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
}

void pad_dec(shell::Output& out, std::uint32_t v, unsigned width) noexcept {
  unsigned digits = 1;
  for (std::uint32_t t = v; t >= 10u; t /= 10u) {
    ++digits;
  }
  for (; digits < width; ++digits) {
    out.chr(' ');
  }
  out.dec(v);
}

void err(shell::Output& out, Fault f) noexcept { out.text("ERR ").text(fault_name(f)).nl(); }

}  // namespace

std::size_t FwService::raw_thunk(shell::Shell& sh, const std::uint8_t* bytes, std::size_t n, void* self) noexcept {
  return static_cast<FwService*>(self)->take(sh, bytes, n);
}

bool FwService::stream_thunk(shell::Shell& sh, void* self) noexcept { return static_cast<FwService*>(self)->tick(sh); }

void FwService::run(shell::Shell& sh, const shell::Args& a) noexcept {
  const bool is_update = a.argc == 3 && same(a.argv[1], "update");
  if (!is_update) {
    armed_ = false;  // any other `fw` command cancels a token
  }
  ui_armed_ = false;  // ... and a client check that waits for ui_install()
  if (phase_ != FwPhase::Idle) {
    sh.out().text("ERR busy\n");
    return;
  }
  client_ = false;  // the console has the service
  const bool card_command = a.argc > 1 && (same(a.argv[1], "put") || same(a.argv[1], "rm") || same(a.argv[1], "arm") || is_update);
  if (card_command && host_.card_on_host()) {
    armed_ = false;
    err(sh.out(), Fault::OnHost);
    return;
  }
  if (a.argc == 1) {
    status(sh);
  } else if (same(a.argv[1], "put") && a.argc == 5) {
    put(sh, a);
  } else if (same(a.argv[1], "rm") && a.argc == 3) {
    rm(sh, a);
  } else if (same(a.argv[1], "arm") && a.argc == 4) {
    arm(sh, a);
  } else if (is_update) {
    update(sh, a);
  } else {
    sh.out().text("usage: fw | fw put <name> <bytes> <crc32hex> | fw rm <name> | fw arm <name> <crc32hex> | fw update <token>\n");
  }
}

// ---- fw ---------------------------------------------------------------------------------------------------------------------------
void FwService::print_first_ptf(shell::Shell& sh) noexcept {
  FwEntry e;
  if (store_.list_begin() == Fault::Ok) {
    while (store_.list_next(e)) {
      if (is_ptf_name(e.name)) {
        sh.out().text("first-ptf ").text(e.name).nl();
        store_.list_end();
        return;
      }
    }
  }
  store_.list_end();
  sh.out().text("first-ptf (none)\n");
}

void FwService::status(shell::Shell& sh) noexcept {
  shell::Output& out = sh.out();
  const WriteState w = store_.state();
  out.text("fw: write path ").text(w.enabled ? "ENABLED" : "OFF: ");
  if (!w.enabled) {
    out.text(fault_name(w.why));
  }
  out.nl();
  if (w.mounted) {
    out.text("card: volume ").text(w.serial[0] != '\0' ? w.serial : "?").text(", ").text(w.fs).text(", Firmware/ ").text(w.dir ? "yes" : "no").text(", write-protect ")
        .text(w.write_protected ? "yes" : "no").nl();
  } else {
    out.text("card: not mounted\n");
  }
  out.text("Firmware/ in directory order (* = the .ptf the reset button flashes):\n");
  FwEntry e;
  bool first_marked = false;
  unsigned shown = 0;
  unsigned total = 0;
  if (store_.list_begin() == Fault::Ok) {
    while (store_.list_next(e)) {
      ++total;
      const bool first = !first_marked && is_ptf_name(e.name);
      first_marked = first_marked || first;
      if (shown < kListMax) {
        out.text(first ? "  * " : "    ");
        pad_dec(out, e.size, 9);
        out.text("  ").text(e.name).nl();
        ++shown;
      }
    }
  }
  store_.list_end();
  if (total == 0u) {
    out.text("    (empty or not readable)\n");
  } else if (total > shown) {
    out.text("    ... ").dec(total - shown).text(" more\n");
  }
  out.text("upload: ");
  switch (phase_) {
    case FwPhase::Idle:
    case FwPhase::Done: out.text("idle"); break;
    case FwPhase::Receiving: out.text("receiving ").dec(received_).text(" of ").dec(declared_); break;
    case FwPhase::Discarding: out.text("discarding after an error"); break;
    case FwPhase::Finalizing:
    case FwPhase::Verifying: out.text("verifying"); break;
    case FwPhase::Arming:
    case FwPhase::Rechecking: out.text("arming"); break;
    case FwPhase::Handover: out.text("handing over"); break;
  }
  out.text(armed_ ? ", armed\n" : "\n");
  out.text("last: ");
  switch (last_.kind) {
    case FwLast::Kind::None: out.text("nothing yet"); break;
    case FwLast::Kind::Put:
    case FwLast::Kind::Arm:
      out.text(last_.kind == FwLast::Kind::Put ? "put " : "arm ");
      if (last_.fault == Fault::Ok) {
        out.text("ok ").text(last_.name).chr(' ').dec(last_.bytes).chr(' ');
        hex8(out, last_.crc);
        out.text(" image ").dec(last_.image);
      } else {
        out.text("ERR ").text(fault_name(last_.fault)).text(" at ").dec(last_.offset);
      }
      break;
    case FwLast::Kind::Update:
      out.text("update ").text(last_.name).text(": ").text(fault_name(last_.fault));
      break;
  }
  out.nl();
}

// ---- fw rm ------------------------------------------------------------------------------------------------------------------------
void FwService::rm(shell::Shell& sh, const shell::Args& a) noexcept {
  if (host_.storage_busy()) {  // OS-007o: FatFs is not re-entrant; a removal never interleaves with a save or a project load
    err(sh.out(), Fault::CardBusy);
    return;
  }
  const Fault f = store_.remove(a.argv[2]);
  if (f == Fault::Ok) {
    sh.out().text("removed ").text(a.argv[2]).nl();
    host_.log("fw: file removed");
  } else {
    err(sh.out(), f);
  }
}

// ---- fw put -----------------------------------------------------------------------------------------------------------------------
void FwService::put(shell::Shell& sh, const shell::Args& a) noexcept {
  std::uint32_t bytes = 0;
  std::uint32_t crc = 0;
  if (!parse_dec(a.argv[3], 0xFFFFFFFEu, bytes) || !parse_hex(a.argv[4], crc)) {
    sh.out().text("usage: fw put <name> <bytes> <crc32hex>\n");
    return;
  }
  if (host_.storage_busy()) {
    err(sh.out(), Fault::CardBusy);
    return;
  }
  const Fault f = store_.begin_upload(a.argv[2], bytes);
  if (f != Fault::Ok) {
    last_ = FwLast{};
    last_.kind = FwLast::Kind::Put;
    last_.fault = f;
    copy_name(last_.name, a.argv[2]);
    err(sh.out(), f);
    return;
  }
  v_.reset();
  fill_ = 0;
  received_ = 0;
  acked_ = 0;
  declared_ = bytes;
  remaining_ = bytes;
  crc_announced_ = crc;
  copy_name(name_, a.argv[2]);
  idle_since_ = host_.now_ms();
  phase_ = FwPhase::Receiving;
  host_.log("fw put: receiving");
  sh.out().text("ready\n");
  sh.out().flush();
  sh.start_raw(&raw_thunk, this);
  sh.start_stream(&stream_thunk, this, false);
}

void FwService::fail(shell::Shell* sh, Fault f, std::uint32_t offset) noexcept {
  store_.close_read();
  store_.abort_upload();  // deletes the temporary file; nothing when there is no upload (an arm)
  const bool arming = phase_ == FwPhase::Arming;
  const bool rechecking = phase_ == FwPhase::Rechecking;
  last_ = FwLast{};
  last_.kind = rechecking ? FwLast::Kind::Update : (arming ? FwLast::Kind::Arm : FwLast::Kind::Put);
  last_.fault = f;
  last_.offset = offset;
  copy_name(last_.name, name_);
  host_.log(rechecking ? "update: image failed its re-check, not handing over" : (arming ? "fw arm: refused" : "fw put: refused, temp file deleted"));
  if (sh == nullptr) {  // a client operation: the outcome is for ui_slice()
    ui_end(UiStatus::State::Failed, f, offset);
    phase_ = FwPhase::Idle;
    return;
  }
  sh->out().text("ERR ").text(fault_name(f)).text(" at ").dec(offset).nl();
  if (phase_ == FwPhase::Receiving && remaining_ > 0u && f != Fault::Idle) {
    phase_ = FwPhase::Discarding;  // swallow what the host still sends of the file; the prompt comes back when it is all in
    idle_since_ = host_.now_ms();
  } else {
    sh->end_raw();
    phase_ = FwPhase::Done;
  }
}

std::size_t FwService::take(shell::Shell& sh, const std::uint8_t* bytes, std::size_t n) noexcept {
  const std::uint32_t now = host_.now_ms();
  if (phase_ == FwPhase::Discarding) {
    const std::size_t use = n < remaining_ ? n : remaining_;
    remaining_ -= static_cast<std::uint32_t>(use);
    if (use != 0u) {
      idle_since_ = now;
    }
    if (remaining_ == 0u) {
      sh.end_raw();
      phase_ = FwPhase::Done;
    }
    return use;
  }
  if (phase_ != FwPhase::Receiving) {
    sh.end_raw();  // nothing is waiting for raw bytes: whoever installed this is gone
    return 0;
  }
  if (sh.out().room() < kAckRoom) {
    return 0;  // no room for an acknowledgement: wait (the host is NAKed once the receive ring fills)
  }
  if (fill_ == sizeof buf_) {  // one write per pass: the full buffer goes out now and the rest of the input waits for the next pass
    const Fault f = store_.append(buf_, fill_);
    fill_ = 0;
    if (f != Fault::Ok) {
      fail(&sh, f, received_);
    }
    return 0;
  }
  std::size_t use = n < remaining_ ? n : remaining_;
  if (use > sizeof buf_ - fill_) {
    use = sizeof buf_ - fill_;
  }
  const bool good = v_.feed(bytes, use);
  std::memcpy(buf_ + fill_, bytes, use);
  fill_ += use;
  received_ += static_cast<std::uint32_t>(use);
  remaining_ -= static_cast<std::uint32_t>(use);
  idle_since_ = now;
  if (!good) {
    fail(&sh, v_.fault(), v_.fault_offset());
    return use;
  }
  if (received_ - acked_ >= kAckEveryBytes && remaining_ != 0u) {
    sh.out().chr('+').dec(received_).nl();
    acked_ = received_;
  }
  if (remaining_ == 0u) {
    sh.end_raw();
    phase_ = FwPhase::Finalizing;
  }
  return use;
}

// ---- the stream: idle timeout, the end of an upload, the read-back and the arm, the wait for the bootloader -------------------------
bool FwService::tick(shell::Shell& sh) noexcept {
  if (client_) {
    return false;  // a client operation owns the service: the stream is a console's, left over from an operation that has ended
  }
  const std::uint32_t now = host_.now_ms();
  switch (phase_) {
    case FwPhase::Idle: return false;
    case FwPhase::Done:
      phase_ = FwPhase::Idle;
      return false;
    case FwPhase::Receiving:
      if (now - idle_since_ >= kIdleTimeoutMs) {
        fail(&sh, Fault::Idle, received_);
      }
      return true;
    case FwPhase::Discarding:
      if (remaining_ == 0u || now - idle_since_ >= kIdleTimeoutMs) {
        sh.end_raw();
        phase_ = FwPhase::Idle;
        return false;
      }
      return true;
    case FwPhase::Finalizing: {
      if (fill_ != 0u) {  // the tail of the last buffer, one write this pass
        const Fault f = store_.append(buf_, fill_);
        fill_ = 0;
        if (f != Fault::Ok) {
          fail(&sh, f, received_);
        }
        return true;
      }
      if (!v_.finish()) {
        fail(&sh, v_.fault(), v_.fault_offset());
        return true;
      }
      if (v_.crc() != crc_announced_) {
        fail(&sh, Fault::Crc, received_);
        return true;
      }
      image_len_ = v_.image_length();
      crc_seen_ = v_.crc();
      Fault f = store_.close_upload();
      if (f == Fault::Ok) {
        v_.reset();
        read_total_ = 0;
        f = store_.open_read(kTempName, read_size_);
        if (f == Fault::Ok && read_size_ != declared_) {
          f = Fault::Readback;
        }
      }
      if (f != Fault::Ok) {
        fail(&sh, f, received_);
        return true;
      }
      phase_ = FwPhase::Verifying;
      return true;
    }
    case FwPhase::Verifying:
    case FwPhase::Arming:
    case FwPhase::Rechecking: return verify_slice(&sh);
    case FwPhase::Handover: return handover_check(&sh);
  }
  return false;
}

// The wait for the bootloader: true while it goes on. After kHandoverWaitMs the pin is let go and _fwinfo deleted, and it is said.
bool FwService::handover_check(shell::Shell* sh) noexcept {
  if (phase_ != FwPhase::Handover) {
    return false;
  }
  if (host_.now_ms() - handover_at_ < kHandoverWaitMs) {
    return true;
  }
  host_.bootloader_request(false);  // release the pin
  (void)store_.remove_fwinfo();
  host_.log("update: the bootloader did not take over");
  last_ = FwLast{};
  last_.kind = FwLast::Kind::Update;
  last_.fault = Fault::Io;
  copy_name(last_.name, armed_name_);
  if (sh != nullptr) {
    sh->out().text("ERR bootloader did not take over\n");
  } else {
    ui_end(UiStatus::State::DidNotStart, Fault::Io, 0);
  }
  phase_ = FwPhase::Idle;
  return false;
}

void FwService::poll(shell::Shell& sh) noexcept {
  if (armed_ && armed_sessions_ != host_.host_sessions()) {
    armed_ = false;  // the card went to a computer since `arm`: it may have been rewritten, the token is dead
  }
  if (ui_armed_ && armed_sessions_ != host_.host_sessions()) {
    ui_armed_ = false;  // ... and so is a client check
  }
  ui_expire();  // ... and by the clock, if the screen never asks again (an abandoned check must not keep the HIL injection refused for good)
  if (phase_ == FwPhase::Handover) {
    (void)handover_check(client_ ? nullptr : &sh);
  }
}

void FwService::terminal_closed() noexcept {
  armed_ = false;
  if (client_ || phase_ == FwPhase::Handover) {
    return;  // a client operation does not depend on the terminal; a hand-over is ended by poll()'s clock
  }
  store_.close_read();
  store_.abort_upload();
  if (phase_ != FwPhase::Idle) {
    host_.log("fw: terminal closed, work abandoned");
  }
  phase_ = FwPhase::Idle;
}

// One slice of the file read back from the card through a validator that has seen nothing else (an upload's read-back, or `fw arm`).
bool FwService::verify_slice(shell::Shell* sh) noexcept {
  const bool rechecking = phase_ == FwPhase::Rechecking;
  const bool arming = phase_ == FwPhase::Arming || rechecking;
  std::size_t got = 0;
  const Fault f = store_.read(buf_, sizeof buf_, got);
  if (f != Fault::Ok) {
    fail(sh, f, read_total_);
    return true;
  }
  if (got != 0u) {
    if (!v_.feed(buf_, got)) {
      fail(sh, arming ? v_.fault() : Fault::Readback, v_.fault_offset());
      return true;
    }
    read_total_ += static_cast<std::uint32_t>(got);
    return true;
  }
  store_.close_read();
  const bool valid = v_.finish();
  if (arming) {
    if (!valid) {
      fail(sh, v_.fault(), v_.fault_offset());
      return true;
    }
    // (a client check has no CRC announced to it: the CRC it finds is what it reports, and what ui_install() holds the file to)
    const bool wrong_crc = rechecking ? v_.crc() != armed_crc_ : (!client_ && v_.crc() != crc_announced_);
    if (wrong_crc || (rechecking && read_total_ != armed_size_)) {
      fail(sh, Fault::Crc, read_total_);
      return true;
    }
    if (rechecking) {
      return begin_handover(sh);
    }
    const std::uint32_t now = host_.now_ms();
    if (sh != nullptr) {
      ++counter_;
      std::uint32_t t = host_.entropy() ^ (now * 2654435761u) ^ (counter_ * 0x9E3779B9u);
      t ^= t >> 15;
      t *= 0x2C1B3C6Du;
      t ^= t >> 12;
      token_ = t != 0u ? t : 1u;
      armed_ = true;
    } else {
      ui_armed_ = true;  // no token: only ui_install() can use it
    }
    armed_at_ = now;
    armed_size_ = read_size_;
    armed_crc_ = v_.crc();  // (the announced one, for the console: the check above found them equal)
    armed_sessions_ = host_.host_sessions();
    copy_name(armed_name_, name_);
    const WriteState w = store_.state();
    std::memcpy(armed_serial_, w.serial, sizeof armed_serial_);
    last_ = FwLast{};
    last_.kind = FwLast::Kind::Arm;
    last_.bytes = read_total_;
    last_.crc = v_.crc();
    last_.image = v_.image_length();
    copy_name(last_.name, name_);
    if (sh != nullptr) {
      sh->out().text("armed ");
      hex8(sh->out(), token_);
      sh->out().nl();
    } else {
      ui_end(UiStatus::State::Checked, Fault::Ok, 0);
      ui_.image = v_.image_length();
      ui_.crc = v_.crc();
    }
    host_.log("fw arm: image validated, armed");
    phase_ = FwPhase::Idle;
    return false;
  }
  if (!valid || v_.crc() != crc_seen_ || v_.image_length() != image_len_ || read_total_ != declared_) {
    fail(sh, Fault::Readback, read_total_);
    return true;
  }
  const Fault renamed = store_.commit_upload();
  if (renamed != Fault::Ok) {
    fail(sh, renamed, read_total_);
    return true;
  }
  finish_ok(*sh);  // (an upload's read-back: a console operation, there is a shell)
  return false;
}

void FwService::finish_ok(shell::Shell& sh) noexcept {
  last_ = FwLast{};
  last_.kind = FwLast::Kind::Put;
  last_.bytes = declared_;
  last_.crc = crc_seen_;
  last_.image = image_len_;
  copy_name(last_.name, name_);
  shell::Output& out = sh.out();
  out.text("ok ").text(name_).chr(' ').dec(declared_).chr(' ');
  hex8(out, crc_seen_);
  out.text(" image ").dec(image_len_).nl();
  print_first_ptf(sh);
  host_.log("fw put: ok, image stored");
  phase_ = FwPhase::Idle;
}

// ---- fw arm / fw update -----------------------------------------------------------------------------------------------------------
void FwService::arm(shell::Shell& sh, const shell::Args& a) noexcept {
  std::uint32_t crc = 0;
  if (!valid_image_name(a.argv[2]) || !parse_hex(a.argv[3], crc)) {
    sh.out().text("usage: fw arm <name> <crc32hex>  (name: <stem>.ptf)\n");
    return;
  }
  if (!update_name_ok(a.argv[2])) {
    err(sh.out(), Fault::NameLong);  // (the bootloader would flash another file: OS-BUG-FWNAMELEN)
    return;
  }
  const Fault f = begin_check(a.argv[2]);
  if (f != Fault::Ok) {
    err(sh.out(), f);
    return;
  }
  crc_announced_ = crc;
  sh.start_stream(&stream_thunk, this, false);
}

// The start of a check, `fw arm`'s and the client's: the card free and writable (the update would write), the file readable. Arming on Ok.
Fault FwService::begin_check(const char* name) noexcept {
  if (!update_name_ok(name)) {
    return valid_image_name(name) ? Fault::NameLong : Fault::BadName;  // the one door of every arming: never a name the bootloader mishandles (OS-BUG-FWNAMELEN)
  }
  if (host_.storage_busy()) {
    return Fault::CardBusy;
  }
  const WriteState w = store_.state();
  if (!w.enabled) {
    return w.why;
  }
  const Fault f = store_.open_read(name, read_size_);
  if (f != Fault::Ok) {
    return f;
  }
  v_.reset();
  read_total_ = 0;
  declared_ = read_size_;
  received_ = 0;
  copy_name(name_, name);
  phase_ = FwPhase::Arming;
  return Fault::Ok;
}

void FwService::update(shell::Shell& sh, const shell::Args& a) noexcept {
  shell::Output& out = sh.out();
  std::uint32_t token = 0;
  const bool parsed = parse_hex(a.argv[2], token);
  const bool was_armed = armed_ && armed_sessions_ == host_.host_sessions();  // (a hand-over to a computer since `arm` killed it)
  const bool match = parsed && token == token_;
  const bool expired = host_.now_ms() - armed_at_ >= kArmLifetimeMs;
  armed_ = false;  // one use, whatever follows
  if (!parsed) {
    out.text("usage: fw update <token>\n");
    return;
  }
  if (!was_armed || !match) {
    out.text("ERR not armed for that token\n");
    return;
  }
  if (expired) {
    out.text("ERR token expired\n");
    return;
  }
  const Fault f = begin_recheck();
  if (f == Fault::NotArmed) {
    out.text("ERR the card changed since arm\n");
    return;
  }
  if (f != Fault::Ok) {
    err(out, f);
    return;
  }
  sh.start_stream(&stream_thunk, this, false);
}

// The file is read and validated once more, as `arm` did (every check and the CRC), before anything is touched: the token only says it was good then. The card must be
// the one `arm` saw (NotArmed otherwise). Rechecking on Ok. Shared by `fw update` and ui_install().
Fault FwService::begin_recheck() noexcept {
  const WriteState w = store_.state();
  if (!w.enabled) {
    return w.why;
  }
  if (std::strcmp(w.serial, armed_serial_) != 0) {
    return Fault::NotArmed;
  }
  if (host_.storage_busy()) {
    return Fault::CardBusy;
  }
  std::uint32_t size = 0;
  const Fault f = store_.open_read(armed_name_, size);
  if (f != Fault::Ok || size != armed_size_) {
    store_.close_read();
    return f != Fault::Ok ? f : Fault::Readback;
  }
  v_.reset();
  read_total_ = 0;
  copy_name(name_, armed_name_);
  phase_ = FwPhase::Rechecking;
  return Fault::Ok;
}

// The image passed again: from here nothing but the hand-over. True: the wait for the bootloader has begun (the stream goes on).
bool FwService::begin_handover(shell::Shell* sh) noexcept {
  host_.log("update: stopping the transport, muting");
  host_.quiesce();
  (void)store_.remove_fwinfo();  // a stale one
  const Fault f = store_.write_fwinfo(armed_name_);
  last_ = FwLast{};
  last_.kind = FwLast::Kind::Update;
  last_.fault = f;
  copy_name(last_.name, armed_name_);
  if (f != Fault::Ok) {
    if (sh != nullptr) {
      err(sh->out(), f);
    } else {
      ui_end(UiStatus::State::Failed, f, 0);
    }
    host_.log("update: _fwinfo failed, not handing over");
    phase_ = FwPhase::Idle;
    return false;
  }
  if (sh != nullptr) {
    sh->out().text("update: handing over to the bootloader (").text(armed_name_).text(")\n");
    sh->out().flush();
  }
  host_.drain_output(kDrainMs);
  host_.log("update: bootloader request pin low");
  host_.bootloader_request(true);
  handover_at_ = host_.now_ms();
  phase_ = FwPhase::Handover;
  return true;
}

// ---- the client API (OS-FWCONFIG): the same check and hand-over for a screen, without a shell ---------------------------------------
void FwService::ui_end(UiStatus::State state, Fault f, std::uint32_t offset) noexcept {
  const bool installing = ui_.installing;
  ui_ = UiStatus{};
  ui_.state = state;
  ui_.installing = installing;
  ui_.fault = f;
  ui_.offset = offset;
}

namespace {
bool client_free(FwPhase p) noexcept { return p == FwPhase::Idle || p == FwPhase::Done; }
}  // namespace

Fault FwService::ui_list_begin() noexcept {
  if (!client_free(phase_)) {
    return Fault::Busy;
  }
  if (host_.card_on_host()) {
    return Fault::OnHost;
  }
  if (host_.storage_busy()) {
    return Fault::CardBusy;
  }
  list_first_ = true;
  return store_.list_begin();
}

bool FwService::ui_list_next(FwEntry& out, bool& first) noexcept {
  while (store_.list_next(out)) {
    if (is_ptf_name(out.name)) {  // not _o3up.tmp, not _fwinfo, nothing the bootloader's button would not pick up
      first = list_first_;
      list_first_ = false;
      return true;
    }
  }
  return false;
}

void FwService::ui_list_end() noexcept { store_.list_end(); }

Fault FwService::ui_check_begin(const char* name) noexcept {
  if (!client_free(phase_)) {
    return Fault::Busy;
  }
  // Everything that can refuse is tried before anything is dropped: a refused call leaves a console token and a passed check that waits for ui_install() as they were.
  if (!valid_image_name(name)) {
    return Fault::BadName;
  }
  if (!update_name_ok(name)) {
    return Fault::NameLong;  // (before anything is dropped, like the other refusals)
  }
  if (host_.card_on_host()) {
    return Fault::OnHost;
  }
  const Fault f = begin_check(name);
  if (f != Fault::Ok) {
    return f;
  }
  armed_ = false;  // the screen takes the service: a console token is dropped, as any other `fw` command would, and so is an earlier check
  ui_armed_ = false;
  client_ = true;
  ui_ = UiStatus{};
  ui_.state = UiStatus::State::Running;
  return Fault::Ok;
}

// One owner: a re-check or the hand-over that is running (or a console job) is answered Busy and NOTHING is touched, neither ui_ (the screen keeps reporting the job, and its
// "do not power off") nor the check that is waiting. After poll() has lapsed a check by the clock the answer is NotArmed, not Expired.
Fault FwService::ui_install() noexcept {
  if (phase_ != FwPhase::Idle) {
    return Fault::Busy;
  }
  const bool armed = ui_armed_ && armed_sessions_ == host_.host_sessions();  // (a hand-over to a computer since the check killed it)
  const bool expired = host_.now_ms() - armed_at_ >= kArmLifetimeMs;
  ui_armed_ = false;  // one use, whatever follows (ui_slice() then reports a check that was not used as lapsed)
  Fault f = Fault::Ok;
  if (!client_ || !armed) {
    f = Fault::NotArmed;
  } else if (expired) {
    f = Fault::Expired;
  } else if (host_.card_on_host()) {
    f = Fault::OnHost;
  } else {
    f = begin_recheck();
  }
  if (f != Fault::Ok) {
    return f;  // refused: the answer is the return value
  }
  ui_.installing = true;
  ui_end(UiStatus::State::Running, Fault::Ok, 0);
  return Fault::Ok;
}

// A passed check lapses kArmLifetimeMs after it was made, whether or not the screen still asks (ui_slice(), poll()).
void FwService::ui_expire() noexcept {
  if (ui_armed_ && host_.now_ms() - armed_at_ >= kArmLifetimeMs) {
    ui_armed_ = false;
    if (ui_.state == UiStatus::State::Checked) {
      ui_end(UiStatus::State::Failed, Fault::Expired, 0);
    }
  }
}

UiStatus FwService::ui_slice() noexcept {
  if (client_) {
    if (phase_ == FwPhase::Arming || phase_ == FwPhase::Rechecking) {
      (void)verify_slice(nullptr);
    } else if (phase_ == FwPhase::Handover) {
      (void)handover_check(nullptr);
    }
  }
  if (ui_.state == UiStatus::State::Checked) {  // a passed check waits for ui_install(): it lapses
    if (ui_armed_ && armed_sessions_ != host_.host_sessions()) {
      ui_armed_ = false;
    }
    ui_expire();
    if (ui_.state == UiStatus::State::Checked && !ui_armed_) {
      ui_end(UiStatus::State::Failed, Fault::NotArmed, 0);
    }
  }
  UiStatus s = ui_;
  if (s.state == UiStatus::State::Running) {
    s.total = read_size_;
    s.done = phase_ == FwPhase::Handover ? read_size_ : read_total_;
  }
  return s;
}

void FwService::ui_cancel() noexcept {
  ui_armed_ = false;
  if (client_ && (phase_ == FwPhase::Arming || phase_ == FwPhase::Rechecking)) {
    store_.close_read();
    host_.log("fw: check dropped");
    phase_ = FwPhase::Idle;
  }
  if (!(client_ && phase_ == FwPhase::Handover)) {
    ui_ = UiStatus{};  // (the hand-over cannot be taken back: its outcome stays for ui_slice())
  }
}

}  // namespace o3os::platform::mk66::update
