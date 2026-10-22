#!/usr/bin/env python3
"""Bake the setup page into the firmware as a C string.

The board serves this page itself, from a soft access point, at a moment when
it is connected to nothing: it cannot fetch a stylesheet, a font or an icon from
anywhere. So the page is one self-contained file (tools/setup-page/index.html)
and it is compiled in, rather than read from a filesystem that a first flash
would have to write separately.

    python3 tools/gen-setup-page.py            write firmware/include/setup_page.h
    python3 tools/gen-setup-page.py --check    exit 1 if the file is out of date
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "tools" / "setup-page" / "index.html"
OUT = ROOT / "firmware" / "include" / "setup_page.h"

# A raw string literal needs a delimiter the page does not contain; "HTML" is
# checked rather than assumed, so a page that ever grows ")HTML\"" fails here
# instead of in the compiler.
DELIM = "HTML"


def emit(html: str) -> str:
    if f'){DELIM}"' in html:
        sys.exit(f"gen-setup-page: the page contains ){DELIM}\" — pick another delimiter")
    head = '''// Baked by tools/gen-setup-page.py. Do not edit.
//
// The captive portal's page, compiled in: the board serves it from its own soft
// access point, before it is on any network, so nothing may be fetched.
//
// Source of truth: tools/setup-page/index.html
#pragma once

namespace magpie {

constexpr const char kSetupPage[] = R"%s(
''' % DELIM
    tail = ''')%s";

}  // namespace magpie
''' % DELIM
    return head + html + tail


def main() -> int:
    html = SRC.read_text()
    text = emit(html)
    if "--check" in sys.argv:
        if OUT.exists() and OUT.read_text() == text:
            print("setup_page.h is current", file=sys.stderr)
            return 0
        print("setup_page.h is out of date", file=sys.stderr)
        return 1
    OUT.write_text(text)
    print(f"wrote {OUT.relative_to(ROOT)} ({len(html)} bytes of page)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
