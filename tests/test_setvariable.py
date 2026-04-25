#!/usr/bin/env python3
"""End-to-end test for cppdbg setVariable.

initialize -> launch -> setBreakpoints on hello.cpp:9 (after `int z = add(x, y);`)
-> configurationDone -> stopped(entry) -> continue -> stopped(breakpoint)
-> read x (must be "0n2") -> setVariable x = 99 -> read x again
(must be "0n63" hex / "0n99" decimal) -> continue -> terminated.

Usage:
    python tests/test_setvariable.py <cppdbg.exe> <hello.exe> <hello.cpp>
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
                           clientID="setvar-test", linesStartAt1=True,
                           columnsStartAt1=True, pathFormat="path")
        assert r["success"], r
        assert r["body"].get("supportsSetVariable") is True, r
        client.wait_event("initialized")

        r = client.request("launch", timeout=30, program=hello_exe)
        assert r["success"], r
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
        tid = ev["body"]["threadId"]

        r = client.request("stackTrace", threadId=tid, levels=1)
        assert r["success"], r
        frame_id = r["body"]["stackFrames"][0]["id"]

        r = client.request("scopes", frameId=frame_id)
        assert r["success"], r
        locals_ref = next(s for s in r["body"]["scopes"]
                          if s["name"].lower() == "locals")["variablesReference"]

        r = client.request("variables", variablesReference=locals_ref)
        assert r["success"], r
        before = {v["name"]: v["value"] for v in r["body"]["variables"]}
        print(f"[test] before: x={before.get('x')!r} y={before.get('y')!r} z={before.get('z')!r}")
        assert "2" in before["x"], before
        assert "3" in before["y"], before
        assert "5" in before["z"], before

        r = client.request("setVariable",
                           variablesReference=locals_ref,
                           name="x", value="99")
        assert r["success"], r
        new_val = r["body"]["value"]
        print(f"[test] setVariable x=99 -> response value={new_val!r}")
        assert "99" in new_val, new_val

        # Re-read locals to confirm the write took.
        r = client.request("variables", variablesReference=locals_ref)
        assert r["success"], r
        after = {v["name"]: v["value"] for v in r["body"]["variables"]}
        print(f"[test] after:  x={after.get('x')!r} y={after.get('y')!r} z={after.get('z')!r}")
        assert "99" in after["x"], after
        # y and z should be unchanged.
        assert "3" in after["y"] and "5" in after["z"], after

        r = client.request("continue", threadId=tid)
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
