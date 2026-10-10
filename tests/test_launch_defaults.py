#!/usr/bin/env python3
"""Launch without stopOnEntry: first stop is the breakpoint, debuggee
stdout arrives as output events before exited.

Usage:
    python tests/test_launch_defaults.py <cppdbg.exe> <hello.exe> <hello.cpp>
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
                           clientID="launch-defaults-test", linesStartAt1=True,
                           columnsStartAt1=True, pathFormat="path")
        assert r["success"], r
        client.wait_event("initialized")

        r = client.request("launch", timeout=30, program=hello_exe)
        assert r["success"], r

        r = client.request(
            "setBreakpoints",
            source={"path": hello_src},
            breakpoints=[{"line": 7}],
        )
        assert r["success"], r
        assert r["body"]["breakpoints"][0]["verified"], r

        r = client.request("configurationDone")
        assert r["success"], r

        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "breakpoint", ev
        print("[test] first stop is the breakpoint")

        r = client.request("continue", threadId=ev["body"]["threadId"])
        assert r["success"], r

        events = client.drain_events("terminated", timeout=30)
        names = [e.get("event") for e in events]
        exited_at = names.index("exited")
        stdout = "".join(
            e["body"]["output"]
            for e in events[:exited_at]
            if e.get("event") == "output"
            and e["body"].get("category") == "stdout"
        )
        assert "hello 2 5" in stdout, (stdout, names)
        assert events[exited_at]["body"]["exitCode"] == 0, events[exited_at]
        print(f"[test] stdout before exited: {stdout.strip()!r}")

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
