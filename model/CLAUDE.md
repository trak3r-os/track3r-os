# model/ — project, editor/undo, parameters, settings

- Owns the in-memory project (`Project`: standard layout, trivially constructible, fixed arenas), the undoable `Editor`, parameter descriptor tables, device/project settings, pure step-edit rules. Must not: know a screen, a file or the audio engine, allocate, use `double`, or include anything but `core` (`check_layers.py`). Byte layouts of files live in `formats/`, not here.
- Entry points:
  - `include/o3os/model/project.h`: `Project`, `ModelArena`, mutators with checks and a `Status`
  - `include/o3os/model/editor.h` + `undo.h`: the undoable face of `Project`; undo units are bracketed by `begin_group()` / `end_group()`
  - `include/o3os/model/params.h`: which values are parameters (kind, index, field), ranges, defaults, owner struct
  - `include/o3os/model/settings.h`, `project_state.h`: settings and project-level state
  - `include/o3os/model/pattern_ops.h`: DUPLICATE, EXPAND and SHRINK of one track's 128-step column, pure (PAT-OPS-A); test `tests/pattern_ops_test.cpp`
  - `include/o3os/model/pattern_verbs.h`: INTERPOLATE, RANDOMISE (in place), ROTATE up / down and HUMANISE over one track's range, plus `apply_block` (a block of tracks through the Editor as one undo unit), pure (OS-PTOOLS2 S4; not yet wired to a dialog); test `tests/pattern_verbs_test.cpp`
  - `include/o3os/model/step_edit.h`, `step.h`: edit rules of one pattern step, on the model's encoding (note and instrument numbers are shifted from the OG's; see its header)
- Edits go through `Editor` so they are one undo unit named by an action (the app hands it to undoable actions only); a rejected edit records nothing; undo replays through the Project's own mutators and must restore everything by value.
- Add a parameter: a descriptor row and field in `params.h` and its owner struct; a persisted one also touches `formats/` and its doc. Anything the project file or settings file stores: version bump, backward-compatible loader, round-trip test, never a silent layout change (`formats/CLAUDE.md`, Level C).
- Tests: `tests/{model,undo,param*,settings_format}_test.cpp` (the undo property test runs random edit/undo/redo against full snapshots), `tests/data/pattern_edit_vectors.h`. Scoped: `ctest --test-dir build-os-host -R unit_model --output-on-failure` (or `-L model`). `model/` maps to everything in `tools/ci.sh`'s scope map, so `O3OS_CI_SCOPE=changed` does not narrow it; use `ctest -R`.
- Docs: `docs/formats/o3-project.md`, `docs/formats/o3-settings.md`, `docs/specs/OS-004-project-model.md`, ADR 0003 (track model), 0004 (step format), 0007 (project format), 0009 (parameters, undo). Hardware: `platform/mk66/tools/hil/{persistence,pattern_edit_undo,sample_edit_undo}.json`.
