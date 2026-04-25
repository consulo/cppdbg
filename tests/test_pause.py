#!/usr/bin/env python3
"""End-to-end test for cppdbg pause.

Spawns spin.exe (loops with 50ms sleeps), attaches, lets it run, then
asks the adapter to pause. Asserts a stopped(pause) event arrives and
that the call stack lands inside slow() — once paused the state is
deterministic, so we can fully validate.

Usage:
    python tests/test_pause.py <cppdbg.exe> <spin.exe>
"""

from __future__ import annotations


import os
import subprocess
import sys
import time

from _dap_client import DapClient


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    cppdbg_exe, spin_exe = argv[1], argv[2]
    for path in (cppdbg_exe, spin_exe):
        if not os.path.isfile(path):
            print(f"not found: {path}", file=sys.stderr)
            return 2

    spin = subprocess.Popen([spin_exe], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        line = spin.stdout.readline()
        if b"pid ready" not in line:
            print(f"unexpected output from spin: {line!r}", file=sys.stderr)
            return 1
        time.sleep(0.2)

        client = DapClient(cppdbg_exe)
        try:
            r = client.request("initialize", adapterID="cppdbg",
                               clientID="pause-test", linesStartAt1=True,
                               columnsStartAt1=True, pathFormat="path")
            assert r["success"], r
            client.wait_event("initialized")

            r = client.request("attach", timeout=30, processId=spin.pid)
            assert r["success"], r

            r = client.request("configurationDone")
            assert r["success"], r
            ev = client.wait_event("stopped", timeout=30)
            assert ev["body"]["reason"] == "entry", ev
            tid = ev["body"]["threadId"]

            # Resume: spin's slow() loop runs. Give it time to make
            # progress, then ask the adapter to pause.
            r = client.request("continue", threadId=tid)
            assert r["success"], r
            time.sleep(0.5)

            r = client.request("pause", threadId=tid)
            assert r["success"], r

            ev = client.wait_event("stopped", timeout=10)
            assert ev["body"]["reason"] == "pause", ev
            paused_tid = ev["body"]["threadId"]
            print(f"[test] paused, thread={paused_tid}")

            # Walk every thread's stack — at least one should be inside
            # slow() (the spin loop). The pause break itself often lands
            # on the engine-injected breakin thread, so the slow-thread
            # is a sibling, not necessarily the one named in the event.
            r = client.request("threads")
            assert r["success"], r
            saw_slow = False
            for t in r["body"]["threads"]:
                rs = client.request("stackTrace", threadId=t["id"], levels=8)
                if not rs["success"]:
                    continue
                for f in rs["body"]["stackFrames"]:
                    if "slow" in f.get("name", ""):
                        print(f"[test] thread {t['id']} ({t['name']}) in {f['name']} line={f.get('line')}")
                        saw_slow = True
                        break
            assert saw_slow, "no thread was inside slow() at pause time"

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
