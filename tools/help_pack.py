#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-FLASHDIET A6: pack the help pages (app/help/pages.txt) into the help blob.

    help_pack.py --pages app/help/pages.txt --out <build>/gen/help_blob.cpp

The authoring file app/help/pages.txt (made once from the old table in app/src/help_text.cpp) holds the
pages in this format, one page after the other:

    == <title>     starts a page
    | <text>       a paragraph line (key nullptr)
    (empty line)   a blank line (key "", text "")
    <key>\t<text>  a key line
    <key>\t        a section label (text "")
    \t<text>       a continuation line (key "")

The pages are validated exactly as tests/help_test.cpp validates the old table -- ASCII only, the key and
text widths of kHelpKeyCols / kHelpTextCols (read from app/include/o3os/app/help_text.h), a paragraph
line at most kHelpKeyCols + kHelpTextCols -- plus the page count against kHelpPages and the page buffer's
limits (kMaxLines and the text bytes of HelpPageBuf, read from app/include/o3os/app/help_blob.h). Then they are packed with tools/deflate_pack.py's
deterministic encoder (never zlib: the reproducible-image gate must not depend on the build machine's
zlib). The emitted C++ defines, in namespace o3os::app (declared in app/include/o3os/app/help_blob.h):

    kHelpBlobZ / kHelpBlobZSize / kHelpBlobRawSize   the raw-deflate stream (deflate_pack.emit_cpp, word
                                                    aligned), over the inflated text below
    kHelpBlobPageOffsets[]                          kHelpBlobPages entries: each page's offset in the
                                                    inflated text
    kHelpBlobTitles[]                               the page titles (in flash, not in the stream, so a
                                                    page's strings fit the 4 KiB page buffer)
    kHelpBlobPages

The inflated text is every page's lines concatenated, one '\\n' per line (the same encoding as the file
above, minus the "== <title>" lines); a page's block starts at its offset, so the decoder skips to the
offset and inflates just that page (core::Inflater::skip). Stdlib only.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deflate_pack  # noqa: E402  (tools/deflate_pack.py, OS-FLASHDIET A2)

REPO = Path(__file__).resolve().parent.parent
HELP_TEXT_H = REPO / "app/include/o3os/app/help_text.h"
HELP_BLOB_H = REPO / "app/include/o3os/app/help_blob.h"

# The page buffer's layout as the packer sees it: it runs on the 64-bit build host, where a pointer is 8
# bytes and a HelpLine 16; on the 32-bit target the buffer is roomier, so the host's numbers are the bound.
# The HOST_* are host sizes only -- the target's are smaller (help_blob.h computes its own kTextBytes).
HOST_PTR = 8
HOST_HELP_LINE = 16
HOST_COUNT = 2


def _header_const(path: Path, name: str) -> int:
    m = re.search(r"\b%s\s*=\s*(\d+)" % re.escape(name), path.read_text())
    if not m:
        raise SystemExit("help_pack: %s has no %s" % (path, name))
    return int(m.group(1))


def parse_pages(text: str):
    """The pages of a pages.txt: [(title, [(key or None, text), ...]), ...]. Raises ValueError on a bad line."""
    pages = []
    title = None
    lines = []
    rows = text.split("\n")
    if rows and rows[-1] == "":
        rows.pop()  # the file's final newline, not a blank line
    for lineno, raw in enumerate(rows, 1):
        try:
            if raw.startswith("== "):
                if title is not None:
                    pages.append((title, lines))
                title = raw[3:]
                if not title:
                    raise ValueError("a page with an empty title")
                lines = []
            elif title is None:
                raise ValueError("a line before the first '== ' page")
            elif raw == "":
                lines.append(("", ""))  # a blank line
            elif raw.startswith("|"):
                t = raw[1:]
                if t.startswith(" "):
                    t = t[1:]
                lines.append((None, t))  # a paragraph line
            elif "\t" in raw:
                key, _, rest = raw.partition("\t")
                lines.append((key, rest))  # a key line, a section label or a continuation
            else:
                raise ValueError("neither a page, a paragraph, nor a key line: %r" % raw)
        except ValueError as e:
            raise ValueError("line %d: %s" % (lineno, e)) from None
    if title is not None:
        pages.append((title, lines))
    if not pages:
        raise ValueError("no page (no '== <title>' line)")
    return pages


def validate(pages, key_cols: int, text_cols: int, max_lines: int, text_bytes: int):
    """The failures of the pages against what help_test.cpp holds the old table to, plus the page buffer's
    limits: ASCII, the widths, at most max_lines lines a page, and the page's strings (each NUL terminated,
    plus the shared empty string) fitting the buffer's text. It also refuses what would make the packer and
    the decoder (flush_line in app/src/help_blob.cpp) disagree: a key starting with '|' (the packed line
    would start with '|' and decode as a paragraph) and a line starting with '== ' (a page title, not a
    key line)."""
    failures = []
    for pi, (title, lines) in enumerate(pages):
        where = "page %d (%r)" % (pi, title)
        for c in title:
            if not (32 <= ord(c) < 127) or c == "\t":
                failures.append("%s: the title is not ASCII printable text" % where)
                break
        if not lines:
            failures.append("%s: no line" % where)
        if len(lines) > max_lines:
            failures.append("%s: %d lines, over the page buffer's %d" % (where, len(lines), max_lines))
        compacted = 1  # the shared empty string at text[0]
        for n, (key, text) in enumerate(lines):
            where_line = "%s line %d" % (where, n + 1)
            if key is not None and key.startswith("|"):
                failures.append("%s: the key starts with '|', so the packed line would start with '|' and "
                                "flush_line (help_blob.cpp) would decode it as a paragraph, not a key line"
                                % where_line)
            if key is not None and key.startswith("== "):
                failures.append("%s: the line starts with '== ', which is a page title, not a key line"
                                % where_line)
            for part, what in ((key, "key"), (text, "text")):
                if part is None:
                    continue
                if "\t" in part:
                    failures.append("%s: the %s holds a tab" % (where_line, what))
                for c in part:
                    if not (32 <= ord(c) < 127):
                        failures.append("%s: the %s is not ASCII printable" % (where_line, what))
                        break
            if key is not None and len(key) >= key_cols:
                failures.append("%s: a key of %d characters, at least %d allowed"
                                % (where_line, len(key), key_cols))
            if key is not None and len(text) > text_cols:
                failures.append("%s: a text of %d characters, at most %d allowed"
                                % (where_line, len(text), text_cols))
            if key is None and len(text) > key_cols + text_cols:
                failures.append("%s: a paragraph of %d characters, at most %d allowed"
                                % (where_line, len(text), key_cols + text_cols))
            if key:
                compacted += len(key) + 1
            if text:
                compacted += len(text) + 1
        if compacted > text_bytes:
            failures.append("%s: %d bytes of strings, over the page buffer's %d"
                            % (where, compacted, text_bytes))
    return failures


def pack(pages):
    """The inflated text (every page's lines, one '\\n' each, titles left out) and each page's offset in it."""
    blocks = []
    offsets = []
    pos = 0
    for _title, lines in pages:
        offsets.append(pos)
        block = bytearray()
        for key, text in lines:
            if key is None:
                block += b"| " + text.encode("ascii") + b"\n"
            elif key == "" and text == "":
                block += b"\n"
            elif text == "":
                block += key.encode("ascii") + b"\t\n"
            elif key == "":
                block += b"\t" + text.encode("ascii") + b"\n"
            else:
                block += key.encode("ascii") + b"\t" + text.encode("ascii") + b"\n"
        pos += len(block)
        blocks.append(bytes(block))
    return b"".join(blocks), offsets


def _cpp_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit(pages, raw: bytes, offsets, z: bytes) -> str:
    """The generated help_blob.cpp: deflate_pack's emit_cpp for the stream, then the page index and the
    titles with external linkage in namespace o3os::app (declared in help_blob.h)."""
    stream = deflate_pack.emit_cpp("HelpBlobStream", raw, z)
    out = [
        "// Generated by tools/help_pack.py from app/help/pages.txt -- do not edit.",
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "#include <o3os/app/help_blob.h>",
        "",
        stream,
        "namespace o3os::app {",
        "",
        "// The packed help pages, as declared in help_blob.h: the deflate stream of the %d raw bytes" % len(raw),
        "// (every page's lines; the titles are below, in flash), the page index and the titles.",
        "extern const std::uint8_t* const kHelpBlobZ = kHelpBlobStream.bytes;",
        "extern const std::size_t kHelpBlobZSize = kHelpBlobStreamZSize;",
        "extern const std::size_t kHelpBlobRawSize = kHelpBlobStreamRawSize;",
        "extern const unsigned kHelpBlobPages = %d;" % len(pages),
        "extern const std::uint32_t kHelpBlobPageOffsets[%d] = {" % len(pages),
    ]
    for i in range(0, len(offsets), 8):
        out.append("    " + ", ".join("%du" % o for o in offsets[i:i + 8]) + ",")
    out.append("};")
    out.append("extern const char* const kHelpBlobTitles[%d] = {" % len(pages))
    for i in range(0, len(pages), 4):
        out.append("    " + ", ".join(_cpp_string(t) for t, _ in pages[i:i + 4]) + ",")
    out += [
        "};",
        "",
        "}  // namespace o3os::app",
        "",
    ]
    return "\n".join(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pages", type=Path, required=True, help="the authoring file (app/help/pages.txt)")
    ap.add_argument("--out", type=Path, required=True, help="where to write the generated help_blob.cpp")
    args = ap.parse_args(argv)

    key_cols = _header_const(HELP_TEXT_H, "kHelpKeyCols")
    text_cols = _header_const(HELP_TEXT_H, "kHelpTextCols")
    want_pages = _header_const(HELP_TEXT_H, "kHelpPages")
    max_lines = _header_const(HELP_BLOB_H, "kMaxLines")
    buf_size = _header_const(HELP_BLOB_H, "kSize")
    text_bytes = buf_size - HOST_PTR - HOST_HELP_LINE * max_lines - HOST_COUNT

    try:
        text = args.pages.read_text(encoding="ascii")
    except UnicodeDecodeError as e:
        print("help_pack: %s is not ASCII: %s" % (args.pages, e), file=sys.stderr)
        return 2
    if "\r" in text:
        print("help_pack: %s holds CR (the file is LF only)" % args.pages, file=sys.stderr)
        return 2
    try:
        pages = parse_pages(text)
    except ValueError as e:
        print("help_pack: %s: %s" % (args.pages, e), file=sys.stderr)
        return 2
    if len(pages) != want_pages:
        print("help_pack: %s: %d pages, but %s says kHelpPages = %d"
              % (args.pages, len(pages), HELP_TEXT_H, want_pages), file=sys.stderr)
        return 2
    failures = validate(pages, key_cols, text_cols, max_lines, text_bytes)
    if failures:
        for f in failures:
            print("help_pack: %s: %s" % (args.pages, f), file=sys.stderr)
        return 2

    raw, offsets = pack(pages)
    z = deflate_pack.deflate(raw)  # A2's deterministic encoder; never zlib
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(emit(pages, raw, offsets, z), encoding="utf-8", newline="\n")
    print("help_pack: %d pages, %d raw bytes -> %d stream bytes (%.1f%%), %d pages indexed"
          % (len(pages), len(raw), len(z), 100.0 * len(z) / max(1, len(raw)), len(offsets)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
