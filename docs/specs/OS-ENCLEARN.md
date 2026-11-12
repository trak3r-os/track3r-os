# OS-ENCLEARN — any Learn-able parameter on the encoder

Status: implemented on `feat/OS-ENCLEARN` · Level B · no format change (session only, nothing saved) · Evidence: **new Community OS behaviour**; there is no official-firmware counterpart. The Learn row resolution it reuses is `verified` in this repository's code (`focus_param`, `kParamLearnable`).

## 1. Goal

Move any parameter that MIDI Learn can bind from the front panel, without a MIDI controller and without opening a dialog: pick the row once, then turn the encoder while two keys are held.

## 2. Design

| Part | Rule |
|---|---|
| Assign | `F7` ENC in the Learn overlay (open with `Vol` + `Fx`; context `learn`, OS-UXF-KEYS item 17: the chord `Instr` + `Fx` was removed, new Community OS behaviour). Takes the row under the cursor, resolved as Learn does (`focus_param`, `kParamLearnable`); F7 is always labelled; it closes the confirm state. Refused row: toast `NO TARGET FOR ENCODER`. |
| Target | One in all (`ViewState::enc_target`, 4 B, session only). A different row replaces it. There is no clear gesture: the same row again keeps the target and shows `ENCODER: <PARAM> INS <n>` again. Fixed to that row and instrument. |
| Use | Hold `Instr` + `Vol` and turn the encoder, no dialog open. One step per detent, `Shift` gives the coarse step. Same apply, ranges and undo as a Learn relative binding (`write_moved`). Toast `<PARAM> <value>`; with no target `NO ENCODER TARGET (SET IT IN LEARN)`. |
| Priority | While both keys are held the encoder does nothing else (it overrides Vol-held and Fx-held). A turn cancels the Instr hold popup and the Vol tap/hold. A plain press without a turn does nothing. |
| OS-ZOOM | The `Note` + `Instr` tap is untouched. |
| Gesture | No binding and no modifier: `App::run_once` consumes `Instr` and `Vol` (`gestures_.consume`) when the second of the pair goes down, and again on every turn. The turn is read before the gesture recogniser, so the Learn chord (`Vol` + `Fx`) and the rollover taps are unchanged. App keeps its own `enc_keys_` bits from the raw button events, because a context change (Shift pressed) makes the gesture layer forget a consumed key. |

### Recording

The move enters the recorder a Learn CC move enters (`rec_fx_write`).

- LIVE (`pattern_recording`): "Record knob moves as FX" rules: CUTOFF to 27/28/29 by FILTER TYPE, PANNING 31, DELAY SEND 30, REVERB SEND 32, OVERDRIVE 38, BIT DEPTH 39; same column rule, same strip messages, one value per step.
- STEP (Edit with free recording): the move writes that FX on the cursor's step of the cursor's track. One hold gesture is one undo unit (`enc_take`, closed when a key is released). The parameter itself stays where it was left on undo, as in a LIVE take.
- A parameter with no FX equivalent moves the sound and records nothing; the strip says `REC FX: <PARAM> HAS NO FX` once per gesture.
- WATCH: no recording.

## 3. Files

`app/src/app.cpp` (hook), `bindings.cpp`, `actions.cpp`, `param_actions.cpp` (`enc_assign`, `enc_turn`, shared `write_moved`), `rec_actions.cpp` (STEP, no-FX), `view_state.h`, `pattern_edit.h`; tests `tests/enc_learn_test.cpp`, `tests/builtin_bindings_test.cpp`; QA-GLB-228..232.

## 4. Risks and notes

- A press of `Instr` while `Vol` is down (or the reverse) gives neither key's tap or hold, in either order.
- An Instr hold popup that already opened (400 ms or more before the second key) is not cancelled.
- A parameter of a non-selected instrument, or of a non-Sample instrument, records nothing and says HAS NO FX.
- Not checked on the unit: the feel of the chord timing and the encoder rate need a hardware pass.
