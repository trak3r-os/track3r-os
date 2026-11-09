<!-- manual-sync: <sha>: the repository commit this chapter was last brought in step with (tools/manual_check.py --stale); set it to the current dev sha when you finish or update the chapter -->
<!--
Chapter template. Rules live in .claude/skills/manual-writing/SKILL.md; read it first, starting with section 0 (explain things).
Keep the headings and their order. Delete this comment and every <...> placeholder.
Depth tier (skill section 0): deep, medium or brief. A brief chapter (Sample edit, Sample load, File) leaves out "How it works" and "Try it".
Sync (skill section 15): a sync marker (the comment shown under "How it works" below) right below the heading of every section that states behaviour; "How it works" and "Try it" must have one; then run tools/manual_check.py --ack <chapter>#<section> (or --ack-path <file> for a file many sections cite).
Word cap: skill section 10. Key forms: `Shift` + `F3`, hold `Instr`, pad 5.
IDs: hidden comment at the end of the step they prove, and the Ref column of the Keys table only.
Accessibility (skill section 8): alt text on every image, no meaning by colour alone, headings never skip a level, tables have a header row, link text names its target.
-->
# <Screen name>

Status: draft

<Opening, no heading: 2 to 4 sentences for someone who already knows the official firmware. Lead with what this screen does differently or adds, and which keys moved. For a screen the official manual does not describe, say in the first sentence what it is for. Never a fixed phrase such as "Coming from the official firmware", never a heading of its own. Define or link any term a newcomer would not know.>

## What it is for

<2 to 4 sentences: what you do here, when you come here, which button gets you here. Brief chapters stop at "what it's for" and go on to the screen and the tasks.>

## How it works
<!-- sync: src=<path>,<path> spec=<docs/specs/...> qa=<QA-ID>,<QA-ID> -->

<Deep and medium chapters only (skill section 7). The model in plain words: what is live, what is stored, what ends it. Start from something the reader knows, then give one concrete musical example ("Hold pad 3 while the pattern plays: the track repeats its last step until you let go."). Deep chapters split this into `###` sub-concepts, one idea each, each with its own example and picture.>

![<Concept>: <what is shown>, <what to notice>.](img/concept_<name>.svg)

## Try it
<!-- sync: src=<path>,<path> qa=<QA-ID>,<QA-ID> -->

<Deep and medium chapters only: a walk-through of 5 to 7 numbered steps that uses the idea above. Each step is one action. The last step says what you hear or see. The walk-through is also the simulator scenario `sim/scenarios/manual/<screen>_walk.txt`, and its snaps are the pictures next to the steps.>

1. <One action.>
2. <One action.>

   ![<Screen>: <what is shown>, <what to notice>.](img/<screen>_walk_2.png)

3. <One action, and what you hear or see.>

## The screen at a glance

![<Screen>: <what is shown>, <what to notice>.](img/<screen>_<state>.png)

| Area | What it shows |
|---|---|
| Header | <...> |
| Main area | <...> |
| Info line | <...> |
| Key row | `F1` ... `F8` and what each does here |
| Shift row | the row drawn while `Shift` is held |

## More tasks

### <Goal, for example "Name a track">

Start on the <Screen> screen. <Put the cursor on ...>

1. <One action.> <!-- QA-XXX-001 -->
2. <One action.>
3. <One action.>

You should see <what is on screen>. <What is stored or changed.> <!-- QA-XXX-002 -->

### <Another goal>

<3 to 8 tasks in all. Group catalogue rows by goal. Explain why you would do it when the goal is not obvious.>

## Keys on this screen

<Reference only, never the body of the chapter. The first block is generated: `python3 tools/manual_tables.py` fills it with the key row (F1 to F8) and the Shift row of the screen from the firmware's key-row tables, and `--check` (CI) fails when it is stale. Never edit between the markers. A screen that `SCREENS` in tools/manual_tables.py does not list is added there first. The table after the block is hand-written: what each key does, and the keys that are not on the key row (pads, encoder, arrows, Copy, Rec, Delete). The Ref column lives there.>

<!-- manual-tables: keys screen=<name> -->
<!-- /manual-tables -->

| Key | What it does | Ref |
|---|---|---|
| `F1` (**LABEL**) | <what happens> | QA-XXX-001 |
| `Shift` + `F1` (**LABEL**) | <what happens> | QA-XXX-002 |
| the encoder | <what it changes> | QA-XXX-003 |

## Good to know

- <A limit, a refusal, a surprise: one line each.>
- Works as the official manual describes. / Works the same as on the original firmware. / New in track3r-os. / Not checked against the original firmware. (plain-words labels, section 9 of the skill)

## See also

- [<Chapter>](<chapter>.md): <why>

<!-- Open rows, not written: QA-XXX-nnn, ... -->
