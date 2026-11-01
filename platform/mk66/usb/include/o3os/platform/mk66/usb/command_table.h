// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The names and one-line help of the `diag` commands (OS-006b), as plain data that the host tests can read: the image's command table
// (src/console.cpp) takes every name and help text from here, by enumerator, and binds only the handler. A command is the action
// `diag.<name>` (usb/diag_shell.h); this list is the whole console. The one command that writes the card or reaches the bootloader is `fw` (OS-006c, owner
// decision in CLAUDE.md "Firmware delivery"): its sub-commands are fixed by update/fw_service.h, and a host test fails if another command name like `update`,
// `flash`, `boot` or `write` is added. `ogexp` (OS-FILES-C C2a) makes files on the card too, but only through the save layer: the card guard's rows for Export/<name> OG (hal/save_guard.h),
// the card gate and the atomic save, one new folder per export, nothing ever replaced or removed; it never reaches the bootloader.

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66::console {

enum class Cmd : unsigned { Help, Version, Stage, Cpu, Mem, Crash, Usb, Log, Input, Mix, Mon, Peek, Poke, Audio, Rot, Sd, Ls, Load, Inst, Mod, Reset, Fw, Radio, Voices, Key, Pad, Enc, State, Shot, Leds, Bench, Cache, Ogexp, Power, Disk, Guard, Ins, Count };

struct CommandInfo {
  const char* name;
  const char* help;  // one line, at most 70 characters
};

inline constexpr CommandInfo kCommandInfo[] = {
    {"help", "list the commands"},
    {"version", "build identity and USB identity"},
    {"stage", "boot/run stage, uptime, why the chip reset"},
    {"cpu", "cpu [reset|perf off]: audio load: cycles per stage, underruns, jitter"},
    {"mem", "memory map, SRAM budgets, free SRAM"},
    {"crash", "the crash record the previous run left"},
    {"usb", "usb [audio|mode|profile|map ..]: counters, AUDIO mode, stem map"},
    {"log", "log [count]: the last lines boot code and drivers wrote"},
    {"input", "input [stats]: live input events (any key stops)"},
    {"mix", "audio meters (dBFS) and limiter gain reduction; MAIN is pre-limiter"},
    {"mon", "mon [off|on|0..100]: line-in monitor level (0 = off, on = 70)"},
    {"peek", "peek <hexaddr> [1..16]: read 32-bit words (clocked modules only)"},
    {"poke", "poke <hexaddr> <hexvalue>: write one word, I2S0 registers only"},
    {"audio", "audio start: retry the audio bring-up after a boot failure"},
    {"rot", "rot <0..7>: set REG_ROTATE now (display orientation, not saved)"},
    {"sd", "sd [mount]: the SD card (slot, mount, identity, speed, counters)"},
    {"ls", "ls [path]: list a directory of the card (read only)"},
    {"load", "load [folder]: read an OG project folder (default Workspace)"},
    {"inst", "inst <1..64>: what instrument n plays (start, len, loop, root, vol)"},
    {"mod", "mod [load|bind|note|off|set|gov|notice ..]: SD and built-in modules"},
    {"reset", "mute the outputs, then restart the unit"},
    {"fw", "fw [put|rm|arm|update ..]: firmware files on the SD card (OS-006c)"},
    {"radio", "radio [retry]: FM radio state, last failure, registers, pins"},
    {"voices", "sampler voices: in use, peak, steals, advanced spans (OS-VOX)"},
    {"key", "key <button> [tap|down|up|hold <ms>]: inject a button press"},
    {"pad", "pad <0..47> [tap|down|up] [vel]: inject a pad press"},
    {"enc", "enc <n> | accel [off|t1 t2 [ms]]: n detents; list acceleration"},
    {"state", "app state as one key=value line (screen, cursor, play, ...)"},
    {"shot", "shot [565|8888 [w]] [pclk0]: the screen as framed base64 (o3console)"},
    {"leds", "leds: pad LED levels as the driver holds them, one line"},
    {"bench", "bench <kernel|all|disk reset> [blocks]: cycles/block (bench image)"},
    {"cache", "cache [on|off|inv|check]: the code cache (LMEM): state, switch, check"},
    {"ogexp", "ogexp [status|cancel]: the project as an OG folder in Export/<name> OG"},
    {"power", "power [press|release|pin|off [dry]]: the Power switch and the shutdown"},
    {"disk", "disk [share [ro]|eject]: give the card to the computer, take it back"},
    {"guard", "guard [n <1..255>]: the main-loop guard's window (OS-MAINGUARD)"},
    {"ins", "ins [list|cost|bind t s mod|unbind t s|bypass t s on|off] (OS-INSFX)"},
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
