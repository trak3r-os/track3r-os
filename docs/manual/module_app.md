<!-- manual-sync: 571f81aa -->
# App pages
<!-- sync: src=app/src/module_ui.cpp,app/src/screens/modules_screen.cpp qa=QA-APP-001,QA-APP-002,QA-APP-003,QA-APP-004,QA-APP-009 -->

Status: draft

Every app, such as MIDI Monitor or Rhythm Gen, draws its own page, and the OS keeps the same frame around it: the strip on top, an info line, and a key row with `F8` (**BACK**) last. `Play` is always the transport's, and a screen button leaves the app for its own screen. This is new in track3r-os, and so are apps. <!-- og: new --> What each app does is in [Modules](modules.md); this page covers what every app page shares.

## What it is for

Use this page to find your way around any app. You open an app from the **MODULES** screen with `F1`, or from Pattern: `F7` (**TOOLS**), then `F1` runs the app. You leave it with `F8`.

## The screen at a glance

| Area | What it shows |
|---|---|
| Strip | `APP`, the transport, `NAME midimon`, `STATE RUNNING` and the CPU and memory readout. If the unit stopped the app, `STATE STOPPED`. |
| Main area | The app's own pane, such as the rows of **MIDI MONITOR** at the bottom. If the unit stopped the app, **APP STOPPED**. |
| Info line | `the app has the input` on the left and `F8: back` on the right. |
| Key row | The app's own labels, `F8` **BACK** last. An app that declares none reads `--` on `F1` to `F7`. |

<!-- QA-APP-001 -->

## More tasks

### Go back to where you opened the app

Start on an app page.

1. Press `F8` (**BACK**), or `F7` and `F8` together. <!-- QA-APP-002 -->

You return to where you came from. An app opened from Pattern brings you back to Pattern, with the cursor and the block as they were. An app opened from **MODULES** brings you back to **MODULES**. The app is ended, and there is one level of return.

### Leave for another screen

1. Press a screen button, such as `Song` or `Pattern`. <!-- QA-APP-003 -->

The screen of the button opens and the app is ended. The app never hears the press or the release. All eleven screen buttons do the same.

### Start and stop the transport

1. Press `Play`. The transport starts or stops, and the transport mark in the strip follows. <!-- QA-APP-009 -->

The page stays and the app does not see `Play`. `Rec`, the pads and the other keys still reach the app.

### Read the help page

1. Press `Shift` + `Config`. **HELP: MODULE APP** opens over the app, page 15 of 15, and the key row reads `F8` **CLOSE**. <!-- QA-APP-004 -->
2. Press `F8`. The app page is back, and the app kept running.

The page lists `F8` BACK, `Play`, `Shift` + `Config`, `Shift` + `Up` and `Down`, `Shift` + encoder, `Shift` + `Delete` and `Shift` + `F8`, which does nothing. While it is up, the app gets the press and release of `Shift` only, and nothing else.

### Edit the parameters of an older app

An app built for a module ABI (the version of the interface between the OS and a module) below 1.10 may declare parameters. The OS then draws a **PARAMETERS** column on the left. The example app Pads is one. Start on such an app.

1. Press `Shift` + `Down` and `Shift` + `Up` to move the selected row. <!-- QA-APP-005 -->
2. Turn the encoder with `Shift` held to change the value of the row. The app hears the change. <!-- QA-APP-006 -->
3. Press `Shift` + `Delete` to put the row back to its default. <!-- QA-APP-007 -->

The app is given none of these keys. An app that declares no parameter, or is of ABI 1.10, has no such column. It gets every Shift key and uses them for its own coarse steps: in Rhythm Gen, `Shift` + encoder moves four steps a detent. <!-- QA-APP-008 -->

## Keys on this screen

| Key | What it does | Ref |
|---|---|---|
| `F8` (**BACK**), `F7` and `F8` together | Leave the app for where it was opened. | QA-APP-002 |
| A screen button | Ends the app and opens that screen. | QA-APP-003 |
| `Shift` + `Config` | Opens the help page of the app page. | QA-APP-004 |
| `Shift` + `Up`, `Shift` + `Down` | Select a row of the OS column of parameters. | QA-APP-005 |
| `Shift` + encoder | Edits the selected parameter. | QA-APP-006 |
| `Shift` + `Delete` | Puts the selected parameter back to its default. | QA-APP-007 |
| Any Shift key, on an app without the column | Goes to the app. | QA-APP-008 |
| `Play` | The transport's. | QA-APP-009 |

## Good to know

- The parameter column exists only for an app below ABI 1.10 that declares a parameter. Pads is the only one shipped; the simulator's card does not hold it.
- What a press at the first or last row of the column does is not checked.

## See also

- [Modules](modules.md): installing modules and what each bundled app does.
- [Pattern](pattern.md): the **TOOLS** list that launches an app.
