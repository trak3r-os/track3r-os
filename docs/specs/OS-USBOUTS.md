# OS-USBOUTS: USB outputs 2 / 4 / 6 / 8 / 10, per project

Level C (a project chunk changes, new USB descriptor layouts). New Community OS behaviour. Asked by the maintainer on 2026-10-29.

## Goal

The number of channels the Tracker sends to the computer over USB audio is chosen on the unit, per project: 2, 4, 6, 8 or 10. The stereo USB return (computer to Tracker) is possible when the project sends fewer than 10 channels.

## Decisions (maintainer, 2026-10-29)

| # | Question | Answer |
|---|---|---|
| D1 | Per project or per device | Per project. The profile is already the project's (`USBS` chunk, OS-004d); today only the console `usb profile` changes it |
| D2 | Values | 2, 4, 6, 8, 10. Profile 11 stays console-only (still stored and loaded as today) |
| D3 | Return at 10 channels | Forced off. USB RETURN stays the device setting (Config, field 13); the layout carries the return only when the setting is On **and** the profile has fewer than 10 channels (ADR 0012 amendment 5: "10 OUT / 2 IN" does not open behind a hub with a transaction translator) |
| D4 | The default | Unchanged: a boot is "8 OUT / 2 IN"; a project without `USBS` changes nothing |

Orchestrator's defaults (local, reversible; to confirm by the maintainer):

| # | Default |
|---|---|
| O1 | New profiles `P4` = T1, T2, Master L, Master R and `P6` = T1..T4, Master L, Master R (the factory slots; the user re-maps them on the STEMS page, as today) |
| O2 | The control is the first row of Mixer › STEMS, `OUTPUTS`, edited by the encoder (and Left / Right) as a pending value shown starred, applied by a press-twice key on that page ("USB WILL RECONNECT: PRESS AGAIN"), the pattern of Config's F7 APPLY (OS-USBAUDIO). Leaving the page drops the pending value. The engineer picks the free F-key of the STEMS page and records it here |
| O3 | Config's USB RETURN row, while the project's profile has 10 or more channels, reads `On (10 OUT: off)` or the like, dim; the setting itself is not written (back to 8 channels, the return comes back) |

## Behaviour

- A change of OUTPUTS re-enumerates the port (as a profile change does today: `usb0::request_layout` / `request_mode`), resets the stem map to the new profile's factory slots, and the platform's sync stores the new profile and map in the project (unsaved; not an undo unit, as today).
- In STANDARD (USB AUDIO Off) the profile is only remembered, as today.
- The console's `usb profile` accepts `4` and `6` too.
- Opening a project applies its profile as today (a re-enumeration only when it differs).

## Persistence: `USBS` 1.1

Today's readers (v1.0.x) take `profile` 0..3 strictly: a profile byte 4 would refuse the whole project. So:

- A project whose profile is 2, 8, 10 or 11 is written as `USBS` 1.0, byte for byte as today.
- A project with 4 or 6 channels is written as `USBS` 1.1: the 1.0 part says profile P8 with 8 slots (the real slots first, then the P8 factory slots for the rest), then one appended byte, the real channel count (4 or 6). A 1.0 reader takes it as P8 through its newer-minor rule (known part parsed, the rest skipped, "newer fields skipped"); a 1.1 reader takes the appended byte.
- Loader: 1.0 and 1.1 both read; a hostile 1.1 (count not 4 or 6, short) is refused like any malformed chunk. Round-trip tests for 4 and 6, the 1.0 byte-exact test for 2/8/10/11, and the old-reader view (a 1.1 chunk read as 1.0 gives P8). `docs/formats/o3-project.md` and `tools/o3project.py` follow.

## Design rules from the risk review (2026-10-29, folded)

1. **Wish and presented.** usb0 keeps the return *wish* (the setting, the console's `usb return`) in its own variable and derives `presented = wish && channels < 10`. New `return_wanted()` feeds `UsbReturnSync::target()`, the console and the DiskShare layout (usb0.cpp:355/359, app_main.cpp:916, console.cpp:378/380/388/2659); `return_enabled()` stays the presented value for the descriptors, `graph.set_return_enabled` and the display. So setting On at 10 channels never stores Off, and `usb mode standard` or leaving DISK at 10 channels keep the wish.
2. **Same-layout test** (usb0.cpp:393) compares presented with presented: wish On at 10 channels does not re-enumerate for nothing.
3. **Enum values.** `usb::Profile` appends `P4 = 5` and `P6 = 6` after `P4S = 4`. The model's `UsbState::profile` is the enum; the stored `USBS` byte stays 0..3 (profile 4 stays refused by an explicit 1.0 range check). The writer (native_state.cpp:33, :108) maps 5 / 6 to byte 1 with 8 slots plus the count byte; `payload_size` agrees with `encode`.
4. **Fill order.** Slots 0..n-1 hold the real masks, slots n..7 the P8 factory masks (P8 slots 4..7 for P4, 6..7 for P6). The 1.1 reader requires byte 1, 8 slots and a count of 4 or 6, then zeroes the masks past n before `validate_usb_state`.
5. **Reader.** `known_minor(Usbs)` becomes 1.1 and the size bound `hi` is `kUsbMax + 1` when the minor is >= 1 (native_reader.cpp:65, :79); the count is decoded when `chunk_minor >= 1`, before `shape_ok`. Fact: the v1.0.0 tag has the same newer-minor code, so it loads a 1.1 chunk as P8; a v1.0 build that saves it again drops the 4 / 6 count (accepted: the chunk falls back to 8 channels, the map's first slots kept).
6. **Console.** `usb profile` (console.cpp:394) and `usb_profile_name` (diag_text.cpp:221) learn 4 and 6. Behaviour change: `usb profile 10|11 duplex` now presents no return; the console prints that.
7. **Boot.** `usb_boot_layout` (usb_return.h:107) and `usb0::start` apply the rule.
8. **App to driver.** The app cannot reach usb0: a hook next to `MixBoard::apply_stems` (app_main.cpp:851) and one in the sim; the pending OUTPUTS value stays when `request_layout` returns false (busy or DISK).
9. **STEMS rows.** The OUTPUTS first row shifts every slot row by one (mixer_actions.cpp:101, :250, :266, `stem_rows`).
10. **Tests and docs.** Descriptor goldens, the ADR 0012 amendment 5 bandwidth table, `tools/o3project.py`, `docs/formats/o3-project.md` and the fuzz tests gain the P4 and P6 cases.

Also: `model::kUsbProfileMax`, `kUsbProfileChannels`, `validate_usb_state`, `StemMap::reset` and `supported()` learn the two profiles; `P4S` stays refused and unstored.

## Defaults to confirm (implementation, 2026-10-30)

Choices the implementation made where the decisions above leave room; each is a "default, to confirm" for the maintainer. All are new Community OS behaviour.

| # | Default | Why |
|---|---|---|
| C1 | F6 is the apply key, labelled APPLY, on the STEMS page only (`mixer.apply`). Config's key is F7, but F7 is MUTE on the Mixer | F6 was the one unlabelled key of the Mixer, so no existing function moves |
| C2 | The key is bound in a new field context `Context::MixerStems` = 49 (`mixer.stems`), not on the Mixer screen context | F6 is unlabelled on the other Mixer pages, and the key rules of OS-QANET want a bound key to act or be labelled; bound on STEMS alone it is only a labelled key where it acts. The context comes from `MixerScreen::field_context` |
| C3 | Left / Right edit a value only on the OUTPUTS row; on the slot rows they move the cursor as before | the slot cells are still edited by the encoder, Insert and Delete |
| C4 | `F6` with nothing pending, or with no driver, says `NOTHING TO APPLY`; a refusal (the port is changing mode, DISK) says `USB BUSY: TRY AGAIN` and keeps the pending value | a labelled key always answers |
| C5 | The simulator has no stem map, so no scenario can apply a value: the OUTPUTS row is covered by `mixer_test`, the gallery pictures (`mixer_stems_outputs`, `mixer_stems_worst`) and the unit steps | the simulator's audio board is the engine without the USB stream |
| C6 | Profile 11 (console-only) counts as 10 or more channels for the return rule (D3, O3) and is not offered on the OUTPUTS row | it has more channels than 10 |
| C7 | A change of the wish (USB RETURN setting) survives the layout changes: 10 to 4 and back to 10 gives the return at 4, none at 10, the setting On throughout (`usb0` keeps the wish; `usb::return_presented`) | the setting is the user's, the layout only decides what the descriptors carry |
| C8 | In DISK, the console's `usb return on|off` changes the wish only until the eject: leaving DISK applies the Config USB RETURN setting again (seen on the unit with v1.0.1-usbouts.1, 2026-10-30: `usb return off` at 10 outputs, then `disk eject`, gives the wish On). The Config setting is the one stored value | the settings sync is blocked in DISK, so a console change there is never stored; Config stays the place to change the setting |

## Project load during a change

A project load during a USB change is not abandoned: the load waits for phase 0 (`UsbApply::step`) and then applies the project's USB part. The OUTPUTS request itself refuses on a busy port and keeps the pending value (C4). Corrected after the reviews; there is no open issue here.

## Done when

- Host tests: descriptors for P4 and P6 (return on and off), stem map factory slots, `USBS` 1.0/1.1 round trips and the old-reader view, the return rule (setting On + 10 channels = no return; the setting unchanged), the STEMS OUTPUTS row (pending, apply twice, leave drops it).
- Sim scenario and golden of the STEMS page with the OUTPUTS row; QA rows (`QA-MIX-050..059`, `QA-CFG-250..259`); manual (mixer and config chapters) and ui-map updated.
- `tools/ci.sh` host, qemu and mk66 green; flash and SRAM delta in the commit.

## Hardware-only checks

The unit enumerates each of 2, 4, 6, 8 and 10 channels with the return on and off on Linux (`arecord -l`, a capture and a duplex run); 10 + return setting On presents no return; macOS and Windows if a host is at hand.

## Budget

No per-block cost (the stem tap sums fewer slots for 4 and 6). Flash: descriptor tables and the row, a few hundred bytes expected. SRAM: none expected.
