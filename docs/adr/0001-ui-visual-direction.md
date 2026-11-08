# ADR 0001 — UI visual direction: OctaMED/Amiga character, Renoise workflow depth

Status: **Accepted** (maintainer decision, 2026-10-10) · Applies to: Community OS UI engine and every screen built on it.

## Decision (maintainer's words)
> The UI should visually inherit the compact, utilitarian character of OctaMED/Amiga trackers: flat panels, thin borders, dense monospace data, rectangular controls, strong selected-row highlighting and a limited retro palette. Renoise should influence workflow depth, not visual styling. Everything must be redesigned for the Tracker OG's 800×480 screen, pads, buttons and encoder rather than copied as a desktop interface.

**North-star test (maintainer):** *"If OctaMED could have looked like this on an impossibly powerful 1990s Amiga, we're on the right track."* Every screen review asks this question first.

## What it means for implementers
- **Look:** flat filled panels, 1 px borders, no gradients/glows/rounded widgets by default, rectangular controls, monospace data grids, one strong highlight for the selected row/cell, a small fixed palette defined once in the style guide (no per-screen colours).
- **Workflow:** borrow Renoise's depth (instruments ≠ samples, phrases, modulation, expandable columns, context always visible), never its desktop layout, mouse idioms or visual style.
- **Hardware first:** every screen is designed around 800×480, the pads, buttons and encoder; no interaction that needs a pointer; legible at arm's length.
- **No copying:** OctaMED/Amiga/Renoise are inspiration; no assets, fonts or screenshots from them in the repo.

## Consequences
- A UI style guide (`docs/ui/style-guide.md`: grid, font sizes, palette, highlight and focus rules, component catalogue) is the first deliverable of the UI-engine phase; specs for screens cite it.
- The renderer needs only a small FT812 primitive set (rects, lines, bitmaps, monospace fonts, scissor), which keeps the device cheap to drive and the simulator's own display-list rasteriser small and faithful.
- Verification: simulator scenario screenshots are reviewed against the style guide (the PNGs are read and compared), and calibrated against on-device `CMD_SNAPSHOT2` captures in a `hw-session`.

## Open (for the style guide, not this ADR)
Exact palette, font sizes and grid metrics; whether a user-selectable theme set exists (themes would be SD data packs).

## Amendments
- **2026-10-13 (maintainer):** one red token, `Color::Rec`, for record mode only (frame, REC badge, playhead row), because the maintainer asked for "something in red or something very visible". It is the 17th theme colour; the style guide lists it with its contrast pairs. No other screen uses red, and the module SDK keeps its 16 colours.
- **2026-11-08 (maintainer request OS-SELTINT):** one more token, `Color::SelDim`, the 18th: the duller selection blue of the tracks of a multi-track block other than the cursor's. The style guide lists it with its contrast pairs; the module SDK keeps its 16 colours.
