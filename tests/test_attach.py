#!/usr/bin/env python3
"""End-to-end test for cppdbg M6: attach to a running process.

Spawns spin.exe (a long-running fixture), then drives cppdbg through:
  initialize -> attach(processId=spin.pid) -> setBreakpoints on
  spin.cpp:slow body line -> configurationDone -> stopped(entry) ->
  continue -> stopped(breakpoint) -> verify top frame is in slow() ->
  continue -> disconnect (without terminating spin.exe so we can reap
  it ourselves).

Usage:
    python tests/test_attach.py <cppdbg.exe> <spin.exe> <spin.cpp>
"""

from __future__ import annotations


import os
import subprocess
import sys
import time

from _dap_client import DapClient


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__)
        return 2
    cppdbg_exe, spin_exe, spin_src = argv[1], argv[2], argv[3]
    for path in (cppdbg_exe, spin_exe, spin_src):
        if not os.path.isfile(path):
            print(f"not found: {path}", file=sys.stderr)
            return 2

    spin_src = os.path.abspath(spin_src)

    spin = subprocess.Popen([spin_exe], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        # Wait for spin.exe to print readiness — guarantees it's past
        # process init and into slow() shortly after.
        line = spin.stdout.readline()
        if b"pid ready" not in line:
            print(f"unexpected output from spin: {line!r}", file=sys.stderr)
            return 1
        time.sleep(0.2)

        client = DapClient(cppdbg_exe)
        try:
            r = client.request("initialize", adapterID="cppdbg",
                               clientID="m6-test", linesStartAt1=True,
                               columnsStartAt1=True, pathFormat="path")
            assert r["success"], r
            client.wait_event("initialized")

            r = client.request("attach", timeout=30, processId=spin.pid)
            assert r["success"], r
            print(f"[test] attached to pid={spin.pid}")

            # Set BP on the assignment inside slow() — line 10 in spin.cpp
            # (`spin_iter = i;`). This will fire as the spin loop runs.
            r = client.request("setBreakpoints",
                               source={"path": spin_src},
                               breakpoints=[{"line": 10}])
            assert r["success"], r
            assert r["body"]["breakpoints"][0]["verified"], r
            print(f"[test] BP verified at line {r['body']['breakpoints'][0].get('line')}")

            r = client.request("configurationDone")
            assert r["success"], r

            ev = client.wait_event("stopped", timeout=30)
            # On attach the deferred stop is reported as 'entry' — we use
            # the same flag plumbing as launch.
            assert ev["body"]["reason"] == "entry", ev
            tid = ev["body"]["threadId"]
            print(f"[test] stopped(entry) after attach, thread={tid}")

            r = client.request("continue", threadId=tid)
            assert r["success"], r

            ev = client.wait_event("stopped", timeout=30)
            assert ev["body"]["reason"] == "breakpoint", ev
            bp_tid = ev["body"]["threadId"]
            print(f"[test] hit breakpoint inside slow(), thread={bp_tid}")

            r = client.request("stackTrace", threadId=bp_tid, levels=2)
            assert r["success"], r
            top = r["body"]["stackFrames"][0]
            assert "slow" in top["name"], (
                f"expected to be in slow(), got {top['name']!r}"
            )
            print(f"[test] top frame: {top['name']} line={top.get('line')}")

            # Detach (don't terminate); spin.exe keeps running, we kill it.
            r = client.request("disconnect", terminateDebuggee=False)
            assert r["success"], r
        finally:
            client.close()
    finally:
        spin.kill()
        try:
            spin.wait(timeout=3)
        except Exception:
            pass

    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
