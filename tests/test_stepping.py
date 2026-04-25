#!/usr/bin/env python3
"""End-to-end test for cppdbg M4: next / stepIn / stepOut.

Drives cppdbg through:
  initialize -> launch -> setBreakpoints on hello.cpp:8 (the call to add)
  -> configurationDone -> stopped(entry) -> continue
  -> stopped(breakpoint) at line 8
  -> stepIn  -> stopped(step) inside add() (top frame should be `add`)
  -> stepOut -> stopped(step) back in main, line >= 8
  -> next    -> stopped(step), line advances
  -> continue -> terminated.

Pause is not exercised here (it's timing-sensitive); the engine method is
covered indirectly by the SetInterrupt path being compiled and linked.

Usage:
    python tests/test_stepping.py <cppdbg.exe> <hello.exe> <hello.cpp>
"""

from __future__ import annotations


import os
import sys

from _dap_client import DapClient


def top_frame(client: "DapClient", thread_id: int) -> dict:
    r = client.request("stackTrace", threadId=thread_id, levels=1)
    assert r["success"], r
    return r["body"]["stackFrames"][0]


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
                           clientID="m4-test", linesStartAt1=True,
                           columnsStartAt1=True, pathFormat="path")
        assert r["success"], r
        client.wait_event("initialized")

        r = client.request("launch", timeout=30, program=hello_exe)
        assert r["success"], r

        # Stop at the call site `int z = add(x, y);` (line 8).
        r = client.request("setBreakpoints",
                           source={"path": hello_src},
                           breakpoints=[{"line": 8}])
        assert r["success"], r
        assert r["body"]["breakpoints"][0]["verified"], r

        r = client.request("configurationDone")
        assert r["success"], r

        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "entry", ev

        r = client.request("continue", threadId=ev["body"]["threadId"])
        assert r["success"], r

        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "breakpoint", ev
        thread_id = ev["body"]["threadId"]
        bp_frame = top_frame(client, thread_id)
        print(f"[test] stopped at BP: {bp_frame['name']} line={bp_frame['line']}")
        assert bp_frame["line"] == 8, bp_frame

        # Step into add()
        r = client.request("stepIn", threadId=thread_id)
        assert r["success"], r
        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "step", ev
        in_frame = top_frame(client, thread_id)
        print(f"[test] stepIn -> {in_frame['name']} line={in_frame['line']}")
        assert "add" in in_frame["name"], (
            f"expected to be in add(), got {in_frame['name']!r}"
        )

        # Step out: back to main, at or just past the call site.
        r = client.request("stepOut", threadId=thread_id)
        assert r["success"], r
        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "step", ev
        out_frame = top_frame(client, thread_id)
        print(f"[test] stepOut -> {out_frame['name']} line={out_frame['line']}")
        assert "main" in out_frame["name"], out_frame
        assert out_frame["line"] >= 8, out_frame

        # next (step over) — line should advance.
        prev_line = out_frame["line"]
        r = client.request("next", threadId=thread_id)
        assert r["success"], r
        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "step", ev
        next_frame = top_frame(client, thread_id)
        print(f"[test] next    -> {next_frame['name']} line={next_frame['line']}")
        assert "main" in next_frame["name"], next_frame
        assert next_frame["line"] > prev_line, (
            f"expected line > {prev_line}, got {next_frame['line']}"
        )

        r = client.request("continue", threadId=thread_id)
        assert r["success"], r
        client.wait_event("terminated", timeout=30)

        r = client.request("disconnect", terminateDebuggee=False)
        assert r["success"], r
    finally:
        client.close()

    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
