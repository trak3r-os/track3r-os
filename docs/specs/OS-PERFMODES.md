# OS-PERFMODES — Perform custom play modes C1 to C20

Status: specified 2026-11-05, decisions taken the same day (Q1 a, Q2 a, Q3 a), not implemented · Level C (the sequencer's step order in the render; a minor bump of the `PERF` and `SCEN` chunks) · Backlog row "Perform custom play modes", approved by the maintainer 2026-11-05 (IDEA triage; the ID OS-PERFMODES is given here) · Facts from `dev` at c1dc3125 and from `polyend/tracker-ce` at 7fd6c2c (cited `tracker-ce@7fd6c2c:<path>:<line>`; a read-only clone, `build-ce/tracker-ce`, untracked) · The orders, the way a track walks them and their rolls are **verified parity** from tracker-ce; what this OS adds around them (tracks whose length is not the pattern's, the first step of a Play from the middle, the encoder order of the values) is **new Community OS behaviour**.

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a, Q3 a (every recommended default). Q1 a approves tracker-ce's custom play orders (`core/source/playModeDefs.h`) as an MIT component (data); its notice goes into `THIRD_PARTY.md` with slice M1.**

Only what the maintainer must decide. Every technical detail is decided here and listed as an assumption (section 5).

| # | Question | Options | Recommended default | Why |
|---|---|---|---|---|
| Q1 | How the 20 orders enter the repository | a) as an approved MIT component (data): `THIRD_PARTY.md` gets tracker-ce's copyright notice for "the custom play orders of `core/source/playModeDefs.h`"; the encoding, the generator and every line of code are ours · b) as facts only: cited in the source and here, no notice | a | The 2,560 numbers are reproduced one to one, which MIT calls a "substantial portion"; the hard rule takes a tracker-ce component only with the maintainer's approval and its notice in `THIRD_PARTY.md`. The row's "taken as facts" may already mean b: the maintainer says which. The notice is one paragraph |
| Q2 | The file of a project that stores C1..C20 | a) `PERF` and `SCEN` 1.2, written only when a PLAYMODE slot stores a value 4..23 (the pattern of decision D5, 2026-10-19, for ids 20 and 21); an older build reads it as a newer minor, says "newer fields skipped" and plays RND there · b) no bump: the values fit the field (-100..100); an older build plays RND without a word | a | A value that gains a meaning is a change of meaning (hard rule: version bump, backward-compatible loader, round-trip test); a is what the maintainer chose for D5; a project that never uses C1..C20 stays byte for byte the file it was |
| Q3 | The rolls the orders carry | a) as the official firmware: 11 of the 20 orders put a Roll into the step's first free FX column, some with a probability of 30 or 60 % (F7, F8) · b) the step orders only | a | Parity; the Roll FX exists with the same value encoding (F15), so it is a lookup and an insert; without them C3, C4, C9, C10, C13, C14 and C15 are plain forward 32-step loops |

## 1. Goal

The Perform effect PLAYMODE gains the official firmware's twenty custom step orders, C1 to C20, after today's FWD, BACK, PING and RND. A track in a custom mode plays its pattern's steps in the order's sequence (repeats, jumps, stutters of a step, half-speed walks) and, for eleven orders, adds rolls on given steps. Everything stays live: a slot's row turns the order on for the affected tracks, row 0 or another row ends it, as the other modes do today.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | The official play modes: FORWARD 0, BACKWARD 1, RANDOM 2, CUSTOM_1..CUSTOM_20 3..22, PINGPONG 23 | `tracker-ce@7fd6c2c:core/source/mtSequencer.h:80-108` | verified (tracker-ce) |
| F2 | The Perform effect "Pattern play mode" offers 0..22, labelled Fwd, Back, Rnd, C1..C20 (ping-pong is not offered in Perform) | `tracker-ce@7fd6c2c:core/interface/performanceMode/performanceFx.cpp:781-802` | verified |
| F3 | The orders are 20 tables (labelled CS1..CS20) of 128 entries `{step to play 1..128, FX option 0..15}`; `playOrdersTotal` 20, `playOrderWithFxLength` 128. Entries of C1 at lines 43-170, C2 175-302, C3 307-434, C4 439-566, C5 571-698, C6 703-830, C7 835-962, C8 967-1094, C9 1099-1226, C10 1231-1358, C11 1363-1490, C12 1494-1621, C13 1625-1752, C14 1756-1883, C15 1887-2014, C16 2018-2145, C17 2149-2276, C18 2280-2407, C19 2411-2538, C20 2542-2669 | `tracker-ce@7fd6c2c:core/source/playModeDefs.h:6-10` (entry), `:12-36` (options), `:37-40` (sizes), `:40-2671` (tables) | verified |
| F4 | How a track walks an order: every step it counts its own position `actual_pos_absolute` up by one and wraps it to 0 past the last step index of its length; that length is the pattern's, or the Perform PAT LEN when one is set. The entry is `table[mode][position % 128]`; the step played is `(entry.step - 1) % length`; an entry 0 would play step 1 (none exists) | `tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:1724-1733` (length, PAT LEN), `:1837-1873` (the custom case) | verified |
| F5 | Every mode counts `actual_pos_absolute` the same way (forward 1757, backward 1787, random 1817), so a switch into a custom mode in mid-pattern joins the order at the track's place in its loop, not at entry 1 | `tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:1738-1834` | verified |
| F6 | The wrap of that count is the track's loop end in a custom mode: a queued per-track pattern lands there and the track's FX are cancelled | `tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:1843-1857` | verified |
| F7 | The 15 FX options are rolls: flat or volume-down; 1/4, 1/2 or 1/1 of a step; 30, 60 or 100 %. One draw per step, `random(0, 100) < P` (uniform in 0..99) | `playModeDefs.h:12-36`; `mtSequencer.cpp:1928-2128`; `core/source/util_math.cpp:38-51` | verified |
| F8 | A drawn roll goes into the step's first free FX column (column 1, else column 2; none when both hold an FX) as the Roll FX, value = period + 9 x direction (period 1/1 = 1, 1/2 = 2, 1/4 = 4; direction flat 0, volume-down 1) | `mtSequencer.cpp:281-300`; `mtSequencer.h:188-201`, `:225-233`; `mtSequencer.cpp:3220-3223` | verified |
| F9 | A roll on a step without a note repeats the track's last note | `mtSequencer.cpp:722-746` | verified |
| F10 | At Play every track starts on step `fromPos` (not the order's entry) and the roll of entry `fromPos` is drawn | `mtSequencer.cpp:1303-1340` | verified |
| F11 | Two quirks: option 2 (flat 1/4, 60 %) keeps the previous roll on a miss (its `else` is missing); no order uses options 1..3, so it cannot happen. Orders 17 and 18 are identical | `mtSequencer.cpp:1950-1957`; the table (computed) | verified |
| F12 | What the orders are (computed from F3; CRC-32 over the 256 bytes `step, option` of each order): see the table below | `playModeDefs.h` | verified (computed) |
| F13 | Ours: PLAYMODE 0..3 = FWD, BACK, PING, RND = `TrackPlayMode` 0..3; the bank sends it as `PerfTrack`, `c = mode << 8`, and the sequencer drains `(c >> 8) & 0x3` | `seq/include/o3os/seq/perform.h:104-108`, `:131`; `seq/src/perform.cpp:68-70`; `seq/src/sequencer.cpp:494`; `seq/include/o3os/seq/sequencer.h:125` | code fact |
| F14 | Ours: `advance_tracks` moves `TrackState::pos` once a step by `perf_mode` and lands an E-PERF-07 queue at `loop_end`; `load_tracks` sets `pos` from the start step; `read_row` reads the row at `pos` (then `fx_sanitize`), `pre_fx` consumes the Row FX (Roll); draws use the seeded `rng_` (seeded at configure and at a run start), so an export is deterministic | `seq/src/sequencer.cpp:1359-1424`, `:810-825`, `:1607-1626`, `:1813-1880`, `:89`, `:889` | code fact |
| F15 | Ours: Roll is FX 3, value 0..53 = 9 x direction + period, the same periods (1/1 .. 1/16) and directions (Const 0, VolDown 1, ...) as F8; a Roll on a step without a note repeats at once (`force_roll`) | `seq/include/o3os/seq/fx.h:41`, `:153`, `:252-260`; `sequencer.h:366` | code fact; the encoding is verified parity with F8 |
| F16 | Ours: `PERF` and `SCEN` are 1.1 when a slot holds the FX id 20 or 21, else 1.0 byte for byte; a newer minor is read as far as the reader knows, `newer_minor` set | `docs/formats/o3-project.md` (`PERF` and `SCEN` 1.1, Versions); `formats/src/native_state.cpp:72-93`, `:296-314`; `formats/include/o3os/formats/native.h:80`; `formats/src/native_reader.cpp:53` | code fact |
| F17 | An OG project's performance block is kept verbatim (`OGPM` group 5); no OG value maps to our PLAYMODE, so the value numbering is ours alone | `docs/formats/og.md` section 3 (groups, 5 performance) | code fact |

The twenty orders (F12). "Steps" is the highest step an order names; "period" the number of entries after which the order repeats itself exactly (step and roll); rolls as option (entry numbers 1-based):

| Order | Steps | Period | Rolls | CRC-32 |
|---|---|---|---|---|
| C1 | 1..128 | 128 | none | 215FFAAD |
| C2 | 1..128 | 128 | 6: entry 1 dec 1/2 60 %, 23 flat 1/1 30 %, 42 flat 1/1 100 %, 83 flat 1/1 30 %, 118 flat 1/1 60 %, 127 dec 1/4 100 % | 77F04872 |
| C3 | 1..32 in order | 128 | flat 1/1 30 % on entries 1-29 and 128 | 1EFA15B0 |
| C4 | 1..32 in order | 32 | flat 1/1 100 % on every 8th entry | 77466A79 |
| C5 | 1..8 | 128 | none | B99CDFDF |
| C6 | 1..31 | 32 | none | A852E05D |
| C7 | 1..32 | 32 | entries 14, 15, 28, 29, 30, 31 of every 32 (dec 1/2 100, flat 1/1 100, flat 1/1 100, flat 1/1 100, dec 1/2 30, flat 1/1 60) | ACCAECA6 |
| C8 | 1..32 | 32 | flat 1/1 100 % on entries 25-32 of every 32 | 8E57FB57 |
| C9 | 1..32 in order | 32 | entries 6, 7, 8 of every 8 (flat 1/1 30, flat 1/1 60, dec 1/2 100) | C8D7DF9F |
| C10 | 1..32 in order | 32 | entries 1, 2, 3 of every 8 (dec 1/2 60, dec 1/2 30, flat 1/1 100) | 7E1C050E |
| C11 | 1..16, each four times | 64 | none | 8F11C234 |
| C12 | 1..32, each twice | 64 | none | 1604325A |
| C13 | 1..32 in order | 32 | dec 1/1 30 % on entry 1 of every 8 | B3D6A038 |
| C14 | 1..32 in order | 32 | dec 1/1 30 % on entry 1 of every 4 | BF27215C |
| C15 | 1..32 in order | 32 | dec 1/1 100 % on every entry | 81079E39 |
| C16 | 1..5 | 32 | none | E2847D96 |
| C17 | 1..5 | 32 | none | 053F8CD0 |
| C18 | = C17 | 32 | none | 053F8CD0 |
| C19 | 1..30 | 32 | none | 5150340D |
| C20 | 1..5 | 32 | none | E85DD985 |

All twenty begin with step 1. The whole table, 20 x 128 entries of `step, option` (5,120 bytes in order C1..C20), has CRC-32 **9B2DC0FD**; the steps alone (2,560 bytes) 584852DD. The option codes are the official enum's order: 0 none; 1-3 flat 1/4 at 30, 60, 100 %; 4-6 dec 1/4; 7-9 dec 1/2; 10-12 flat 1/1; 13-15 dec 1/1.

## 3. Design

### 3.1 The orders as data (`seq/play_orders.h`)

- A generator, `tools/gen_play_orders.py --ce <tracker-ce checkout> --out seq/src/play_orders_data.inc`, reads `core/source/playModeDefs.h` of the checkout given by its root (not by the header's path), checks 20 orders of 128 entries and the 16 option names of F3, folds each order to its period (F12: 128, 128, 128, 32, 128, 32, 32, 32, 32, 32, 64, 64, then 32 for C13..C20; 1,088 entries in all) and writes the data file with a header naming the source commit (and the notice of Q1 a). It prints the CRCs of F12 and refuses to write when one differs. CI never runs it (tracker-ce is not in the repository); the data file is committed.
- Encoding: one `std::uint16_t` an entry, bits 0-6 the step minus 1, bits 8-11 the option; an index table of 20 `{offset, period}`. About 2,240 bytes of flash, `.rodata` (not `.ramfunc`).
- API (pure, `noexcept`, no allocation): `kPlayOrders = 20`, `kPlayOrderLen = 128`; `PlayOrderEntry play_order_entry(unsigned order, unsigned index)` (order 0..19, anything else C1; index any, taken `% 128`, then within the order's period); `OrderRoll order_roll(unsigned option)` = `{roll value, percent}`: 0 `{0, 0}`; 1-3 `{4, 30/60/100}`; 4-6 `{13, ...}`; 7-9 `{11, ...}`; 10-12 `{1, ...}`; 13-15 `{10, ...}` (F8 with F15: period + 9 x direction).

### 3.2 The sequencer plays them

- `TrackPlayMode` keeps 0..3 and gains `Custom1 = 4` .. `Custom20 = 23` (helpers `is_custom(m)`, `custom_order(m)` = value - 4). The drain of `PerfTrack` takes `(c >> 8) & 0x1F`; a value above 23 is Forward.
- `TrackState` gains `ord` (0..127): the track's step count since its loop start, kept in **every** mode (F5): `advance_tracks` adds one and wraps it at the track's length; `load_tracks` sets it to the start step modulo the length (as `pos`); a landed E-PERF-07 queue sets it to 0; `read_row`'s shrink rule (a track that got shorter) takes it to 0 with `pos`.
- The custom case in `advance_tracks`: `span = max(1, min(len, effective_len()))` (the track's length, or the Perform PAT LEN when shorter: F4; the same span PING uses today); `++ord`, at `ord >= span` it is 0 and that is the track's `loop_end` (F6: an E-PERF-07 queue lands there); then `pos = (entry(order, ord).step - 1) % span`. `load_tracks` sets `pos` the same way from `ord`, so the first step of a run is the order's (all orders start with step 1: parity at a start on step 1; A-3).
- The rolls (Q3 a): in `read_row`, after `fx_sanitize`, for a custom track whose entry `(order, ord)` has an option: 100 % without a draw, 30 or 60 % with `rng_.range(0, 100) < P`; on success the Roll FX (3) with the option's value goes into the first column whose type is `kFxNone` (column 1, else column 2, else nothing: F8). `pre_fx` then treats it as a Roll the row holds, a step without a note included (F9, F15). The row's copy is changed, never the pattern.
- The pattern's natural end restarts a custom track as it restarts FWD (`load_tracks(0, natural)`: `ord` 0, the order's first entry). The Random clause of the E-PERF-07 rule at `sequencer.cpp:1517` (a Random track lands its queue at the pattern's end) is not extended to custom tracks: their own loop end comes at the latest with the pattern's end, since `span` is at most `effective_len()`.
- Cost: one flash read, a modulo and, on 30 / 60 % entries, one draw, once a step per custom track: under 60 cycles a step for twelve tracks, about 0.01 % of a block at 120 BPM [est]; nothing a microstep. The rolls cost what the Roll FX costs (voices retriggered), the same as a pattern written with those rolls.

### 3.3 The Perform side

- PLAYMODE (`kPerfFxInfo` row 18) gets the range 0..23 and 24 labels: FWD, BACK, PING, RND, C1..C20 (at most 7 characters: the value text fits). `perf_command` is unchanged (the value goes in `c << 8`). Defaults stay (`model::kPerfDefaultValues` row 18: 1, 2, 3).
- The encoder walks 0..23 and stops at both ends, as every range; EDIT stores the value in the held row, scenes keep it (they hold the slots).

### 3.4 The format (Q2 a)

- `PERF` and `SCEN` **1.2** (`kStateVersionPlayOrder = 0x0102`): the layout and the length of 1.0 (99 and 295 bytes). The writer writes 1.2 only when a slot (for `SCEN`, a slot of a stored scene) holds PLAYMODE (id 18) with a value 4..23 in any of its three rows; else 1.1 when it holds id 20 or 21; else 1.0, byte for byte as today. 1.2 allows ids 0..21 as 1.1 does.
- The reader knows PERF and SCEN up to minor 2. In a 1.0 or 1.1 chunk a PLAYMODE value above 3 reads as 3 (RND: what every build before this one played for it, since the bank held it to 0..3); in 1.2 the value is taken as it is (the bank holds it to 0..23). Nothing is refused that was read before.
- An older build (minor 1 known) meets a 1.2 chunk as a newer minor: same length, read whole, `newer_minor` ("newer fields skipped"), the bank holds the value to 3 (RND); its next save writes 1.1 or 1.0 with RND. Accepted: a downgrade through a save loses the order, nothing else.
- `tools/o3project.py` and `tests/o3project_roundtrip.py` follow (decode 1.2; the pinned fixtures keep their size and CRC-32).

### 3.5 Real time and budgets

Render path (M2): no allocation, no SD, bounded (one lookup a track a step), float-free, the table in flash. Flash about +3.1 KiB in all (2.24 KiB data, 0.9 KiB code and labels) [est]; SRAM +16 B (`ord`, may vanish in `TrackState` padding) [est]; `.ramfunc_l` 0 B (the sequencer is flash code).

## 4. Slices

| # | Slice | Size | Level | Flash [est] | Depends on |
|---|---|---|---|---|---|
| M1 | The orders as data: generator, table, accessor, pinned test | S | B | +2.3 KiB | — |
| M2 | The sequencer plays C1..C20 and their rolls (reachable by command only) | S | C | +0.5 KiB, SRAM +16 B | M1 |
| M3 | `PERF` / `SCEN` 1.2 (writer condition, reader, Python reference, round trips) | S | C | +0.2 KiB | — |
| M4 | PLAYMODE 0..23 on the Perform screen: labels, docs, QA, manual, sim | S | B | +0.3 KiB | M2, M3 merged |

**M1 — the orders as data** (S, Level B; nothing calls it yet). Files: `tools/gen_play_orders.py` (new), `seq/include/o3os/seq/play_orders.h`, `seq/src/play_orders.cpp`, `seq/src/play_orders_data.inc` (generated, committed), the seq library's CMake list, `tests/seq_play_orders_test.cpp` (new), `THIRD_PARTY.md` (Q1 a). Acceptance: the expansion of every order over 128 indices, as `step, option` bytes C1..C20, has CRC-32 9B2DC0FD and each order its CRC of F12; every order begins with step 1; C17 equals C18; spot values typed from this spec (C1 entries 1-16: 1 2 3 4 1 2 3 4 9 10 11 12 1 1 15 16; C5 entries 1-16: 1 2 3 4 5 6 7 8 8 7 6 5 4 3 2 1; C11 entries 1-8: 1 1 1 1 2 2 2 2; C16 entries 1-16: 1 2 3 1 2 3 5 2 2 1 2 4 1 2 5 2; C19 entries 1-8: 1 2 30 1 2 30 1 2; C2's six rolls of F12); `order_roll` for all 16 options as 3.1; an index of 128 + i equals i; an order past 19 reads C1. QA: none (no behaviour). Manual: none.

**M2 — the sequencer plays them** (S, Level C: the render). Files: `seq/include/o3os/seq/sequencer.h` (`TrackPlayMode` 4..23, helpers, `TrackState::ord`), `seq/src/sequencer.cpp` (the drain mask, `advance_tracks`, `load_tracks`, `land_track_queue`, `read_row`), `tests/seq_playorder_test.cpp` (new), `tests/seq_perform_test.cpp:62` (the mask in the test's sink: `0x1F`). Acceptance: 3.2. Host tests (offline `Sequencer`, a 128-step pattern whose row r holds note r + 1 on the track, so the note names the step): every order C1..C20 step by step for 256 steps against `play_order_entry` (two passes: the wrap); the same on tracks of 64, 32, 16 and 7 steps (step = entry modulo the length); PAT LEN 8 keeps the order inside 8 steps; a switch FWD -> C5 on step 11 plays entry 12 next (F5), C5 -> BACK and PING and RND unchanged from today (the existing tests stay green); the rolls: C4 on a 32-step pattern rolls on steps 8, 16, 24, 32 at 1/1 flat (the repeats counted), C15 rolls volume-down on every step, a step whose two columns hold FX gets no roll, a step without a note repeats the last note, C3's 30 % over 4,096 steps lands within 30 +- 3 % with the seeded RNG; an E-PERF-07 queue on a C5 track lands at the order's wrap; two offline renders of a song with C3 on four tracks are byte-identical. QA rows: QA-PRF-210, 211, 212, 214 (host and sim columns; unit column "pending"). Unit check: by ear on a 32-step drum pattern, C1..C20 one after the other on two tracks; `diag cpu` with C15 on twelve tracks against FWD (expected: the cost of the rolls only).

**M3 — `PERF` and `SCEN` 1.2** (S, Level C: persistent format). Files: `formats/include/o3os/formats/native.h` (`kStateVersionPlayOrder`), `formats/src/native_state.cpp` (`uses_play_order`, the version choice, the 1.0 / 1.1 rule of 3.4), `formats/src/native_reader.cpp:53` (PERF / SCEN minor 2 known), `model/include/o3os/model/project_state.h` (`kPerfPlayModeMaxV11 = 3`, `kPerfPlayModeMax = 23`), `tools/o3project.py`, `tests/o3project_roundtrip.py`, `tests/perf_playorder_format_test.cpp` (new), `docs/formats/o3-project.md` (`PERF`, `SCEN`, Versions, the limits table). Acceptance: round trips of a bank without C (1.0, the exact bytes of today), with id 20 (1.1), with C7 in slot 3 row 2 (1.2), with C7 only in a stored scene (`SCEN` 1.2, `PERF` 1.0); a 1.1 chunk holding PLAYMODE 8 reads as 3; a 1.2 chunk with 24 is held to 23 by the bank; the version byte is the lowest that holds the data; the Python reference agrees on every case; the pinned fixtures of `o3project_roundtrip.py` keep their sizes and CRC-32. QA row: QA-PRF-213 (an older build: "newer fields skipped", RND; a manual step). Manual: none (M4).

**M4 — PLAYMODE C1..C20 on the screen** (S, Level B). Files: `seq/include/o3os/seq/perform.h` (row 18 range 0..23, `kPerfLabelsPlayMode[24]`, the header comment), `tests/seq_perform_test.cpp` (the encoder walks to C20 and stops; `PerfTrack` carries 4..23; a scene with C7 recalls it; `perf_value_text` gives `C1`..`C20`), `sim/scenarios/qa/qa_perform.txt` and its goldens (a PLAYMODE slot in EDIT turned to C1 and to C20), `docs/qa/catalogue/perform.md` (QA-PRF-210..215), `docs/manual/perform.md` (the effects table row and a task "Shuffle a track's steps"), `docs/specs/OS-PERFFX.md` section 9 (a note under row 18), `docs/ui/official-manual-map.md` (rows "Pattern play mode with C1 to C20": same, verified parity), the manual's LLM files (`tools/manual_llms.py`). Acceptance: the slot reads C1..C20; a pad on that row plays the order on the affected tracks; row 0 ends it. QA rows: QA-PRF-215 (an export with C3 twice: byte-identical). Unit check: as M2, from the screen; an export `cmp`.

QA rows (reserved here; the next free numbers at implementation time if taken): QA-PRF-210 a track plays an order; 211 a change of mode in mid-pattern joins the order at the track's place; 212 an order's rolls; 213 a saved C mode reopens, an older build plays RND; 214 PAT LEN keeps an order inside its length; 215 two exports with an order are identical.

## 5. Assumptions (decided here; each local, testable and reversible)

- A-1 Value numbering is ours: 0 FWD, 1 BACK, 2 PING, 3 RND (unchanged), 4..23 C1..C20. The official firmware numbers them 0 Fwd, 1 Back, 2 Rnd, 3..22 C1..C20 (F1, F2); no file maps between the two (F17).
- A-2 A track whose length is not the pattern's (polymeter; and E-PERF-07 tracks on another pattern) walks the order over `min(its length, the Perform length)`, as PING does: new Community OS behaviour (the official firmware has one length per pattern).
- A-3 The first step of a run is the order's entry for the start step (the official firmware plays step `fromPos` itself first, F10). Same at a start on step 1 (every order begins with 1); a Play from the middle differs by that one step: new Community OS behaviour.
- A-4 100 % rolls take no draw; 30 and 60 % take one from the sequencer's seeded RNG at the row's read (the official firmware draws at the previous step's end, F7). Same probabilities; exports stay deterministic.
- A-5 Option 2's quirk (F11) is not reproduced: a miss is no roll. No order uses it.
- A-6 The orders act only through Perform's PLAYMODE (as in the official firmware, where they need the performance mode); no step FX, track setting or song field gets them.
- A-7 The table lives in core flash, not in an SD module: the sequencer needs it in the render and 2.2 KiB does not justify a loader.
- A-8 Nothing changes for FWD, BACK, PING and RND, nor for the step FX and the voices.

## 6. Risks

- R1 **A wrong number in the table**: the pinned CRCs of F12 (computed from tracker-ce for this spec) fail the build's test; the generator refuses a mismatch.
- R2 **RNG sequence changes** for songs that use 30 / 60 % orders together with Random mode, Chance or random FX on other tracks: only for songs that use an order (none exist yet); determinism holds.
- R3 **Format**: M3 before M4 so that no build writes 4..23 into a 1.0 / 1.1 chunk; `tools/ci.sh qemu` before merge of M3 (the state chunks' arena).
- R4 **The bank's clamp** is where an older build loses the order; documented in `o3-project.md` like D5.

## 7. Evidence labels

- The twenty orders, the index rule, PAT LEN as the length, the join at the track's place, the loop end, the rolls and their probabilities, the first free column: **verified parity** (F1-F11, tracker-ce@7fd6c2c).
- The value numbering (A-1), polymeter tracks (A-2), the first step of a run (A-3), the draw time (A-4), the file versions: **new Community OS behaviour**.
- That the shipped official firmware 1.9.x plays the same tables as tracker-ce@7fd6c2c: **open research question** (CLAUDE.md counts the tracker-ce citation as evidence; no unit comparison is planned).

## 8. State for implementer

- **M1.** Start with: `playModeDefs.h:6-40` (entry, options, sizes) in the tracker-ce checkout; `seq/include/o3os/seq/fx.h:252-260` (Roll value encoding); an existing pure table and test pair for the style, `seq/include/o3os/seq/perform.h:104-135` and `tests/seq_perform_test.cpp`. Rules: no tracker-ce code or comment is copied, only the numbers; the generator reads the checkout's root (a project hook blocks a script that names a C/C++ header), write the `.inc` with `O3_BULK_EDIT=1` and run `python3 -I .claude/hooks/syntax_check.py` on it; the CRC values in the test are the ones of F12, typed, never computed from the table under test; Q1's notice in `THIRD_PARTY.md` if a.
- **M2.** Start with: `TrackPlayMode` `seq/include/o3os/seq/sequencer.h:125`, `TrackState` `:355-403`; `Sequencer::advance_tracks` `seq/src/sequencer.cpp:1359-1424`; `load_tracks` `:810-825` and `land_track_queue` `:827-835`; `read_row` `:1607-1626` and the Roll in `pre_fx` `:1813-1880`; the E-PERF-07 tests `tests/seq_sequencer_test.cpp:1579-1700` for the offline harness (`note_at`, `trackpat_project`). Rules: render context (no allocation, no SD, bounded, no double); the row's copy only; FWD/BACK/PING/RND bit-identical (the existing sequencer and export tests unchanged); Level C: risk review plus one independent review, `ctest -R unit_seq` while iterating, one `tools/ci.sh host` before merge. Unit check: by ear and `diag cpu` (M2 line above), after M4.
- **M3.** Start with: `uses_fx_after_v10` and the PERF decode `formats/src/native_state.cpp:72-93`, `:296-314`; `kStateVersionFx` `formats/include/o3os/formats/native.h:80`; the reader's minor table `formats/src/native_reader.cpp:53`; `docs/formats/o3-project.md` `PERF` and `SCEN` 1.1. Rules: hard rule on persistent formats (bump, loader, round trip); a bank without C writes the bytes it writes today; `tools/ci.sh qemu` before merge; Level C reviews.
- **M4.** Start with: `kPerfFxInfo` row 18 `seq/include/o3os/seq/perform.h:131`; `perf_value_text` `seq/src/perform.cpp:26`; `sim/scenarios/qa/qa_perform.txt`; the manual's effects table `docs/manual/perform.md:48-66`. Rules: labels at most 7 characters; the manual follows the `manual-writing` skill (lead with what is new); evidence labels in the QA rows. Unit check: QA-PRF-210..215 on an image, export `cmp`.
- Report per slice: flash, SRAM_L, SRAM_U deltas; the scoped `ctest`; for M3 the fixture sizes and CRCs.
