#!/usr/bin/env python3
"""End-to-end smoke test for cppdbg M1.

Spawns ``cppdbg.exe``, drives a DAP session over stdio using a tiny
in-process client (no third-party deps), and asserts the M1 guarantees:

* ``initialize`` advertises ``supportsConfigurationDoneRequest``.
* ``launch`` returns success.
* A ``stopped`` event with ``reason == "entry"`` arrives after
  ``configurationDone``.
* ``continue`` resumes the debuggee and ``terminated`` fires on exit.

Usage::

    python tests/test_smoke.py <path-to-cppdbg.exe> <path-to-hello.exe>
"""

from __future__ import annotations

import os
import sys

from _dap_client import DapClient


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    cppdbg_exe, hello_exe = argv[1], argv[2]
    for path in (cppdbg_exe, hello_exe):
        if not os.path.isfile(path):
            print(f"not found: {path}", file=sys.stderr)
            return 2

    client = DapClient(cppdbg_exe)
    try:
        r = client.request(
            "initialize",
            adapterID="cppdbg",
            clientID="smoke",
            linesStartAt1=True,
            columnsStartAt1=True,
            pathFormat="path",
        )
        assert r["success"], r
        assert r["body"]["supportsConfigurationDoneRequest"] is True, r

        client.wait_event("initialized")

        r = client.request("launch", timeout=30, program=hello_exe)
        assert r["success"], r

        r = client.request("configurationDone")
        assert r["success"], r

        ev = client.wait_event("stopped", timeout=30)
        assert ev["body"]["reason"] == "entry", ev

        r = client.request("continue", threadId=0)
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
