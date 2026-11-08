// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pure part of the module loader (ADR 0002, OS-012a): what a .tmod file may say, what the loader does with it, and
// the refusal of everything else. No storage, no memory of its own: a header, a few buffers and the places the parts are
// loaded to go in, and a Status comes out. The stepped file reader on top of it is loader.h; the file format is
// sdk/o3module.h.
//
// What is checked, in the order a hostile file meets it:
//   check_header   the 64 bytes alone: magic, size, target, kind, ABI (equal major, minor not above the OS's), the sizes (image
//                  a multiple of 4 up to kMaxImageBytes, hot part within it, state up to kMaxStateBytes, at most kMaxParams
//                  parameters, the descriptor inside the cold part and aligned), every reserved field zero (the module version, ABI 1.2, is any value when the file says
//                  ABI 1.2 or later and a reserved word, zero, before; the capability mask reserved2[0], ABI 1.3, is any combination of O3M_CAPS_ABI_1_3 in an APP that says
//                  ABI 1.3 or later (O3M_CAPS_ABI_1_4 from 1.4, O3M_CAPS_ABI_1_7 from 1.7) and a reserved word, zero, otherwise; the layout tag reserved2[1], ABI 1.11, is the OS's own import
//                  layout tag in a KIND 3 file that says ABI 1.11 or later, and a reserved word, zero, in every other file), and the length of
//                  the whole file: header + image + 4 bytes a relocation, exactly. Kinds: instrument and effect; the app since ABI 1.1 (a 1.0
//                  file that says app is refused); the importer (kind 3) since ABI 1.11 (OS-FLASHDIET I1: a kind 3 file with a minor below 1.11 is an unknown kind, and one whose word 44 is not
//                  the OS's tag is refused LayoutMismatch). check_header() is the header-only check: the app can call it before anything is placed.
//   Relocator      the table's offsets: 4-byte aligned, strictly ascending (so none twice), the word inside the image and not
//                  across the hot/cold border, the address in the word inside the image (one past its end allowed). Nothing is
//                  written before it has passed; a word that fails is left as it was.
//   read_desc      the descriptor after relocation: its own sizes and versions equal the header's, the name and the parameter
//                  array and their strings inside the image and NUL-terminated within their limits, the views' table (declared count equal to the header's, a table exactly when there are views, its view_draw a Thumb address, the others zero or one; an app has at least one view), every function a Thumb
//                  address (odd) inside the image, parameter defaults within their ranges.
//                  ABI 1.8 (descriptor of 80 bytes; struct_size is per minor): pitch is code or zero; the page table (at most 4 pages of 1..8 parameters, the first at 0, ascending, covering the parameters; names of
//                  at most 7 characters) and the label table (INT parameters only, count = max - min + 1 in 2..16, labels of at most 11 characters) lie in the image and obey those limits.
//                  ABI 1.11 (descriptor of 84 bytes): the importer's table (kind 3 only; NULL in every other kind), checked in full: its size, the format list (1..O3M_IMPORTER_FORMATS_MAX entries
//                  inside the image, each extension a string of at most 7 characters) and its five calls are Thumb addresses inside the image. A kind 3 module has no parameters and no views and makes no
//                  sound (render, set_param, note_on and note_off may be NULL, as an app's).
//                  ABI 1.12 (descriptor of 100 bytes; OS-INSFX-ABI2 A1): the fourth tail, `render_st` (Thumb code inside the image, an effect's only), `fx_flags` (O3M_FX_WIDENS only, and it needs render_st), the factory
//                  presets (an instrument's or an effect's with parameters only: at most 32, each name 1..15 printable characters, 1..param_count values, all finite and inside their parameter's range) and the KEY parameter
//                  flag (an effect's INT parameter 0..16 with no labels, at most two a module). An app and an importer carry the four members zero.
//   crc32          of the header (its CRC field as zero), the image and the table, against the header's.
// A module is trusted community code (ADR 0002: open modules, no signing, no sandbox): this is the robustness of the loader
// against a damaged or truncated file and an honest mistake, not a defence against an attacker with a valid file.
//
// Addresses. The loader places the hot part and the cold part in two places that need not be adjacent (SRAM and SDRAM). Each
// place is a pair: where the bytes go now (`mem`, a pointer of this process) and the address the module will see them at
// (`addr`, a 32-bit target address). On the MK66 the two are the same number; the host tests give them different values and
// compare the result with a module linked at those very addresses.

#include <o3module.h>

#include <cstddef>
#include <cstdint>

namespace o3os::module {

static_assert(sizeof(o3m_file_header) == O3M_FILE_HEADER_SIZE, "the file header is 64 bytes");
// The file format is these offsets (o3module.h has the table): a field that moves is a format change.
static_assert(offsetof(o3m_file_header, magic) == 0 && offsetof(o3m_file_header, header_size) == 4 && offsetof(o3m_file_header, target) == 6 &&
                  offsetof(o3m_file_header, kind) == 8 && offsetof(o3m_file_header, abi_major) == 9 && offsetof(o3m_file_header, abi_minor) == 10 &&
                  offsetof(o3m_file_header, flags) == 11 && offsetof(o3m_file_header, hot_size) == 12 && offsetof(o3m_file_header, image_size) == 16 &&
                  offsetof(o3m_file_header, desc_offset) == 20 && offsetof(o3m_file_header, state_bytes) == 24 && offsetof(o3m_file_header, reloc_count) == 28 &&
                  offsetof(o3m_file_header, param_count) == 32 && offsetof(o3m_file_header, view_count) == 34 && offsetof(o3m_file_header, module_version) == 36 &&
                  offsetof(o3m_file_header, reserved2) == 40 && offsetof(o3m_file_header, image_crc) == 48 && offsetof(o3m_file_header, reserved3) == 52,
              "the file header's field offsets");
static_assert(sizeof(float) == 4, "o3m_param is read as 32-bit words");

inline constexpr std::uint32_t kMaxImageBytes = 256u * 1024u;
inline constexpr std::uint32_t kMaxStateBytes = 256u * 1024u;
inline constexpr std::uint32_t kMaxRelocs = kMaxImageBytes / 4u;
inline constexpr std::uint32_t kMaxParams = O3M_PARAMS_MAX;
inline constexpr std::uint32_t kMaxViews = O3M_VIEWS_MAX;
inline constexpr std::uint32_t kMaxModules = 16;  // tests/module_model_test.cpp holds it equal to model::kMaxModules

// The OS's own ABI version: what check_header() compares a file with.
inline constexpr std::uint32_t kOsAbiMajor = O3M_ABI_MAJOR;
inline constexpr std::uint32_t kOsAbiMinor = O3M_ABI_MINOR;

enum class Status : std::uint8_t {
  Ok = 0,
  NotFound,       // no file of that name
  Io,             // the card failed or the file is shorter than its header says
  TooShort,       // fewer than 64 bytes
  BadMagic,
  BadHeaderSize,  // header_size is not 64
  BadTarget,      // not the Cortex-M4F target
  BadKind,        // not an instrument, an effect or (ABI 1.1) an app; importers are reserved
  AbiMajor,       // a different major version
  AbiMinor,       // a newer minor version than the OS
  BadReserved,    // a reserved field or flag is not zero
  BadSize,        // image, hot part, state, parameter count or descriptor position out of range
  BadFileLength,  // the file is not header + image + relocation table
  BadCrc,
  BadRelocOrder,  // not 4-byte aligned, not ascending
  BadRelocOffset, // the word is outside the image or across the hot/cold border
  BadRelocValue,  // the address in the word is outside the image
  BadDescriptor,  // read_desc found something wrong (see Desc check detail)
  LayoutMismatch, // OS-FLASHDIET I1: a kind 3 file whose header word 44 is not this OS's import layout tag (the module was built against another step layout)
  NoMemory,       // the arenas cannot hold the module
  Busy,           // a load is already running
  BadName,        // not a file stem
  Quarantined,    // refused before and not retried until the next boot (failed validation, or crashed the device)
  ViewOverrun,    // OS-012b: a view went over its draw-call, point, text or time budget (module/view.h): quarantined for the boot
  Older,          // OS-012d: a card copy that is not newer (by the header's module version, OS-MODVER) than the built-in module of that stem (module/builtin.h prefers_card): not used, never an error of the load
  CapUnsupported, // OS-H1 (ADR 0014 amendment D, C4): an app asks for a published capability this OS does not grant (midi_out; export_files before ABI 1.7): refused, not run; not a bad file, so not quarantined
};

inline constexpr const char* status_name(Status s) noexcept {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::NotFound: return "not found";
    case Status::Io: return "i/o error or short file";
    case Status::TooShort: return "shorter than a header";
    case Status::BadMagic: return "not a module file";
    case Status::BadHeaderSize: return "bad header size";
    case Status::BadTarget: return "built for another target";
    case Status::BadKind: return "kind not supported";
    case Status::AbiMajor: return "ABI major version differs";
    case Status::AbiMinor: return "ABI minor version is newer than the OS's";
    case Status::BadReserved: return "reserved field not zero";
    case Status::BadSize: return "size out of range";
    case Status::BadFileLength: return "file length does not match the header";
    case Status::BadCrc: return "checksum mismatch";
    case Status::BadRelocOrder: return "relocation table not ascending or unaligned";
    case Status::BadRelocOffset: return "relocation outside the image";
    case Status::BadRelocValue: return "relocated address outside the image";
    case Status::BadDescriptor: return "bad descriptor";
    case Status::LayoutMismatch: return "importer layout mismatch (word 44 is not this OS's tag)";
    case Status::NoMemory: return "no room for the module";
    case Status::Busy: return "a load is running";
    case Status::BadName: return "bad module name";
    case Status::Quarantined: return "quarantined until reboot";
    case Status::ViewOverrun: return "view over its budget";
    case Status::CapUnsupported: return "needs a capability this OS lacks";
    case Status::Older: return "not newer than the built-in module";
  }
  return "?";
}

// A refusal that says the FILE is wrong (not the card, the memory or the caller): the module is quarantined for the rest of this boot.
constexpr bool is_validation_failure(Status s) noexcept {
  return s >= Status::TooShort && s <= Status::LayoutMismatch;
}

// The descriptor as it is in the file, on a 32-bit target: every pointer a 32-bit address. Identical to o3m_module there
// (static_assert in image.cpp), and independent of the host's pointer size where the tests read it.
struct Desc32 {
  std::uint32_t abi_major, abi_minor, kind, struct_size, name, state_bytes, param_count, params;
  std::uint32_t init, render, set_param, note_on, note_off, shutdown;
  std::uint32_t ui;          // the views' table (ABI 1.1: called for modules of 1.1 and up; validated for all); 0 exactly when view_count is 0
  std::uint32_t view_count;  // 0..kMaxViews
};
static_assert(sizeof(Desc32) == 64, "the descriptor is 16 words");

// ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): the four members appended to the descriptor of a module that declares minor 8 or above, 16 bytes after the 64 of Desc32. A module of a lower minor has a
// 64-byte descriptor and these stay zero (they are not read). The page table and the label table are `Page32` / `Labels32` entries inside the image.
inline constexpr std::uint8_t kPitchAbiMinor = 8;  // the minor that made the descriptor 80 bytes
struct DescExt32 {
  std::uint32_t pitch;       // Thumb address of the pitch call, or 0
  std::uint32_t pages;       // address of `page_count` Page32, or 0
  std::uint32_t page_count;  // 0..O3M_PAGES_MAX, zero exactly when `pages` is 0
  std::uint32_t labels;      // address of `param_count` Labels32, or 0
};
static_assert(sizeof(DescExt32) == 16, "the 1.8 tail of the descriptor is 4 words");
struct Page32 {
  std::uint32_t name, first_param;
};
static_assert(sizeof(Page32) == 8, "a page is 2 words");
struct Labels32 {
  std::uint32_t names, count;  // `names`: the address of `count` string addresses
};
static_assert(sizeof(Labels32) == 8, "a label table entry is 2 words");
// ABI 1.11 (OS-FLASHDIET I1): the minor that published the importer kind (kind 3) and the descriptor's last member, `importer` (an address of the
// o3m_importer table inside the image; NULL in every other kind). The file header's word 44 carries the layout tag for kind 3 from this minor on.
inline constexpr std::uint8_t kImporterAbiMinor = 11;
struct DescImp32 {
  std::uint32_t importer;  // address of the importer's table, or 0
};
static_assert(sizeof(DescImp32) == 4, "the 1.11 tail of the descriptor is one word");
// The importer's table as it is in the file (o3m_importer on a 32-bit target): size, formats, format_count, begin, step, read_pcm, report, close.
struct Importer32 {
  std::uint32_t size, formats, format_count, begin, step, read_pcm, report, close;
};
static_assert(sizeof(Importer32) == 32, "the importer's table is eight words");
struct Format32 {
  std::uint32_t ext;  // address of the extension string
};
static_assert(sizeof(Format32) == 4, "a format entry is one word");
// ABI 1.12 (OS-INSFX-ABI2 A1): the minor that published the effect surface and the descriptor's last four members, appended after `importer`: render_st (Thumb address of the stereo render, or 0), fx_flags
// (O3M_FX_*), presets (address of `preset_count` Preset32, or 0) and preset_count. A module of a lower minor has a descriptor that ends before them and they stay zero (they are not read).
inline constexpr std::uint8_t kStereoAbiMinor = 12;
// ABI 1.13 (OS-MODRATE S2): the minor that gave an instrument's fx_flags its O3M_FX_RATE bit (no layout change). Below it only an effect's WIDENS is a known bit.
inline constexpr std::uint8_t kRateAbiMinor = 13;
// The fx_flags bits a module of this kind and minor may set: an effect's O3M_FX_WIDENS, an instrument's O3M_FX_RATE from 1.13, nothing else (every other bit is 0).
constexpr std::uint32_t fx_flags_allowed(std::uint32_t kind, std::uint32_t minor) noexcept {
  return kind == O3M_KIND_EFFECT ? static_cast<std::uint32_t>(O3M_FX_WIDENS) : (kind == O3M_KIND_INSTRUMENT && minor >= kRateAbiMinor ? static_cast<std::uint32_t>(O3M_FX_RATE) : 0u);
}
struct DescFx32 {
  std::uint32_t render_st;    // Thumb address of the stereo render, or 0 (a mono effect, and every other kind)
  std::uint32_t fx_flags;     // O3M_FX_WIDENS (an effect), O3M_FX_RATE (an instrument of 1.13+) or 0
  std::uint32_t presets;      // address of `preset_count` Preset32, or 0
  std::uint32_t preset_count; // 0..O3M_PRESETS_MAX, zero exactly when `presets` is 0
};
static_assert(sizeof(DescFx32) == 16, "the 1.12 tail of the descriptor is 4 words");
struct Preset32 {
  std::uint32_t name;    // address of the preset's name
  std::uint32_t values;  // address of `count` float32
  std::uint32_t count;
};
static_assert(sizeof(Preset32) == 12, "a preset is 3 words");
// The descriptor's size for a module of ABI 1.`minor` (o3module.h O3M_MODULE_SIZE, on the 32-bit target): 64 up to 1.7, 80 from 1.8, 84 from 1.11, 100 from 1.12.
constexpr std::uint32_t desc_size(std::uint32_t minor) noexcept {
  return static_cast<std::uint32_t>(sizeof(Desc32) + (minor >= kPitchAbiMinor ? sizeof(DescExt32) : 0u) + (minor >= kImporterAbiMinor ? sizeof(DescImp32) : 0u) +
                                    (minor >= kStereoAbiMinor ? sizeof(DescFx32) : 0u));
}

// The views' table. ABI 1.9 (OS-KEYROWDLG S5) appends `keys` (Thumb address of the key-row call, or 0): the table is 20 bytes for a module that declares minor 8 or below and 24 from minor 9
// (o3module.h O3M_UI_SIZE on the 32-bit target); the word past byte 20 is read only then.
inline constexpr std::uint8_t kKeysAbiMinor = 9;
struct Ui32 {
  std::uint32_t struct_size, enter, input, view_draw, exit;  // enter, input and exit may be 0
};
static_assert(sizeof(Ui32) == 20, "the views' table is 5 words");
constexpr std::uint32_t ui_size(std::uint32_t minor) noexcept { return minor >= kKeysAbiMinor ? sizeof(Ui32) + sizeof(std::uint32_t) : sizeof(Ui32); }

struct Param32 {
  std::uint32_t name;
  float min, max, def;
  std::uint32_t flags;
};
static_assert(sizeof(Param32) == 20, "a parameter is 5 words");

// The two places a module is loaded to.
struct Placement {
  std::uint8_t* hot_mem = nullptr;
  std::uint32_t hot_addr = 0;
  std::uint8_t* cold_mem = nullptr;
  std::uint32_t cold_addr = 0;
};

// Everything in the header alone, and the file's length. `file_bytes` is the size of the whole file.
Status check_header(const o3m_file_header& h, std::uint32_t file_bytes) noexcept;

// Applies relocation table entries to an image already placed. One Relocator per module; apply() takes the table in pieces, in
// file order. `h` must have passed check_header(); the placement's memory must hold hot_size and (image_size - hot_size) bytes.
class Relocator {
 public:
  Relocator() noexcept = default;
  Relocator(const o3m_file_header& h, const Placement& p) noexcept { begin(h, p); }
  void begin(const o3m_file_header& h, const Placement& p) noexcept {
    h_ = h;
    p_ = p;
    applied_ = last_ = 0;
  }
  Status apply(const std::uint32_t* offsets, std::uint32_t count) noexcept;
  std::uint32_t applied() const noexcept { return applied_; }
  bool complete() const noexcept { return applied_ == h_.reloc_count; }

 private:
  o3m_file_header h_{};
  Placement p_{};
  std::uint32_t applied_ = 0;
  std::uint32_t last_ = 0;  // the last offset applied (the next must be above it)
};

// The memory behind a target address range inside the placed module: nullptr unless [addr, addr + length) lies wholly in the
// hot part or wholly in the cold part.
const std::uint8_t* resolve(const o3m_file_header& h, const Placement& p, std::uint32_t addr, std::uint32_t length) noexcept;

// Reads the descriptor of a placed, relocated module and checks it against the header and the image (see the top). `out` is
// written whatever the verdict (a failed one is not to be used). struct_size must be the size of the declared minor's descriptor (64 up to 1.7, 80 from 1.8, 84 from 1.11, 100 from 1.12). `ext`, when given, gets the 1.8 members
// (zeros for a lower minor) after the same checks: `pitch` is code or 0; the page table, the labels and their strings are inside the image and inside the limits of o3module.h (OS-INSTUI S10). `imp`, when given, gets the
// 1.11 member (zero below that minor): kind 3 requires it to be a valid importer table (see the top), every other kind requires it to be 0. `fx`, when given, gets the four 1.12 members (zeros below that minor)
// after the checks listed at the top (render_st, fx_flags, the presets and the KEY flag).
Status read_desc(const o3m_file_header& h, const Placement& p, Desc32& out, DescExt32* ext = nullptr, DescImp32* imp = nullptr, DescFx32* fx = nullptr) noexcept;

// A parameter of a placed module, checked by read_desc() already; false when `index` is out of range.
bool read_param(const o3m_file_header& h, const Placement& p, const Desc32& d, std::uint32_t index, Param32& out) noexcept;

// CRC-32 (the zlib one): running value in, running value out; start from 0.
std::uint32_t crc32_update(std::uint32_t crc, const void* data, std::size_t length) noexcept;

}  // namespace o3os::module
