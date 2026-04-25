#!/usr/bin/env python3
"""End-to-end test for cppdbg evaluate.

Stops at hello.cpp:9 (after `int z = add(x, y);`) and exercises the
evaluate request in three contexts:

  - "repl"  : "x"          -> primitive, value contains "2"
              "x + y * 2"  -> primitive, value contains "8"
              "argc"       -> primitive, value contains "1"
  - "watch" : "argv"       -> compound; can drill via variablesReference
  - "hover" : "z"          -> compound preferred path, still gives "5"

Usage:
    python tests/test_evaluate.py <cppdbg.exe> <hello.exe> <hello.cpp>
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
                           clientID="evaluate-test", linesStartAt1=True,
                           columnsStartAt1=True, pathFormat="path")
        assert r["success"], r
        assert r["body"].get("supportsEvaluateForHovers") is True, r
        client.wait_event("initialized")

        r = client.request("launch", timeout=30, program=hello_exe)
        assert r["success"], r
        r = client.request("setBreakpoints",
                           source={"path": hello_src},
                           breakpoints=[{"line": 9}])
        assert r["success"], r
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

        # repl: primitive expressions.
        for expr, expected in [("x", "2"), ("y", "3"), ("z", "5"),
                               ("x + y * 2", "8"), ("argc", "1")]:
            r = client.request("evaluate", expression=expr,
                               frameId=frame_id, context="repl")
            assert r["success"], (expr, r)
            value = r["body"]["result"]
            print(f"[test] repl  {expr!r} -> {value!r} (type={r['body'].get('type')!r})")
            assert expected in value, f"{expr}: expected {expected!r} in {value!r}"

        # watch: ask for compound rendering. argv is char**.
        r = client.request("evaluate", expression="argv",
                           frameId=frame_id, context="watch")
        assert r["success"], r
        argv_value = r["body"]["result"]
        argv_ref = r["body"].get("variablesReference", 0)
        print(f"[test] watch argv -> {argv_value!r} ref={argv_ref}")
        assert argv_ref > 0, (
            f"expected compound variablesReference for argv, got {argv_ref}"
        )

        # Drill in: argv is a char** so children expose it as a pointer.
        r = client.request("variables", variablesReference=argv_ref)
        assert r["success"], r
        children = r["body"]["variables"]
        print(f"[test] argv children: {[(v['name'], v.get('value','')[:40]) for v in children]}")
        assert children, "expected at least one child variable for argv"

        # hover: same expression, different context — we still produce a value.
        r = client.request("evaluate", expression="z",
                           frameId=frame_id, context="hover")
        assert r["success"], r
        print(f"[test] hover z -> {r['body']['result']!r} type={r['body'].get('type')!r}")
        assert "5" in r["body"]["result"], r

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
