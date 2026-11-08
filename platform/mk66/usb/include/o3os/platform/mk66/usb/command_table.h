// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The names and one-line help of the `diag` commands (OS-006b), as plain data that the host tests can read: the image's command table
// (src/console.cpp) takes every name and help text from here, by enumerator, and binds only the handler. A command is the action
// `diag.<name>` (usb/diag_shell.h); this list is the whole console. The one command that writes the card or reaches the bootloader is `fw` (OS-006c, owner
// decision in CLAUDE.md "Firmware delivery"): its sub-commands are fixed by update/fw_service.h, and a host test fails if another command name like `update`,
// `flash`, `boot` or `write` is added. `ogexp` (OS-FILES-C C2a) makes files on the card too, but only through the save layer: the card guard's rows for Export/<name> OG (hal/save_guard.h),
// the card gate and the atomic save, one new folder per export, nothing ever replaced or removed; it never reaches the bootloader.

#include <o3os/platform/mk66/console_text_ids.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66::console {

enum class Cmd : unsigned { Help, Version, Stage, Cpu, Mem, Crash, Usb, Log, Input, Mix, Mon, Peek, Poke, Audio, Rot, Sd, Ls, Load, Inst, Mod, Reset, Fw, Radio, Voices, Key, Pad, Enc, State, Shot, Leds, Bench, Cache, Ogexp, Power, Disk, Guard, Ins, Status, Trans, Mtc, Ptfx, Scratch, Count };

struct CommandInfo {
  const char* name;
  std::uint16_t help;  // the id of the one-line help (txt::help_<name>, text/console_text.txt; text(help) is the text), at most 70 characters
};

inline constexpr CommandInfo kCommandInfo[] = {
    {"help", txt::help_help},
    {"version", txt::help_version},
    {"stage", txt::help_stage},
    {"cpu", txt::help_cpu},
    {"mem", txt::help_mem},
    {"crash", txt::help_crash},
    {"usb", txt::help_usb},
    {"log", txt::help_log},
    {"input", txt::help_input},
    {"mix", txt::help_mix},
    {"mon", txt::help_mon},
    {"peek", txt::help_peek},
    {"poke", txt::help_poke},
    {"audio", txt::help_audio},
    {"rot", txt::help_rot},
    {"sd", txt::help_sd},
    {"ls", txt::help_ls},
    {"load", txt::help_load},
    {"inst", txt::help_inst},
    {"mod", txt::help_mod},
    {"reset", txt::help_reset},
    {"fw", txt::help_fw},
    {"radio", txt::help_radio},
    {"voices", txt::help_voices},
    {"key", txt::help_key},
    {"pad", txt::help_pad},
    {"enc", txt::help_enc},
    {"state", txt::help_state},
    {"shot", txt::help_shot},
    {"leds", txt::help_leds},
    {"bench", txt::help_bench},
    {"cache", txt::help_cache},
    {"ogexp", txt::help_ogexp},
    {"power", txt::help_power},
    {"disk", txt::help_disk},
    {"guard", txt::help_guard},
    {"ins", txt::help_ins},
    {"status", txt::help_status},
    {"trans", txt::help_trans},
    {"mtc", txt::help_mtc},
    {"ptfx", txt::help_ptfx},
    {"scratch", txt::help_scratch},
};
static_assert(sizeof(kCommandInfo) / sizeof(kCommandInfo[0]) == static_cast<std::size_t>(Cmd::Count), "one row per Cmd");

constexpr const CommandInfo& info(Cmd c) noexcept { return kCommandInfo[static_cast<unsigned>(c)]; }

// OS-POWER P1 (D2): once the shutdown has begun the shell runs only these, the reads that diagnose it (src/console.cpp power_gate); every other command is answered `ERR powering off`. One bit per Cmd.
static_assert(static_cast<unsigned>(Cmd::Count) <= 64u, "kPowerReads has a bit per Cmd");
constexpr std::uint64_t cmd_bit(Cmd c) noexcept { return std::uint64_t{1} << static_cast<unsigned>(c); }
inline constexpr std::uint64_t kPowerReads = cmd_bit(Cmd::Help) | cmd_bit(Cmd::Version) | cmd_bit(Cmd::Stage) | cmd_bit(Cmd::Log) | cmd_bit(Cmd::Crash) | cmd_bit(Cmd::Power) | cmd_bit(Cmd::State) |
                                             cmd_bit(Cmd::Cpu) | cmd_bit(Cmd::Mem);
constexpr bool power_read(Cmd c) noexcept { return (kPowerReads & cmd_bit(c)) != 0u; }

}  // namespace o3os::platform::mk66::console
