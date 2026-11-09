---
name: manual-writing
description: How to write a chapter of the track3r-os user manual (docs/manual/) from the QA catalogue, the screen specs and simulator screenshots, in plain task-based English that a cheap model can follow. Use for every manual chapter, the Quick start, the Cheat sheet and the LLM guide (llms.txt, llms-full.txt). Starts with the first rule, explain things (depth by screen complexity). Covers the audience (owners of the official firmware, friendly to newcomers), voice, structure, key notation, turning catalogue rows into steps, explaining hard concepts, images and accessibility, evidence labels, word budgets, the checklist and what never goes in.
---

# Writing the manual

The manual is for a musician with a Tracker on the desk and no programming background.
- **Most readers already own the unit and know the official firmware.** A chapter leads with what is new or different.
- **It stays friendly to newcomers.** Every term is defined on first use or linked to the glossary, and nothing assumes knowledge without a pointer to where it is explained.

It is plain, it explains, and it shows before it tells. Its sources are:
- the QA catalogue (`docs/qa/catalogue/<screen>.md`);
- the screen specs (`docs/specs/`);
- the UI map (`docs/ui/ui-map.md`);
- the map of the official manual (`docs/ui/official-manual-map.md`, OS-MANUAL2 M2-1);
- simulator screenshots.

So it follows the UI and never invents behaviour. One chapter per screen and one chapter per session; use the template `docs/manual/chapter-template.md`.

If a fact is not in a catalogue row, a spec or a file you read, do not write it. Write `<!-- TODO: ask: ... -->` and move on.

## 0. The first rule: explain things

Every chapter teaches how its screen works and why you would use it, with examples. A list of which key does what is not a chapter. (Maintainer, 2026-10-18.)

- **Depth follows how complex the screen is**, not "every screen equally".

  | Tier | Chapters | What they get |
  |---|---|---|
  | Deep | Perform, Song (order and arrangement), Mixer (routing and sends), FX (step FX), Record (the looper), Modules and module-instrument-fx, Pattern tools | A full **How it works**: the model, `###` sub-concepts each with an example and a picture, and a **Try it** walk-through |
  | Medium | Pattern, Instrument, Wave, Config, Export | A short **How it works** (one model paragraph and an example) and a **Try it** |
  | Brief | Sample edit, Sample load, File | A short **What it is for**, then the tasks. No How it works and no Try it. |

  A chapter not in the table is medium unless it has no screen (section 11).
- **Owners meet what is new or different first**, in the opening prose under the title (section 3). The explanation is deepest for what the official manual does not describe: the looper, modules, step FX, the USB disk and power off.
- **Examples are musical and concrete**: "Hold pad 3 while the pattern plays: track 3 repeats its last step until you let go", not "the pad triggers the effect".
- **Key tables are reference, at the end** (`## Keys on this screen`). They are never the body of the chapter, and never a substitute for an explanation.

## 1. What the style study found

Techniques taken from well-regarded instrument documentation. We copy the technique, never the text or the artwork.

| Technique | Seen in | Our rule |
|---|---|---|
| A short quick start before any theory: first sound in five minutes | Teenage Engineering guides (OP-1, EP-133), Korg volca and Novation Circuit quick sheets, Roland quick-start sheets | The Quick start chapter. No chapter needs to be read first. |
| A numbered, shallow table of contents, plus a "how to" section named after goals ("sync two units", not "clock settings") | Teenage Engineering EP-133 guide | Every chapter has a More tasks section with headings named after what the player wants to do. |
| Short action-first sentences, lower-case-friendly, one action per step | Teenage Engineering, Elektron | Section 3: voice. |
| Every control has one fixed written form, copied from the unit; the same key combination always means the same kind of thing (for example one key is always "yes", another always "no") | Dirtywave M8 manual and the community M8 tutorials (keys in brackets, combinations with a plus) | Section 4: key notation. Point out patterns ("Shift always shows the second row of keys") so readers stop memorising. |
| Concepts are explained with something familiar first, with an "you can skip this for now" for the deep part; new terms defined where they first appear | community M8 tutorial (hexadecimal explained through halves and quarters); Ableton Live manual (a "Live Concepts" chapter before the reference) | Section 7: the concept recipe. |
| A reference part that is separate from the lessons: chapters by workflow area, then keyboard shortcuts and fact sheets at the end | Ableton Live manual | Chapters teach tasks; the Cheat sheet page is the reference. |
| Panel diagrams and key maps with numbered callouts; the community also turns the key map into a one-page cheat sheet | Dirtywave M8 (key map, community cheat sheets), Elektron panel diagrams | Section 8: images; the Cheat sheet page. |
| A community-maintained reference that mirrors the vendor's notation so readers can cross-check | Synthstrom Deluge community documentation, M8 community tutorials | Our notation is the notation printed on the unit and shown on screen. |
| A complaint worth designing against: long manuals that assume a cover-to-cover read, when most owners want one answer | reviews of long vendor manuals | Short chapters, a goal-named heading for every task, a word budget (section 10). |
| Generous pictures and diagrams placed next to the steps they illustrate | Polyend's own Tracker manual (inspiration only, maintainer 2026-10-18) | Section 8: a picture beside each walk-through step that changes the screen. Ours are always our own: simulator screenshots and our generated SVGs. Never copy that manual's layout, text or pictures (section 14). |

Limits of the study: web research found little direct criticism of manuals. What was read directly: the Teenage Engineering EP-133 online guide (structure), a community Dirtywave M8 tutorial (notation, tone, concept teaching), the Ableton Live manual (table of contents), the llmstxt.org page. The rows for Elektron, Korg, Novation and Roland, and the Deluge community documentation, come from general knowledge of those manuals and were not re-checked online. Reviews of manuals are rare; praise is mostly about structure and the quick start, complaints about length and manuals that list features but not tasks. Treat the table as a style brief, not as measured fact.

## 2. Reading level and shape

- Aim for about school grade 7 to 8. Average sentence 12 to 15 words; no sentence over 25.
- One idea per sentence. One action per numbered step. Maximum 7 steps in a list; split longer ones under a new heading.
- Active voice, present tense, "you". Imperative for steps: "Press `Play`."
- Use the words the screen uses (UPPER CASE labels are printed on screen; copy them exactly, in bold: **FILL**). Do not invent synonyms. Pick one word for one thing and keep it ("step", not "row" and "line" and "slot").
- Define a term the first time a chapter uses it, in half a sentence, or link it to the glossary (`docs/manual/glossary.md`). Add every new term to the glossary. Each chapter does this for itself, because readers arrive at any chapter.
- Never assume knowledge without a pointer. If a step needs something explained elsewhere, link that chapter or glossary entry where the step needs it.
- Say what you see after an action ("The strip says **TRACK 03 NAMED**.") so the player can check they did it right.
- Numbers with units (`120 BPM`, `1500 ms`). Spell out one to nine in prose, digits in tables and for anything on screen.
- No jokes, no marketing words, no "simply", "just", "easily", "obviously". No "please". Friendly, calm, short.
- Spelling: American English throughout.

Do and don't:

| Don't | Do |
|---|---|
| The user can utilise the Fill function in order to populate the selected range with values. | Press **FILL** (`F4`). Choose a value. Press **FILL** again to fill the block. |
| Please note that it is necessary to ensure that Rec is enabled prior to input. | Press `Rec` first. The screen shows the red **REC** badge. |
| The encoder is a rotary control that modifies the parameter at the cursor. | Turn the encoder to change the value under the cursor. Hold `Shift` for bigger steps. |
| Glide is applied only if the preceding note is legato. | Glide slides the pitch from the last note. It needs a note before it on the same track. |
| This can be easily done. | (Delete it. Show the steps.) |

## 3. Chapter structure

Every screen chapter has these parts, in this order (OS-MANUAL2, maintainer 2026-10-18). Keep the headings exactly, so a script and a reader can find them. Section 0 says which tier leaves parts out.

1. **Title** (`# Pattern`), and under it the status mark if the chapter is incomplete (`Status: draft`).
2. **The opening.** No heading. 2 to 4 sentences of plain prose for someone who knows the official firmware.
   - Lead with what this screen does differently or adds, and which keys moved.
   - For a screen the official manual does not describe, the first sentence says what it is for.
   - Never use a fixed phrase or a heading such as "Coming from the official firmware". Never name a comparison section. Write it as a person would say it ("Perform works on the whole pattern at once, live: nothing you do here is written into the steps.").
3. **What it is for** (2 to 4 sentences): what you do here, when you come here, and how you get here (the button).
4. **How it works** (deep and medium tiers): the model, in the recipe of section 7.
   - What is live, what is stored, and what ends it.
   - One picture, and examples.
   - Deep chapters split it into `###` sub-concepts.
5. **Try it** (deep and medium tiers): a walk-through of 5 to 7 numbered steps that uses the model.
   - It ends with what you hear or see.
   - Each step that changes the screen may carry its picture right below it.
   - The walk-through is the simulator scenario `sim/scenarios/manual/<screen>_walk.txt` (OS-MANUAL2 M2-4), so its steps and pictures cannot drift apart.
6. **The screen at a glance**: one screenshot, then a table of its areas (header, main area, info line, key row, Shift row), one line each. Callouts if the image has them.
7. **More tasks**: 3 to 8 task sections.
   - Each is named after a goal ("Name a track", "Mute everything at once") and starts from a stated place ("Start on the Mixer screen.").
   - Each has numbered steps and ends with what you should see.
   - Say why you would do it when the goal is not obvious.
8. **Keys on this screen**: reference only, at the end. First the generated block (`<!-- manual-tables: keys screen=<name> -->` ... `<!-- /manual-tables -->`): the screen's key row and its Shift row, written by `python3 tools/manual_tables.py` from the firmware's key-row tables; never edit between the markers, and `--check` fails when it is stale. Then one hand-written table of what each key does and of the keys that are not on the key row; this is the only place where IDs show (the quiet `Ref` column).
9. **Good to know**: limits, things that surprise, and differences from the official manual (as the map records them) not already said in the opening, with the evidence labels of section 9.
10. **See also**: links to the 2 or 3 chapters that continue the story.

Cross-references: link to the chapter, not to a spec. Write "see [Song](song.md)". Never write "as above" or "see section 3". Link a term to the glossary the first time it appears in a chapter.

Chapters that do not belong to a screen: **Quick start** (`quick-start.md`) and **Cheat sheet** (`cheat-sheet.md`), both in section 11.

### Callout boxes

Use a blockquote with a bold lead word. Only these three, never nested. The walk-through is its own section (**Try it**), not a box.

```
> **Tip**: ...        (a shortcut or a habit; one sentence)
> **Careful**: ...    (data loss, an action that cannot be undone, a mode that blocks other keys)
> **Original firmware**: ...   (a difference met in the middle of a task, not already said in the opening; see section 9)
```

Budget: at most two Tips, two Careful and two Original firmware boxes per chapter. Careful is only for real loss ("The Mixer has no undo.").

## 4. Key notation

Controls are written in backticks, with the name printed on the unit or used in `docs/ui/ui-map.md`. Use these forms and no others.

| Write | Meaning |
|---|---|
| `Play`, `Rec`, `Shift`, `Pattern`, `Instr` | a button |
| `F1` ... `F8` | a soft key (the 8 keys under the screen). When the label matters add it in bold: `F4` (**FILL**) on first use, then `F4` or **FILL**. |
| `Shift` + `F3` | hold the first, press the second, release both. Always the held key first. |
| hold `Instr` | press and keep it down (longer than a tap) |
| `F7` + `F8` together | two keys pressed at the same time (a chord), written "`F7` and `F8` together" |
| `Left`, `Right`, `Up`, `Down` | the arrow keys |
| the encoder; turn the encoder; `Shift` + turn the encoder | the knob. No push function. |
| pad 5, any pad, `Shift` + pad 5 | one of the 48 pads |
| then | a sequence: `Shift` + `F1`, then type the name |

Rules:

- Do not write keys as `[F3]`, `Shift+F3`, `SHIFT-F3` or `Fn`.
- Name the key a player presses, not the internal action (`ui.none`, `block.paste` never appear).
- When the same key does different things on different pages, say the page: "On the **MIXER** page, `F7` mutes the track."
- Write the Shift row as its own small table when a screen has one (what `Shift` + `F1` ... `F8` do), because the unit draws that row while `Shift` is held.

## 5. From catalogue rows to steps

A QA row has: ID, Function, How (gesture), Expected, Sim, Unit, Spec, Status. The reader needs the first four, rewritten.

1. **Group rows into tasks.** Rows that belong to one goal (QA-MIX-010, -011, -012: naming a track) become one "More tasks" section. One row never becomes a whole section unless the task is that small.
2. **Gesture to steps.** The "How" cell becomes numbered steps. One key or action per step. Put the starting place first ("Start on the Mixer screen.").
3. **Expected to the last line.** The "Expected" cell becomes "You should see ..." (what is on screen), plus one line for what is stored or changed ("The name is saved with the project."). Cut internals: file names of tests, flags, spec numbers, code identifiers. Keep numbers a player meets (20 characters, 4 seconds).
4. **Keep the IDs, quietly.**
   - In a steps list, put the ID in an HTML comment at the end of the step it proves: `1. Press `Shift` + `F1`. <!-- QA-MIX-010 -->`. It does not render; the checker can still count it.
   - In the Keys table, the last column is `Ref` and holds the ID. That is the only visible place.
   - Never put an ID in running prose.
5. **Status decides what you write.** Read the Status cell:
   - `sim PASS` or `host PASS` or `unit PASS`: write it.
   - `unit pending` / `unit open` next to a PASS: write it with no hardware label.
   - Neither a sim nor a unit PASS (Open rows): do not write the behaviour. Add one line at the end of the chapter file: `<!-- Open rows, not written: QA-XXX-nnn, ... -->`.
6. **Edge rows** (refusals and dim keys) go into "Good to know" as one line each: "On a free slot `Shift` + `F3` does nothing."
7. **Check the row before you quote it.** If the Expected text names a screen label, spell it as the catalogue spells it.

Example. Row: *Name a track (Shift + F1) ... the on-screen keyboard opens titled TRACK NAME ... F8 keeps it and the strip says TRACK nn NAMED.* becomes:

```
### Name a track

Start on the Mixer screen. Put the cursor on the track.

1. Press `Shift` + `F1`. <!-- QA-MIX-010 -->
   The keyboard opens with the title **TRACK NAME**.
2. Use the pads to type a name. It can have up to 20 characters.
3. Press `F8` (**OK**) to keep it. `F7` leaves the old name.

You should see **TRACK 03 NAMED** in the strip. The name shows on the Mixer and on the Pattern header, and it is saved with the project. <!-- QA-MIX-011 -->
```

## 6. Keeping facts true

- Names, limits and labels come from the catalogue, the specs and `docs/ui/ui-map.md`, not from memory.
- When a sentence says "always", "never" or gives a number, point to the row or file you took it from in a hidden comment: `<!-- QA-PAT-012 -->` or `<!-- docs/specs/OS-NAMES.md -->`.
- If a catalogue row and the UI map disagree, write nothing and add `<!-- CONFLICT: ... -->` for the maintainer.

## 7. Explaining a complex concept

This recipe fills **How it works**, at the depth of the chapter's tier (section 0). Use it for anything hard:
- signal flow and sends;
- tracks and voices;
- step FX;
- the mod matrix and LFOs;
- play modes and slices;
- Perform and its scenes;
- pattern chains, song order and the arrangement;
- the looper;
- stems;
- module instruments.

In a deep chapter, each idea is its own `###` sub-concept, named for the idea ("What is a send?").

1. **What it is**, in one or two sentences.
   - Use an everyday comparison where one exists ("A send is a second volume knob that feeds a shared effect, like a mixing desk's aux knob.").
   - The comparison must be true. If it only half fits, say where it stops.
2. **Why you would use it**: one sentence about the result a player hears or gains.
3. **The model**: what is live (heard now, not written), what is stored (in the pattern, the instrument or the project), and what ends it (releasing a pad, leaving the screen, stopping). These three questions are where owners get lost.
4. **An example**: a concrete musical case in one or two sentences ("With LATCH on, tap pad 5 once: track 5 keeps repeating until you tap it again, so both hands are free for the encoder.").
5. **A small picture** for flows and layouts, as a generated SVG (section 8). One example is a left-to-right strip of boxes: track, send, reverb, master.
6. **The details** come after the example: the rules, ranges, exceptions and tables of values. Start with "If you want to know more".

Order rules:
- The example comes before the details.
- The picture comes before the paragraph that describes it.
- No new term appears without a definition or a glossary link one line above.

The hands-on exercise is the chapter's **Try it** section, after How it works. It is not a box inside it.

## 8. Images

Three kinds. Use the cheapest that does the job. A screen that can be described in one sentence needs no image.

| Kind | Use for | How it is made | Status |
|---|---|---|---|
| **Screenshot** | what a screen looks like; the result of a task | simulator PNG | pipeline exists in part (below) |
| **Panel and key map** | where the buttons are; which keys a task uses | `docs/manual/img/panel.svg` with the keys of one combination marked, generated by `tools/manual_diagrams.py` | exists (below) |
| **Concept strip** | signal flow, order, ranges | the screen map and the signal flow are generated (`tools/manual_diagrams.py`); anything else is a small SVG or a fenced text diagram by hand | partly generated |

### Screenshots (simulator)

The simulator `build-os-host/sim/o3sim` renders the real UI code to PNG. Two sources, both deterministic and checked against goldens (`sim/README.md`):

- A scenario `sim/scenarios/qa/qa_<screen>.txt` with `snap <name>` lines writes `<name>.png` into `--out <dir>`. The catalogue's Sim column names it: `qa/qa_mixer.txt › qa_mix_002` means scenario `qa_mixer`, snap `qa_mix_002`. Prefer these: they are the exact state a row describes.
- The gallery `o3sim --gallery <dir>` writes `<screen>_<state>_<theme>.png` for each registered screen state. Use it for the general "at a glance" picture.
- The walk-through's own scenario `sim/scenarios/manual/<screen>_walk.txt` (a deep or medium chapter's "Try it"): one block per step, the key presses as the chapter writes them, `snap walk_<step>` after each step that changes the screen, and `assert state ...` for a sentence the screen can prove (`msg=SCENE_1_STORED`). `sim/scenarios/manual/perform_walk.txt` is the sample to copy. The manual numbers the pads 1 to 48 and the scenario's `pad` takes 0 to 47: the manual's pad N is `pad N-1`. A screen that needs the engine (Perform's slots) starts with `document sound`, `audio on`. Its pictures are `<screen>_walk_<step>.png`, manifest lines `<screen>_walk_<step>.png manual/<screen>_walk walk_<step>`; regenerate the goldens with `update_goldens` after a change.

Rules:

- Theme: Amiga (the default) for every picture, so the book looks the same everywhere. Size: as rendered (800 x 480), no scaling by hand.
- Be generous where it helps, and place each picture next to the step or idea it shows (the approach of the official manual, taken as inspiration only).
  - Each walk-through step that changes the screen may carry its picture.
  - A task gets at most one picture.
  - Caps per chapter, the "at a glance" picture included: deep 12, medium 7, brief 4.
  - The Panel chapter (`panel.md`) is the key reference, not a task chapter: it may show the panel, the screen map and up to three chord figures (Learn, Navigator, the keys held at power-up), each with its `extra =` reason in `diagrams.txt`.
- Use a picture when it shows something words do not, such as a dialog, a keyboard, a full pattern grid or a flow, not to repeat a sentence.
- Never edit a screenshot by hand and never draw over it. Callouts go in a sidecar (below).
- Do not commit PNGs that are not referenced. The PNGs are generated outputs and they are committed (maintainer, 2026-10-17: a narrow exception to "no binaries in git", so GitHub shows the chapters with their pictures): the generator writes them to `docs/manual/img/`, you commit them with the chapter, and chapters reference `img/<name>.png`. Ctest `o3sim_manual_images` and `tools/manual_check.py --strict` (in `tools/ci.sh host`) fail on a stale PNG or a chapter that shows a picture with no file.

File names: `<screen>_<snap-or-state>.png`, lower case, underscores: `mixer_qa_mix_002.png`, `pattern_typical.png`. Key-combination diagrams: `chord_<name>.svg` (for example `chord_live_rec.svg`); the base is `panel.svg`. The screen map is `map_screens.svg`, the signal flow `concept_signal_flow.svg`; other concept strips `concept_<name>.svg`. All of them are generated and committed.

Alt text is required on every image, never empty.
- It is one sentence that says what the picture shows and what to notice, not "screenshot of".
- Pattern: "<Screen>: <what is shown>, <what to notice>."
- Example: `![Mixer, MASTER FX page: the limiter card says ALWAYS ON.](img/mixer_qa_mix_002.png)`.
- Under 25 words.

An image that needs a numbered callout list gets a one-line caption below it.

### Accessibility (maintainer, 2026-10-18)

- Plain language and short sentences (section 2).
- Alt text on every image (above).
- **No information carried by colour alone.** In text, name the colour and the label ("the red **REC** badge"). In our SVGs, pair every colour with a shape, a number or a label.
- **Good contrast in our SVGs:**
  - text against its background at least 4.5:1;
  - lines and key outlines at least 3:1;
  - checked in both the light and the dark rendering where the SVG has one.
- **A clean heading hierarchy:** one `#` title, then `##`, then `###`. Never skip a level, and never use a heading only for its size.
- **Tables have a header row.** Never use a table for layout.
- **Link text names its target:** "see [Song](song.md)", never "here", "click here", "this page" or a bare URL.

`tools/manual_check.py` checks the cheap part (OS-MANUAL2 M2-5), on every page including the index and the template: missing, empty or file-name alt text, alt text over 25 words or starting "screenshot of", a heading that is not the one `#` title first, a second `#`, a skipped level, a table without a header separator row or with an empty header cell, "here"-style links, a bare URL as link text or left in the text. `-v` lists the lines that name a colour, for the fact reviewer to check the meaning is also in words. Contrast of the SVGs is a review item.

Callouts: numbered circles over a screenshot or panel, explained by a numbered list under the picture ("1. The track name. 2. The meter."). Maximum 6 per picture. For screenshots, the generator writes `<name>.svg` that embeds the unchanged PNG and draws the circles from a sidecar `img/<name>.callouts` (lines `x y n`, in picture pixels); the chapter then references the SVG.

### Panel and key map

One schematic SVG of the unit, our own boxes and labels, not a photograph or an outline traced from the vendor's artwork. Contents, from `docs/design/product-ux.md` (control table): 48 pads (4 rows of 12), 8 soft keys `F1` to `F8`, the 11 screen buttons (Params, Performance, File, Config, Master, SampleLoad, SamplePlay, SampleEdit, SampleRec, Song, Pattern), the column buttons (Note, Instr, Vol, Fx), arrows, encoder, `Play`, `Rec`, `Copy`, `Insert`, `Delete`, `Shift`, the screen. Each control is a `<g id="key-<name>">` (ids `key-f1`, `key-shift`, `key-play`, `key-pad-1` ...), with a text label inside, in a plain grey style.

`tools/manual_diagrams.py` writes one `chord_<name>.svg` per stanza of `docs/manual/diagrams.txt`: the panel, cropped to the part the keys are on, with the keys of one combination marked (a ring is a key held down, a filled key is pressed, the numbers 1 and 2 give the order, a label says what an F key does). The keys are not typed into the manifest: each `bind = <Context> <action> [<arg>]` line names a row of `app/src/bindings.cpp` and the diagram takes the keys from it (the boot chords and the Power hold come from `platform/mk66/src/app_main.cpp` and `core/power_hold.h`), so a diagram fails the check when its binding moves. To add one: add a stanza (chapter, `region = soft|nav|edit|power` (joined by `+` for two parts: the figure is cropped to the marked keys plus one key), alt text of 25 words at most, caption of 130 characters at most, the `bind` lines), run `python3 tools/manual_diagrams.py`, and paste the line that `--list` prints into the chapter, where the combination is explained. `--check` (ctest `manual_diagrams`) fails when an SVG is stale, missing, not listed, over 6 KB (a cropped figure is about 4 KB), or not in its chapter with that alt text, when a label of the diagram is not in the chapter, or when a chapter shows two or more chord diagrams and one after the first has no `extra = <why it earns its place>`. A figure marks at most four keys (keys that share a label, like the four arrows of a block, count once). A figure of a Shift row (only Shift + `F1`..`F8`) is an error unless the stanza says `why = <...>`: the unit draws its own Shift row, and `panel.md` says once that Shift relabels `F1` to `F8`. Use a diagram when a task uses a key combination a newcomer will not find easily (a chord, the hold of `Note`, the keys held at power-up); use none for a Shift row or when the steps are single, obvious presses. Never edit an SVG by hand.

If the SVG turns out too costly, the fallback is a Markdown table in the Cheat sheet: a text map of the unit in a code block, one line per control group. Keep it as an equal fallback, not a rival.

**Open point for the maintainer:** the physical positions of the controls are not recorded in the repository (the control table gives the roles and counts only). Whoever builds `panel.svg` needs the maintainer to confirm the layout from the unit (or an own photo kept out of git). Positions are the only thing missing; the ids and labels are known.

### Tools, and what exists

| Job | Tool | Exists? |
|---|---|---|
| Render a scenario's snaps | `build-os-host/sim/o3sim --scenario <file> --out <dir>` | yes |
| Render the gallery | `build-os-host/sim/o3sim --gallery <dir>` | yes |
| Render the snaps listed in `docs/manual/images.txt` into `docs/manual/img/` (commit the PNGs; callout SVGs and `panel` copies are not built yet) | `tools/manual_images.py` | yes |
| Make the key-combination diagrams, the screen map and the signal flow (`--check` runs as ctest `manual_diagrams`) | `tools/manual_diagrams.py` | yes |
| Write the key tables between `<!-- manual-tables: ... -->` markers from the firmware's key-row tables (`--check` runs in `tools/ci.sh host`, `--self-test` is ctest `manual_tables_selftest`) | `tools/manual_tables.py` | yes |
| Check links, index, manifest, TODOs; list what the catalogue changed since each chapter's sync sha (`--stale`) | `tools/manual_check.py` | yes (links, index, manifest, stale; the checklist items of section 13 are not mechanised yet) |
| Build the LLM guide (`llms.txt`, `llms-full.txt`; `--check` runs in `tools/ci.sh host`) | `tools/manual_llms.py` | yes, first version: chapters concatenated, comments stripped (the catalogue and ui-map parts of section 12 are not built yet) |

`tools/manual_images.py` (Python 3, standard library only, no firmware; the callout SVGs below are specified but not built yet):

- Reads the manifest `docs/manual/images.txt`, one image per line: `<name>.png <scenario> <snap>` (a `snap` line of `sim/scenarios/qa/<scenario>.txt`), `<name>.png <folder>/<scenario> <snap>` (a snap of `sim/scenarios/<folder>/<scenario>.txt`: the walk-throughs, `manual/<screen>_walk`) or `<name>.png gallery <screen>_<state>` (the Amiga-theme gallery picture). A line ending in `# MISSING` names a source that does not exist yet and is skipped.
- Runs `o3sim` once per scenario (and once for the gallery) at nice 10, one process at a time, and copies the pictures to `docs/manual/img/<name>.png`. The PNGs are committed, never edited by hand.
- `--check` regenerates into a temporary directory and reports the images whose bytes changed, are missing or are not in the manifest (`--check --strict` exits 1 for any of them: ctest `o3sim_manual_images`); `--list` prints the manifest. It never builds: it expects `build-os-host/sim/o3sim`.
- Not built yet: a `.callouts` sidecar writing `<name>.svg` with numbered circles.

Specification of `tools/manual_check.py` (the per-chapter checklist, mechanised in part): fail on a missing section heading from section 3; an image path with no manifest line; an image with empty alt text; a `[F3]`-style key form; the words in the never-list (section 13); word count above the chapter's cap; a visible `QA-` ID outside a table's `Ref` column; a catalogue ID in the chapter that does not exist in the catalogue.

## 9. Evidence labels, in plain words

The repository's labels are `verified parity`, `new Community OS behaviour` and `open research question`. A reader should never need those words. Write the plain version, in an **Original firmware** note or as a short tag after the sentence.

| Repository label | Say this | When |
|---|---|---|
| verified parity | "Works the same as on the original firmware." | the catalogue or spec says parity with evidence observed on an official unit |
| documented same (OS-MANUAL2) | "Works as the official manual describes." | the official-manual map marks it "same" and our QA row for it passes; a description in the official manual is documented behaviour, not verified parity |
| new Community OS behaviour | "New in track3r-os." | the official-manual map marks it new (the official manual does not describe it), or the catalogue says new Community OS behaviour; say nothing about what an official unit does |
| open research question | "Not checked against the original firmware." | parity unknown; also use for anything the spec calls open |

Rules:

- Never write "same as the original" without the row or spec saying so with evidence.
- Do not say "compatible" or "fully supported" for anything marked pending.
- Say it once per feature, where the player first meets it, not on every line. A chapter heading may carry the label for a whole new screen page: "New in track3r-os."
- Keep the name "original firmware". Do not write "OG", "OS-xxx" item IDs or spec numbers in the text; they go in hidden comments.

## 10. Word budgets

Counted without tables of keys, front matter and hidden comments. A chapter over its cap is split, not squeezed.

| Part | Words |
|---|---|
| The opening (no heading) | 30 to 80 |
| What it is for | 40 to 90 |
| How it works, medium tier | 80 to 250 |
| How it works, deep tier | 250 to 900, as `###` sub-concepts of 120 to 250 each |
| Try it (5 to 7 steps) | 80 to 180 |
| Screen at a glance (table and lead-in) | 80 to 160 |
| One "More tasks" task | 40 to 150 (3 to 7 steps) |
| Good to know | 60 to 200 |

Chapter caps (all parts together, tier from section 0):

| Chapter | Cap |
|---|---|
| Quick start | 1100 (both paths, section 11) |
| Brief: Sample edit, Sample load, File | 400 to 800 |
| Medium: Wave and Chop, Config, Export | 800 to 1300 |
| Deep: Perform, Song, Mixer, FX, Record, Modules, Pattern tools | 1200 to 2200; past 2200, split into sub-pages |
| Pattern, Instrument | 1800 each, in two or three sub-pages (for example Pattern: editing; recording; tools) |
| Cheat sheet | 1 printed page of tables, under 400 words of prose |

## 11. The two chapters without a screen

**Quick start** (`docs/manual/quick_start.md`, 1100 words in all) is "what's different, in five minutes" (OS-MANUAL2, maintainer 2026-10-18). It is written after the screen chapters it links to. Fixed shape:
1. **One opening sentence** says who it is for: owners who know the official firmware. Point newcomers to the short path in part 4.
2. **Installing:** one line and a link to `install.md`. Do not repeat it.
3. **What's different:** the 6 to 8 biggest differences an owner will meet first. Each has one or two sentences, the key, and a link to the chapter that explains it.
   - The features the official manual does not describe come first (the looper, modules, step FX, the USB disk, power off).
   - Then keys that moved, and screens that behave differently.
4. **New to trackers?** A short path of at most 300 words:
   - power on and what the screen shows;
   - make a sound (pick an instrument, press a pad);
   - put four notes in a pattern and press `Play`;
   - save the project.

   Each term gets a glossary link, and each step at most one picture.
5. **Where next:** five links.

**Cheat sheet** (`docs/manual/cheat-sheet.md`). The key map on one printable page: the panel diagram with the screen buttons, then tables: the global keys (`Play`, `Rec`, `Shift` combinations, Learn, Navigator, theme), the soft-key row of every screen in a grid (rows: screens; columns: `F1` ... `F8`; a second grid for the Shift row), and the five or six habits that work everywhere ("press a screen button again for its next page"). No prose beyond one line per table. Built from `docs/ui/ui-map.md` and the Global catalogue; regenerate it when either changes (the `manual_llms.py` generator below can emit these tables too).

## 12. The LLM guide (`llms.txt`, `llms-full.txt`)

Two files, in `docs/manual/`, for chat assistants and for tools that drive the unit. A user pastes the full file into an assistant to get help; a tool reads the index first. The convention is llmstxt.org: `llms.txt` is a Markdown index (one H1 with the name, a blockquote summary, H2 sections of links with a one-line note each, an `Optional` section for the secondary links). `llms-full.txt` is the same content combined in one file for loading whole into a context window. Other products publish the same pair (an index file plus a full export), and warn that the full file can be too large for some models, so keep ours compact and split by screen as well (see below). Nothing in this convention is a standard for the full file; our shape is our own.

### Format

`llms.txt` (under 3 KB):

```
# track3r-os

> Community operating system for the Polyend Tracker hardware. Unofficial. This index lists the reference files an assistant should read.

Short notes: key notation, evidence labels, what the OS is not.

## Reference
- [Full reference](llms-full.txt): every screen, key, combination, mode, limit and term in one file
- [Keys](llms-keys.txt): the key map and every soft-key row (small)

## Screens
- [Pattern](llms/pattern.txt): ...one line each

## Optional
- [Install and update](install.md): ...
```

`llms-full.txt` is dense and structured, no marketing, no tutorials. Fixed order:

1. **Preamble** (under 200 words): what the unit is, the notation used below, the evidence labels, the rule "if a thing is not listed here, it is not known; say so".
2. **Hardware** : the controls (48 pads, `F1` ... `F8`, 11 screen buttons, column buttons, arrows, encoder, transport, `Shift`), what each does globally, in one table.
3. **Global behaviour**: the press-again page cycle, Shift row, Learn, Navigator, theme, undo areas, what is final.
4. **One section per screen**, same headings every time: `Purpose`, `Reach it` (button), `Pages`, `Keys` (a table: key or combination, page or mode, effect, limits, evidence label, catalogue ID), `Modes and states` (what changes the meaning of keys: record modes, held modifiers, overlays), `Values and limits` (ranges, counts, maximum lengths), `Refusals` (what a key does when dim or blocked, and what the screen says), `Common tasks` (goal, steps as a key sequence).
5. **Formats and files**: what the OS reads and writes on the card, folders, names.
6. **Glossary**: every term with a one-line definition.
7. **Known gaps**: open questions. An assistant must tell the user when it is unsure.

Style: one fact per line; tables over prose; the key notation of section 4; screen labels in upper case as drawn; IDs in the visible `ID` column (this file is for machines, so IDs are visible here); every claim carries its label; no screenshots. Keep the full file under about 60 KB (about 15k tokens); if a section grows past it, split by screen into `llms/<screen>.txt` and keep `llms-full.txt` as the concatenation.

Writing for tools that drive the unit: use the same key names as the HIL console and the simulator scenario grammar (`sim/README.md`) in the `Common tasks` sequences, so a tool can send them; say which behaviour needs the unit and not the simulator.

### Single source, no drift

The guide is **generated**, never hand-edited, from the same sources as the manual:

| Content | Source |
|---|---|
| Keys, combinations, Shift rows, pages, overlays | `docs/ui/ui-map.md` (the screen table), checked against `app/src/bindings.cpp` by `tests/builtin_bindings_test.cpp` and `tests/shift_labels_test.cpp` |
| Behaviour, expected results, evidence, status | `docs/qa/catalogue/*.md` |
| Plain-words task steps, concepts, glossary | the finished manual chapters in `docs/manual/` |
| Hardware controls | `docs/design/product-ux.md` control table |
| Formats | `docs/formats/` |

A generator **is worth writing**: the guide is large, mechanical and must track the UI. Specification of `tools/manual_llms.py` (Python 3, standard library only, no firmware, read-only on the sources):

- Parse the catalogue tables (rows by ID, columns by header) and the ui-map screen table (the Pages, F1 to F8, Shift, Other keys columns).
- Parse the chapters' `Keys on this screen` tables and the `Try it` and `More tasks` sections (the older `Try this` box and `How do I...` heading until every chapter is rewritten); copy them in under the screen's `Common tasks`, converting the hidden `<!-- QA-... -->` comments into visible `[QA-...]` tags.
- Emit `llms-full.txt` in the order above, `llms.txt` with the screen list, and optionally `llms/<screen>.txt`.
- Map the Status cell to labels: `PASS` with parity evidence to `same as the original firmware`; new Community OS behaviour to `new in track3r-os`; `unit pending` to no label; open rows are listed under Known gaps.
- `--check` regenerates in memory and exits 1 if the committed files differ, so CI (or the manual check) catches drift. Also fail when a catalogue ID cited in a chapter is missing, or a screen in the ui-map has no section.
- The glossary comes from `docs/manual/glossary.md` (one `term: definition` per line).
- It needs the chapters to follow section 3 exactly; that is another reason the headings are fixed.

Until it exists: do not hand-write `llms-full.txt`. A hand-written copy is where drift starts.

## 13. Per-chapter checklist

Before handing a chapter back, tick every line. A reviewer re-checks the same list.

1. The chapter has the headings of section 3 for its tier (section 0), in order, and nothing else at heading level 2.
2. The chapter explains (section 0). For deep and medium tiers, How it works answers what is live, what is stored and what ends it, with at least one musical example. The key table is not the body.
3. The opening is plain prose with no heading and no fixed comparison phrase. It leads with what is new or different, and every same/different claim matches the official-manual map and section 9.
4. The Try it walk-through has 5 to 7 steps, ends with what you hear or see, and matches its scenario `sim/scenarios/manual/<screen>_walk.txt` step for step.
5. Every task is named after a goal and starts from a stated place.
6. Every step is one action, written with the key forms of section 4.
7. Every key and combination in the catalogue rows for this screen appears in the Keys table, with its ID in the `Ref` column; nothing in the table is missing from the catalogue.
8. Every "You should see" matches the row's Expected text.
9. Rows with only open status are not written, and are listed in the hidden comment.
10. Evidence labels follow section 9; nothing says "same as the original" without evidence.
11. Accessibility (section 8): every image has non-empty alt text that says what it shows; no meaning by colour alone; no skipped heading level; every table has a header row; link text names its target.
12. Each image has a manifest line, a file name from section 8, sits next to the step or idea it shows, and stays within the tier's picture cap.
13. New terms are defined on first use in this chapter or linked to the glossary, and added to the glossary; nothing assumes knowledge without a pointer.
14. Word count within the cap of section 10; no sentence over 25 words.
15. Cross-references are links to chapters; no spec numbers, item IDs (`OS-xxx`) or code names in visible text.
16. The never-list below is clean.
17. If the checker script exists: `tools/manual_check.py <chapter>` passes. If not: say so in the hand-back.

## 15. Update mode (keeping the manual in step with the OS)

Sync v2 (OS-MANUAL2 M2-3). Every section that states behaviour carries a marker right below its heading:

```
<!-- sync: src=<path>[,<path>] spec=<docs/specs/..> qa=<QA-ID>[,<QA-ID>] -->
```

- A section is the text under one heading, and its name is the heading's GitHub anchor: `perform.md#store-a-scene`.
- A "How it works" or "Try it" section must have the marker; any other section that states behaviour should. One marker per section. Each field is optional but at least one is needed; a path is a file or a folder of the repository, and every path and QA ID must exist.
- The old `<!-- src: ... -->` comment is refused.
- `docs/manual/sync.lock` has one line per marked section: the git blob hash of each cited path (a folder: a hash of its tracked files; a manual page: its text without the `manual-tables` blocks, so a renamed key does not reach the cheat sheet) and a hash of each cited QA row (its Function, How and Expected cells, not its Status), as they were when the section was last acknowledged. It is written by the tool, never by hand: after you have checked the section against what it cites, run `python3 tools/manual_check.py --ack <chapter>#<section>`. The same command drops the line of a section that is gone. A new section is acknowledged the same way. A file that many sections cite (`app/src/bindings.cpp` is cited by 18) is acknowledged for all of them at once: `python3 tools/manual_check.py --ack-path <file> [<file> | <QA-ID> ...]` (only that item moves in each line). Read the sections that cite it first: the command says you did.
- **A conflict in `sync.lock` after a merge** (two lanes acknowledged the same file): take either side (`git checkout --ours docs/manual/sync.lock`, or `--theirs`), then on the merged tree run `--ack-path <file>` for each file both sides changed. `--ack-all` writes every line again from the tree, without looking at what moved: use it only when the merge left many lines wrong, after you have checked that nothing else moved (`--stale` lists the sections whose sources moved). Commit the lock. A lock that still has `<<<<<<<` markers fails `--strict` and `--gate` and says so.
- The branch gate (`tools/ci.sh host`, `python3 tools/manual_check.py --gate`): on a branch, every section that cites a file this branch changed (or a QA row whose text it changed) must have changed too, either its text or its line of the lock. Otherwise the gate names the sections and prints one ready-to-run `--ack-path` command for everything they cite. On `dev`, or with no merge base, it only warns. `O3OS_MANUAL_BASE` or `--base <branch or commit>` overrides the base (default `git merge-base HEAD dev`); a name that is not a commit fails the gate. GitHub CI sets it to `origin/<target branch>` for a pull request, with the full history, so the gate runs there too.
- `python3 tools/manual_check.py --stale` is the warning report, and `ci.sh host` prints it: the QA rows added, changed or removed since each chapter's `manual-sync` sha, and the sections whose sources moved since their line of the lock.
- The chapter-level `<!-- manual-sync: <sha> -->` stays until every section of a chapter has a marker. Chapters that have no catalogue file (`computer_tools.md`, `install.md`) follow their `src=` markers; `glossary.md` is exempt; `cheat_sheet.md` follows every catalogue file and cites the chapters it repeats.

Every chapter starts with `<!-- manual-sync: <sha> -->`: the repository commit it was last brought in step with. The orchestrator runs `python3 tools/manual_check.py --stale` at each doc-keeper batch and, when the list is not empty, sends you the list. You do not rewrite chapters.

1. Take the `--stale` list for your chapter: the QA IDs added or changed since the sync sha, and which of them the chapter does not cite yet. Read only those catalogue rows (`git diff <sha> -- docs/qa/catalogue/<screen>.md` shows what changed).
2. Edit only the sections that carry those IDs (the hidden `<!-- QA-... -->` comments find them). A new row gets a step, a table row or a sentence in the right section, in the voice and key notation of sections 3 to 5. A changed row changes only the words it touches. Keep the section 10 word cap; if the chapter is at the cap, shorten something the rows do not touch.
3. If the picture of a screen changed (`python3 tools/manual_images.py --check` lists the screenshots whose bytes changed), reread the text around that picture. New pictures get a line in `docs/manual/images.txt`.
4. Anything you cannot support from a row, spec or file stays a `<!-- TODO: ask: ... -->`.
5. Set the chapter's marker to the current `dev` sha (`git rev-parse --short dev`), but only after every listed ID has been dealt with. Then run `python3 tools/manual_check.py --stale` again (the chapter must be gone from the list) and `python3 tools/manual_llms.py` (commit `llms.txt` and `llms-full.txt`; CI fails when they are stale).

## 14. What never goes in

- Anything from the official firmware: decompiler output, symbol names, string dumps, copied text, logos, artwork, photographs or vendor diagrams, even redrawn. Describe the original firmware's behaviour only as a fact the catalogue or the official-manual map records, with its evidence. "Polyend" and "Tracker" appear only to say what the OS runs on or is compatible with.
- Anything from Polyend's manual beyond paraphrased facts with a section reference in the map. Its text, its pictures and its page layout are all out. Its approach (generous pictures next to the steps) is inspiration only, and our pictures are always our own simulator screenshots and generated SVGs.
- A fixed comparison heading or phrase, such as "Coming from the official firmware". Say what is new or different in the chapter's own opening prose (section 3).
- Text copied or closely paraphrased from any other manual. Describe a style; do not reuse sentences or drawings.
- Personal data: home paths, e-mail addresses, device serials, private notes, names of testers.
- Source code, internal identifiers (action names, function names, file paths of code), flash or CPU numbers, build instructions (those are in `docs/`), roadmap promises ("will come in the next version"). Say what the OS does now.
- Claims without a catalogue row, spec or file behind them. No guessing, no "probably", no "should".
- Marketing words and comparisons with other products.
- Words: "simply", "just", "easy", "easily", "obviously", "please", "utilize", "leverage". Em dashes. Emoji.
- Hardware-risk instructions beyond `install.md` (flashing, boot recovery, opening the unit). Link `install.md`.
- Invented keys, invented screen labels, or key forms other than section 4.
