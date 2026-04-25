#!/usr/bin/env python3
"""End-to-end tests for cppdbg M8: conditional / hit-count / log /
function breakpoints, plus disassemble.

Uses loops.cpp:
  1: #include <cstdio>
  2:
  3: int sum = 0;
  4:
  5: int compute(int i) {
  6:     sum += i;
  7:     return sum;
  8: }
  9:
 10: int main() {
 11:     for (int i = 0; i < 10; ++i) {
 12:         compute(i);
 13:     }
 14:     std::printf("sum=%d\\n", sum);
 15:     return 0;
 16: }

Usage:
    python tests/test_advanced_breakpoints.py <cppdbg.exe> <loops.exe> <loops.cpp>
"""

from __future__ import annotations


import os
import queue
import subprocess
import sys
import time

from _dap_client import DapClient


def evaluate_int(client, frame_id, expr):
    r = client.request("evaluate", expression=expr,
                       frameId=frame_id, context="repl")
    assert r["success"], (expr, r)
    raw = r["body"]["result"]
    # DbgEng renders ints as "0nN" (decimal) or "0xN" (hex).
    raw = raw.lstrip()
    if raw.startswith("0n"):
        return int(raw[2:])
    if raw.startswith("0x") or raw.startswith("0X"):
        return int(raw, 16)
    return int(raw)


def setup_session(cppdbg_exe, loops_exe, loops_src, source_breakpoints):
    """Initialize → launch → setBreakpoints → configurationDone → past entry.
    Returns (client, thread_id)."""
    client = DapClient(cppdbg_exe)
    r = client.request("initialize", adapterID="cppdbg", clientID="m8-test",
                       linesStartAt1=True, columnsStartAt1=True,
                       pathFormat="path")
    assert r["success"], r
    body = r["body"]
    for cap in ("supportsConditionalBreakpoints",
                "supportsHitConditionalBreakpoints",
                "supportsLogPoints",
                "supportsFunctionBreakpoints",
                "supportsDisassembleRequest"):
        assert body.get(cap) is True, f"{cap} not advertised: {body}"
    client.wait_event("initialized")

    r = client.request("launch", timeout=30, program=loops_exe)
    assert r["success"], r

    if source_breakpoints:
        r = client.request("setBreakpoints",
                           source={"path": loops_src},
                           breakpoints=source_breakpoints)
        assert r["success"], r
        for b in r["body"]["breakpoints"]:
            assert b["verified"], r

    r = client.request("configurationDone")
    assert r["success"], r
    ev = client.wait_event("stopped", timeout=30)
    assert ev["body"]["reason"] == "entry", ev
    return client, ev["body"]["threadId"]


def get_local(client, tid, name):
    r = client.request("stackTrace", threadId=tid, levels=1)
    assert r["success"], r
    frame_id = r["body"]["stackFrames"][0]["id"]
    r = client.request("scopes", frameId=frame_id)
    assert r["success"], r
    locals_ref = next(s for s in r["body"]["scopes"]
                      if s["name"].lower() == "locals")["variablesReference"]
    r = client.request("variables", variablesReference=locals_ref)
    assert r["success"], r
    return frame_id, {v["name"]: v["value"] for v in r["body"]["variables"]}


def test_conditional(cppdbg_exe, loops_exe, loops_src):
    print("--- conditional BP: stop on compute() only when i == 7 ---")
    client, tid = setup_session(cppdbg_exe, loops_exe, loops_src,
                                source_breakpoints=[
                                    {"line": 6, "condition": "i == 7"}
                                ])
    try:
        r = client.request("continue", threadId=tid)
        assert r["success"], r
        ev = client.wait_event("stopped", timeout=15)
        assert ev["body"]["reason"] == "breakpoint", ev
        tid2 = ev["body"]["threadId"]
        _, locals_ = get_local(client, tid2, "i")
        i_text = locals_.get("i", "")
        print(f"[test] hit at i={i_text!r}")
        assert "7" in i_text, f"expected i=7, got {i_text!r}"
        r = client.request("continue", threadId=tid2)
        assert r["success"], r
        client.wait_event("terminated", timeout=15)
        client.request("disconnect", terminateDebuggee=False)
    finally:
        client.close()


def test_hitcount(cppdbg_exe, loops_exe, loops_src):
    print("--- hit-count BP: fire on the 4th hit (i==3) ---")
    client, tid = setup_session(cppdbg_exe, loops_exe, loops_src,
                                source_breakpoints=[
                                    {"line": 6, "hitCondition": "4"}
                                ])
    try:
        r = client.request("continue", threadId=tid)
        assert r["success"], r
        ev = client.wait_event("stopped", timeout=15)
        assert ev["body"]["reason"] == "breakpoint", ev
        tid2 = ev["body"]["threadId"]
        _, locals_ = get_local(client, tid2, "i")
        i_text = locals_.get("i", "")
        print(f"[test] hit on iteration with i={i_text!r}")
        assert "3" in i_text, f"expected 4th hit (i=3), got i={i_text!r}"
        # After the pass-count threshold, DbgEng's BP continues to fire on
        # every subsequent hit. Clear the BP so the loop can run to end.
        r = client.request("setBreakpoints",
                           source={"path": loops_src}, breakpoints=[])
        assert r["success"], r
        r = client.request("continue", threadId=tid2)
        assert r["success"], r
        client.wait_event("terminated", timeout=15)
        client.request("disconnect", terminateDebuggee=False)
    finally:
        client.close()


def test_function_bp(cppdbg_exe, loops_exe, loops_src):
    print("--- function BP on 'loops!compute' ---")
    client, tid = setup_session(cppdbg_exe, loops_exe, loops_src,
                                source_breakpoints=[])
    try:
        r = client.request("setFunctionBreakpoints",
                           breakpoints=[{"name": "loops!compute"}])
        assert r["success"], r
        assert r["body"]["breakpoints"][0]["verified"], r
        print("[test] function BP verified")
        r = client.request("continue", threadId=tid)
        assert r["success"], r
        ev = client.wait_event("stopped", timeout=15)
        assert ev["body"]["reason"] == "breakpoint", ev
        tid2 = ev["body"]["threadId"]
        r = client.request("stackTrace", threadId=tid2, levels=1)
        assert r["success"], r
        top = r["body"]["stackFrames"][0]
        print(f"[test] stopped in {top['name']}")
        assert "compute" in top["name"], top
        # Detach without continuing — clear function BPs first so they
        # don't fire again on the next iteration.
        client.request("setFunctionBreakpoints", breakpoints=[])
        client.request("continue", threadId=tid2)
        client.wait_event("terminated", timeout=15)
        client.request("disconnect", terminateDebuggee=False)
    finally:
        client.close()


def test_logpoint(cppdbg_exe, loops_exe, loops_src):
    print("--- log BP at line 6: should never stop, just log + continue ---")
    client, tid = setup_session(cppdbg_exe, loops_exe, loops_src,
                                source_breakpoints=[
                                    {"line": 6, "logMessage": "iter hit"}
                                ])
    try:
        r = client.request("continue", threadId=tid)
        assert r["success"], r
        # Drain *all* events until terminated, collecting outputs and
        # asserting we never see a stopped event (logpoints must be
        # transparent).
        log_count = 0
        stopped_count = 0
        deadline = time.time() + 15
        while True:
            left = deadline - time.time()
            assert left > 0, "no terminated event"
            ev = client._events.get(timeout=left)
            kind = ev.get("event")
            if kind == "output":
                if "iter hit" in ev["body"].get("output", ""):
                    log_count += 1
            elif kind == "stopped":
                stopped_count += 1
            elif kind == "terminated":
                break
        print(f"[test] received {log_count} log lines, {stopped_count} stopped events")
        assert stopped_count == 0, "logpoint must not surface stopped"
        assert log_count == 10, f"expected 10 log hits, got {log_count}"
        client.request("disconnect", terminateDebuggee=False)
    finally:
        client.close()


def test_disassemble(cppdbg_exe, loops_exe, loops_src):
    print("--- disassemble around compute() entry ---")
    client, tid = setup_session(cppdbg_exe, loops_exe, loops_src,
                                source_breakpoints=[{"line": 6}])
    try:
        r = client.request("continue", threadId=tid)
        assert r["success"], r
        ev = client.wait_event("stopped", timeout=15)
        assert ev["body"]["reason"] == "breakpoint", ev
        tid2 = ev["body"]["threadId"]
        # Get the current PC from stackTrace's instructionPointerReference
        # is optional; instead we evaluate $ip.
        r = client.request("evaluate", expression="@rip",
                           context="repl")
        assert r["success"], r
        ip_text = r["body"]["result"]
        # value like "0x00007ff6`abc012345"; strip the backtick.
        ip_text = ip_text.replace("`", "")
        print(f"[test] @rip = {ip_text}")
        r = client.request("disassemble",
                           memoryReference=ip_text,
                           instructionCount=4,
                           resolveSymbols=True)
        assert r["success"], r
        instrs = r["body"]["instructions"]
        assert len(instrs) == 4, instrs
        for i, inst in enumerate(instrs):
            print(f"[test]   {inst['address']}  {inst.get('symbol','')}  {inst['instruction']}")
            assert inst["address"].startswith("0x"), inst
            assert inst["instruction"], inst
        # The top frame is compute() — the symbol resolver should
        # mention compute on the very first decoded instruction.
        assert any("compute" in (inst.get("symbol") or inst["instruction"])
                   for inst in instrs), instrs
        # Clear BPs so we run to completion.
        client.request("setBreakpoints",
                       source={"path": loops_src}, breakpoints=[])
        client.request("continue", threadId=tid2)
        client.wait_event("terminated", timeout=15)
        client.request("disconnect", terminateDebuggee=False)
    finally:
        client.close()


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    cppdbg_exe, loops_exe, loops_src = argv[1], argv[2], argv[3]
    for path in (cppdbg_exe, loops_exe, loops_src):
        if not os.path.isfile(path):
            print(f"not found: {path}", file=sys.stderr)
            return 2
    loops_src = os.path.abspath(loops_src)

    test_conditional(cppdbg_exe, loops_exe, loops_src)
    test_hitcount(cppdbg_exe, loops_exe, loops_src)
    test_logpoint(cppdbg_exe, loops_exe, loops_src)
    test_function_bp(cppdbg_exe, loops_exe, loops_src)
    test_disassemble(cppdbg_exe, loops_exe, loops_src)

    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
