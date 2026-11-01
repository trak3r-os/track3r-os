// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The firmware update port of the App (hal/firmware.h, OS-FWCONFIG) over FwService's client API: a translation and nothing else. The check, the re-check, the hand-over and its
// wait are FwService's (the same code `fw arm` and `fw update` run); this class turns a Fault into the port's Refusal, lists Firmware/ in the port's shape and hands the
// status over with a static text for the fault. It holds no state of its own. Pure, so the host tests and QEMU run it with a fake store; the image creates it in
// fw_host_mk66.cpp (device::sink()). Main loop only.

#include <o3os/hal/firmware.h>
#include <o3os/platform/mk66/update/fw_service.h>

namespace o3os::platform::mk66::update {

static_assert(hal::firmware::kNameCap == kImageNameMax + 1, "the port's name buffer is the writer's longest image name and a NUL");
static_assert(hal::firmware::kCheckLifetimeMs == kArmLifetimeMs, "the port's lifetime of a check is the console's token lifetime");

// The reason a call was refused, in the port's words.
hal::firmware::Refusal refusal_of(Fault f) noexcept;

class FwServiceSink final : public hal::firmware::FirmwareSink {
 public:
  explicit FwServiceSink(FwService& service) noexcept : svc_(service) {}

  bool available() const noexcept override { return true; }  // the image always has the path; the card decides, per call
  hal::firmware::Refusal list_begin() noexcept override;
  bool list_next(hal::firmware::File& out) noexcept override;
  void list_end() noexcept override { svc_.ui_list_end(); }
  hal::firmware::Refusal begin_check(const char* name) noexcept override { return refusal_of(svc_.ui_check_begin(name)); }
  hal::firmware::CheckStatus poll_check() noexcept override;
  void cancel() noexcept override { svc_.ui_cancel(); }
  hal::firmware::Refusal install() noexcept override { return refusal_of(svc_.ui_install()); }
  hal::firmware::Refusal pad() noexcept override { return refusal_of(svc_.ui_pad()); }

 private:
  FwService& svc_;
};

}  // namespace o3os::platform::mk66::update
