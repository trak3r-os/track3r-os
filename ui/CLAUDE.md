# ui/ — renderer, themes, components, plots

- Owns display-list drawing over `eve/` (`Renderer`), the three compiled-in themes, the glyph/tile/icon faces, stateless components, and `plot/` (curves, scope, spectrum, wave view, RAM_G map). Must not: know what data means (screens in `app/` hand it rectangles and tokens), include `app`, `model`, `formats` or `audio`, write a colour value outside `src/themes.cpp`, or push a state past the display-list budget (RAM_DL 2048 words; every state stays at or below 1792, `ctest budget`).
- Entry points:
  - `include/o3os/ui/renderer.h`: draws into a display list through `eve/`
  - `include/o3os/ui/theme.h`, `src/themes.cpp`: 18 colour tokens (16, Rec, SelDim), faces, metrics; the only place colours are valued
  - `include/o3os/ui/components.h`: Strip, Badge, SoftKeys, FieldRow, Bar, Meter
  - `include/o3os/ui/assets.h`: RAM_G map and boot-time upload of the faces, tiles, icons
  - `plot/include/o3os/ui/plot/`: `ram_g_map.h` owns the whole RAM_G map
- Add a theme: a `Theme` in `src/themes.cpp`, then `kThemes`, `ThemeId`, `kThemeCount`; run `ctest -R contrast` (every pair must pass) and look at the gallery (`docs/ui/style-guide.md` section 9).
- Add a component: stateless, drawn from a layout rectangle and theme tokens; budget it in a sim screen state (`sim/src/screens*.cpp`) so `budget` and the gallery cover it.
- Generated data: fonts and icons are built from `tools/font_convert.py` / `tools/icon_convert.py` into the build tree. `src/boot_logo_data.cpp` is committed; regenerate with `python3 -I tools/logo_convert.py --png ui/assets/boot_logo.png --out ui/src/boot_logo_data.cpp` (`logo_convert_fresh` fails when stale).
- Tests: `tests/{overlay,budget,pattern_view,plot_*,raster*}_test.cpp`. Scoped: `ctest --test-dir build-os-host -R 'contrast|budget|unit_ui|unit_plot|plot_budget'`, or `-L app -LE e2e`; `O3OS_CI_SCOPE=changed tools/ci.sh host` maps `ui/` to the `app` label.
- Any look change moves sim goldens: `cmake --build build-os-host --target update_goldens` (`O3OS_GOLDEN_ONLY=<name>`), then view the PNGs. Regenerate, never merge or edit `sim/goldens.txt` by hand (`sim/CLAUDE.md`).
- Docs: `docs/ui/style-guide.md` (grid, type scale, palettes, budget, plots section 11), `docs/ui/ui-map.md`, ADR 0001 and 0010. QA rows go in `docs/qa/catalogue/<screen>.md` (format in `app/CLAUDE.md`).
