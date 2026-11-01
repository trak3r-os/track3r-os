// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/mk66/update/fw_sink.h>

#include <cstring>

namespace o3os::platform::mk66::update {

namespace fwp = o3os::hal::firmware;

fwp::Refusal refusal_of(Fault f) noexcept {
  switch (f) {
    case Fault::Ok: return fwp::Refusal::None;
    case Fault::OnHost: return fwp::Refusal::OnComputer;
    case Fault::CardBusy: return fwp::Refusal::StorageBusy;
    case Fault::Busy: return fwp::Refusal::Busy;
    case Fault::NoCard: return fwp::Refusal::NoCard;
    case Fault::Disabled:
    case Fault::WriteProtected:
    case Fault::Unsupported: return fwp::Refusal::NotWritable;
    case Fault::NoDir: return fwp::Refusal::NoFolder;
    case Fault::NotFound: return fwp::Refusal::NotFound;
    case Fault::BadName: return fwp::Refusal::BadName;
    case Fault::NameLong: return fwp::Refusal::NameTooLong;
    case Fault::NotArmed:
    case Fault::Readback: return fwp::Refusal::NotChecked;  // (a Readback is the file that changed since the check: install() asked for a check that no longer holds)
    case Fault::Expired: return fwp::Refusal::Expired;
    default: return fwp::Refusal::Io;
  }
}

fwp::Refusal FwServiceSink::list_begin() noexcept {
  const fwp::Refusal r = refusal_of(svc_.ui_list_begin());
  if (r != fwp::Refusal::None) {
    svc_.ui_list_end();  // (a listing that did not open is closed all the same: list_end() is safe at any time)
  }
  return r;
}

bool FwServiceSink::list_next(fwp::File& out) noexcept {
  FwEntry e;
  bool first = false;
  if (!svc_.ui_list_next(e, first)) {
    return false;
  }
  std::memcpy(out.name, e.name, sizeof out.name);  // (the same size, both NUL-terminated)
  out.size = e.size;
  out.reset_target = first;
  return true;
}

fwp::CheckStatus FwServiceSink::poll_check() noexcept {
  const UiStatus u = svc_.ui_slice();
  fwp::CheckStatus s;
  switch (u.state) {
    case UiStatus::State::Idle: s.state = fwp::CheckStatus::State::Idle; break;
    case UiStatus::State::Running: s.state = fwp::CheckStatus::State::Running; break;
    case UiStatus::State::Checked: s.state = fwp::CheckStatus::State::Checked; break;
    case UiStatus::State::Failed: s.state = fwp::CheckStatus::State::Failed; break;
    case UiStatus::State::DidNotStart: s.state = fwp::CheckStatus::State::DidNotStart; break;
  }
  s.installing = u.installing;
  s.done = u.done;
  s.total = u.total;
  s.image = u.image;
  s.crc = u.crc;
  s.offset = u.offset;
  s.fault = s.state == fwp::CheckStatus::State::Failed ? fault_name(u.fault) : nullptr;
  return s;
}

}  // namespace o3os::platform::mk66::update
