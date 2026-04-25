#!/usr/bin/env python3
"""End-to-end test for cppdbg M2: source-line breakpoint.

Drives cppdbg through: initialize -> launch -> setBreakpoints on
hello.cpp line 7 -> configurationDone -> stopped(entry) -> continue ->
stopped(breakpoint) -> continue -> terminated.

Usage:
    python tests/test_breakpoint.py <cppdbg.exe> <hello.exe> <hello.cpp>
"""

from __future__ import annotations


import os
import sys

from _dap_client import DapClient


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__)
        return 2
    cppdbg_exe, hello_exe, hello_src = argv[1], argv[2], argv[3]
    for path in (cppdbg_exe, hello_exe, hello_src):
        if not os.path.isfile(path):
            print(f"not found: {path}", file=sys.stderr)
            return 2

    hello_src = os.path.abspath(hello_src)

    client = DapClient(cppdbg_exe)
    try:
        r = client.request("initialize", adapterID="cppdbg",
                           clientID="bp-test", linesStartAt1=True,
                           columnsStartAt1=True, pathFormat="path")
        assert r["success"], r
        client.wait_event("initialized")

        r = client.request("launch", timeout=30, program=hello_exe)
        assert r["success"], r

        # Set a breakpoint on line 7 ("int y = 3;").
        r = client.request(
            "setBreakpoints",
            source={"path": hello_src},
            breakpoints=[{"line": 7}],
        )
        assert r["success"], r
        bps = r["body"]["breakpoints"]
        assert len(bps) == 1, bps
        assert bps[0]["verified"], f"breakpoint not verified: {bps[0]}"
        print(f"[test] BP verified at line {bps[0].get('line')}")

        r = client.request("configurationDone")
        assert r["success"], r

        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "entry", ev
        print("[test] stopped at entry as expected")

        r = client.request("continue", threadId=0)
        assert r["success"], r

        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "breakpoint", ev
        print("[test] stopped at breakpoint as expected")

        r = client.request("continue", threadId=0)
        assert r["success"], r

        client.wait_event("terminated", timeout=30)

        r = client.request("disconnect", terminateDebuggee=False)
        assert r["success"], r
    finally:
        try:
            client.close()
        except Exception:
            pass

    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
