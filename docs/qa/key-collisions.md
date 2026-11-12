# Key collisions: decisions

One entry per item of `docs/ui/ui-map.md` section 3 (collisions and overload). Each entry gives the collision in one line and the resolution. No behaviour changes here: "keep" means the current behaviour stays and is now documented. The reasons are in `docs/ui/ui-map.md`.

Item 17 (OS-PATTEMPO) is not in section 3 on `dev`, so it has no entry.

1. **Closed (OS-SHIFTROW).** Shift+F1-F5 had five meanings. Now Shift+F belongs to the screen; Global has none.
2. **Keep.** Shift+Up/Down means block, range, slice count, amount and row. Song adds Shift+Left/Right for its track range.
3. **Keep.** Focus Loop: Shift+Play is Pattern's (play from the cursor's row; with a block selected it loops the block, OS-UXF row 2). Shift+arrows are refused while the cursor follows the playhead. The Perform comment that calls Shift+Play "Global's" was stale and is fixed (`app/include/o3os/app/screens/perform_screen.h:26`).
4. **Keep.** F8 is UNDO, BACK or an action. Perform F8 RESET has no second-press guard (maintainer default, 2026-10-20).
5. **Keep.** Pad grammar splits: pad selects a sound on Pattern, Wave, Instrument and Record; pad selects the instrument on Sample load and Sample edit.
6. **Keep.** Press-again cycles over 3 (Config 6, Mixer 4, Params 4 or 5, and the nested F4 cycle).
7. **Closed (OS-INSTUI S3).** The F1 source page had over eight rows; the mode rows now swap on F1.
8. **Closed (OS-INSTUI S4).** The Wave MODE key: Shift+F1 with a Wave Shift row.
9. **Keep.** Config MIDI page is 7/8. F8 is the one free key, reserved for the OS-MIDI2 transport and clock rows.
10. **Keep.** Dead ends: modules only via the Navigator; overlays swallow screen buttons. The Params return after a sample load stays one level deep.
11. **Keep.** Vol then Fx is Learn only within the 40 ms chord window (`core/include/o3os/core/gesture.h:84`, `kGestureTimings.chord_ms`); otherwise it is the Vol popup.
12. **Keep (maintainer default).** Leaving Perform for any screen but Master and Song ends every FX, with no warning.
13. **Keep.** Text entry: F1 accepts (OK) and F8 cancels (BACK), as on every overlay (OS-UXF-KEYS item 5); F7 types the key in focus. The key row labels each key.
14. **Keep, with a rule.** Pattern's Shift row is full (8/8). The next Pattern feature goes into a chooser or a dialog.
15. **Closed (OS-METRO).** The dead METRO VOL row on the Config MIDI page moved to the PATTERN page.
16. **Closed (OS-SHIFTROW).** Global's Shift+F1-F5 jumps no longer shadow the screen's plain keys.
