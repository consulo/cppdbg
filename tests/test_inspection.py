#!/usr/bin/env python3
"""End-to-end test for cppdbg M3: threads / stackTrace / scopes / variables.

Drives cppdbg through:
  initialize -> launch -> setBreakpoints on hello.cpp:9 (after `int z = add(x, y);`)
  -> configurationDone -> stopped(entry) -> continue
  -> stopped(breakpoint) -> threads -> stackTrace -> scopes -> variables
  -> assert that x, y, z appear in Locals with sensible values
  -> continue -> terminated.

Usage:
    python tests/test_inspection.py <cppdbg.exe> <hello.exe> <hello.cpp>
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
                           clientID="m3-test", linesStartAt1=True,
                           columnsStartAt1=True, pathFormat="path")
        assert r["success"], r
        client.wait_event("initialized")

        r = client.request("launch", timeout=30, program=hello_exe)
        assert r["success"], r

        # Stop AFTER `int z = add(x, y);` (line 8) so x, y, z all exist.
        r = client.request("setBreakpoints",
                           source={"path": hello_src},
                           breakpoints=[{"line": 9}])
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
        print(f"[test] stopped at user breakpoint, thread={thread_id}")

        # Threads
        r = client.request("threads")
        assert r["success"], r
        threads = r["body"]["threads"]
        assert any(t["id"] == thread_id for t in threads), threads
        print(f"[test] threads: {[t['name'] for t in threads]}")

        # Stack trace
        r = client.request("stackTrace", threadId=thread_id)
        assert r["success"], r
        frames = r["body"]["stackFrames"]
        assert len(frames) >= 1, frames
        top = frames[0]
        print(f"[test] top frame: {top['name']} at {top.get('source', {}).get('path')}:{top.get('line')}")
        assert "main" in top["name"], top

        # Scopes
        r = client.request("scopes", frameId=top["id"])
        assert r["success"], r
        scopes = r["body"]["scopes"]
        assert len(scopes) >= 1, scopes
        locals_scope = next(s for s in scopes if s["name"].lower() == "locals")
        print(f"[test] scopes: {[s['name'] for s in scopes]}")

        # Variables — must contain x, y, z with the expected literals.
        r = client.request("variables",
                           variablesReference=locals_scope["variablesReference"])
        assert r["success"], r
        vars_ = {v["name"]: v for v in r["body"]["variables"]}
        print(f"[test] locals: {[(n, v.get('value')) for n, v in vars_.items()]}")
        for name, expected in (("x", "2"), ("y", "3"), ("z", "5")):
            assert name in vars_, f"missing local {name!r} in {list(vars_)}"
            v = vars_[name]
            assert expected in v["value"], (
                f"local {name}: expected value containing {expected!r}, got {v['value']!r}"
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
