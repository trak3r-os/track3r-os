# tests/ptfx: our OS-CEPTFX S0 test plugins (sources and sidecars)

**What is a format fact here, and where it comes from.** The `.ptfx` format facts — the 472-byte header's field
offsets, the flags, the 32-byte parameter layout (label[8], min/max/default/step f32, unit[6], display type,
pad), the payload = linked image at base 0 with no relocation table and `.bss` after `align4(code_size)`, the
three entry signatures, the host-table field order and size, the list strings inside the payload (u16 offset +
u8 count, up to 16 items of up to 10 chars), and the Cortex-M4F hard-float PIC no-libc build — are stated in
`docs/specs/OS-CEPTFX.md` section 2.1, which cites `tracker-ce@7fd6c2c:editor-fx-sdk/tracker_plugin.h` and
`ptfx_builder.py` as its evidence. Our files restate those facts in our own words: `ptfx_plugin_abi.h` asserts
the offsets it uses at compile time, and `tools/ptfx_pack.py` builds the payload from the same facts with its own
link layout, its own section names and a JSON sidecar for the header. No tracker-ce text or code is copied; the
SDK itself is only fetched, hash-pinned, into untracked `build-fixtures/ptfx-sdk/` to build the six examples.

The eleven test plugins of `docs/specs/OS-CEPTFX.md` 5 (S0), written against our own small ABI header
`ptfx_plugin_abi.h`. Each `NAME.c` has a `NAME.json`
sidecar with the header fields the packer writes (name, author, flags, scratch, parameters); the entry
offsets come from `nm` after the link, as in spec 2.1.

`tools/ptfx_pack.py` cross-compiles them for the Cortex-M4F of the MK66 (flags of spec 2.1) and writes the
`.ptfx` files into `build-fixtures/ptfx/` — nothing built is committed (`.gitignore` has `build*/`):

```
python3 -I tools/ptfx_pack.py tests/ptfx/gain.c tests/ptfx/gain.json -o build-fixtures/ptfx/gain.ptfx
python3 -I tools/ptfx_fixtures.py        # all of ours plus the six SDK examples (build-fixtures/ptfx/)
python3 -I tools/ptfx_inspect.py build-fixtures/ptfx/gain.ptfx
```

The plugins, and what they are for (the QEMU / unit expectations of spec 5):

| Plugin | Kind | Behaviour |
| --- | --- | --- |
| `gain` | effect | integer gain, exact (int16 x int, saturating) |
| `gen_ramp` | generator | fills `out[0]` with a ramp / saw / square (a list parameter) |
| `extra` | effect | `set_extra_samples(pre, post)` from init and every set_param |
| `recorder` | effect | logs every call's `samples`, NULL in-entries and `channels` into the scratch |
| `hang` | effect | never returns (timeout) |
| `fault_scs` | effect | reads SCB->CPUID unprivileged (fault) |
| `fault_addr` | effect | writes an unmapped address (fault) |
| `cpsid` | effect | `cpsid i`, then loops (the mask has no effect unprivileged) |
| `stackhog` | effect | deep recursion (stack overflow or fault) |
| `overcount` | effect | returns `samples + 1` (the run fails) |
| `clobber` | effect | overwrites r4-r11, s16-s31 and FPSCR and returns |
