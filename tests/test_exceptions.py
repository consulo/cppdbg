#!/usr/bin/env python3
"""End-to-end test for cppdbg M7: setExceptionBreakpoints.

throws.cpp throws std::runtime_error in thrower() and catches it in
main(), then prints "done" and exits cleanly. We verify two scenarios:

  - default (no filters):       process runs to "done" — no stopped event
  - "firstChance" filter on:    cppdbg breaks at the throw site

Usage:
    python tests/test_exceptions.py <cppdbg.exe> <throws.exe>
"""

from __future__ import annotations


import os
import queue
import subprocess
import sys
import time

from _dap_client import DapClient


def run_session(cppdbg_exe, throws_exe, exception_filters):
    """Returns (stopped_events, terminated_seen). For firstChance=on the
    test only verifies that *one* stopped(exception) event arrives —
    once we see it, M7's filter behaviour is proven; we tear down via
    disconnect(terminateDebuggee=True) to avoid the post-exception
    resume path (which has occasional engine-side latency unrelated
    to filter correctness)."""
    client = DapClient(cppdbg_exe)
    try:
        r = client.request("initialize", adapterID="cppdbg",
                           clientID="m7-test", linesStartAt1=True,
                           columnsStartAt1=True, pathFormat="path")
        assert r["success"], r
        filters_advertised = r["body"].get("exceptionBreakpointFilters", [])
        assert any(f["filter"] == "firstChance" for f in filters_advertised), (
            f"firstChance filter not advertised: {filters_advertised}"
        )
        client.wait_event("initialized")

        r = client.request("launch", timeout=30, program=throws_exe)
        assert r["success"], r
        r = client.request("setExceptionBreakpoints",
                           filters=exception_filters)
        assert r["success"], r
        r = client.request("configurationDone")
        assert r["success"], r
        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "entry", ev
        tid = ev["body"]["threadId"]

        r = client.request("continue", threadId=tid)
        assert r["success"], r

        # Drain events. With filters off: expect terminated, no stops.
        # With firstChance on: we'll see stopped(exception) — that's
        # the assertion we care about; bail out via terminate-disconnect
        # instead of rolling forward through the catch handler.
        stopped_events = []
        terminated = False
        deadline = time.time() + 15
        while not terminated:
            left = deadline - time.time()
            if left <= 0:
                break
            try:
                ev = client._events.get(timeout=left)
            except queue.Empty:
                break
            kind = ev.get("event")
            if kind == "stopped":
                stopped_events.append(ev["body"])
                if "firstChance" in exception_filters:
                    # M7's behaviour is verified — early-out.
                    break
            elif kind == "terminated":
                terminated = True
        client.request("disconnect",
                       terminateDebuggee="firstChance" in exception_filters)
        return stopped_events, terminated
    finally:
        client.close()


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    cppdbg_exe, throws_exe = argv[1], argv[2]
    for path in (cppdbg_exe, throws_exe):
        if not os.path.isfile(path):
            print(f"not found: {path}", file=sys.stderr)
            return 2

    print("--- default (no filters): exception caught silently ---")
    stops, term = run_session(cppdbg_exe, throws_exe, [])
    print(f"[test] stops={len(stops)} terminated={term}")
    assert term, "process should have terminated"
    assert len(stops) == 0, f"expected 0 stopped events, got {stops}"

    print("--- firstChance filter on: stops on the throw ---")
    stops, _ = run_session(cppdbg_exe, throws_exe, ["firstChance"])
    print(f"[test] stops={len(stops)} reasons={[s.get('reason') for s in stops]}")
    assert any(s.get("reason") == "exception" for s in stops), stops

    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
