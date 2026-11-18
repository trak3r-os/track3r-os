# Contributing to track3r-os

Thank you for helping. track3r-os is an unofficial community operating system for the Tracker hardware. [CLAUDE.md](CLAUDE.md) is the working agreement for people and for the AI agents that maintain the project; this page is the short version for someone sending a change.

## The rule that comes first

This is the first hard rule of CLAUDE.md, word for word:

> **Never add code, assets or reverse-engineering material from the official firmware.** No decompiler output, symbol maps, string dumps or copied code. Behaviour of the original firmware may be described as a fact ("observed behaviour of the official firmware, for compatibility"), with evidence. No Polyend logos or artwork. "Polyend" and "Tracker" are used only to describe compatibility.

A change that breaks this rule is closed, whatever else it does.

## Bugs and ideas

Use the issue forms. A bug report needs the BUILD ID (Config, INFO, or the `version` command of the USB diag console), the steps, what happened, what you expected and the card you used. Ideas go to the project backlog as IDEA rows, and the maintainer decides which of them are taken up.

## Sending a change

1. Branch from `dev` and open the pull request against `dev`. Never against `main`: `main` moves only by fast-forward to a green, reviewed `dev`.
2. One change per pull request. A fix does not carry a refactor; note the refactor in the description instead.
3. Commit messages read `type(area): summary [ID]`, for example `fix(seq): the swing offset of an odd step [OS-123]`. The body says why the change is made. Use the ID of the issue or backlog row when there is one.
4. `tools/ci.sh host` passes with no new warning (first-party code builds with warnings as errors). How to set up the toolchain is in [docs/toolchain.md](docs/toolchain.md). Pure logic (parsers, format round trips, DSP maths, sequencer arithmetic) comes with a host test.
5. Real-time code (audio render, interrupt handlers and what they call) does no allocation, no file I/O, no blocking and no unbounded loop. A saved file format never changes layout, size or meaning without a version bump, a loader for the old version and a round-trip test.
6. A claim about how the original firmware behaves cites its evidence. Mark a behaviour as `verified parity` (with the evidence), `new Community OS behaviour` or `open research question`.
7. Update the docs the change needs in the same pull request (QA catalogue rows, the UI map, the manual chapter).

## Licence and sign-off

Your contribution is licensed under **GPL-3.0-or-later**, like the rest of the repository. The one exception is the module SDK under `module/sdk/`, which is **MIT**: a change there is MIT.

Every commit of a contribution from outside the project carries a sign-off under the [Developer Certificate of Origin](https://developercertificate.org/): it says you wrote the change or have the right to submit it under the project's licence. Add it with:

```
git commit -s
```

This adds a `Signed-off-by: Your Name <you@example.com>` line. The address becomes part of the public history; a GitHub no-reply address is fine. Code taken from another project needs a compatible licence and an entry in [THIRD_PARTY.md](THIRD_PARTY.md).

## How pull requests are handled

Every pull request gets an automated review (a `firmware-reviewer` pass), posted as the review. The project's maintainer agents may merge a Level A or B change (a typo, docs, a tool, a normal feature or fix) when the review approves it and, after the reviewer found the diff safe, a local `tools/ci.sh` run is green. There is no GitHub CI: run `tools/ci.sh host` yourself before you open the pull request.

Some changes wait for the human maintainer, whatever the review says: persistent formats, update, boot and storage code, interrupt handlers and the audio path, and published ABIs (the module SDK, the file formats). Expect those to take longer, and expect questions about hardware verification, because a host test is not a test on the unit.
