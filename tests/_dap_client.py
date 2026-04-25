"""Shared DAP client for cppdbg integration tests.

Each test used to carry its own ~90-line copy of this. Pulling it into
one module keeps the per-test files focused on the actual scenario.

The client speaks DAP over a child process's stdio: framed JSON on
stdin/stdout, plain text on stderr (drained to the host's stderr with a
[cppdbg] prefix so engine diagnostics surface in test logs).
"""

from __future__ import annotations

import json
import queue
import subprocess
import sys
import threading
import time


class DapClient:
    """Synchronous request / wait_event API over a stdio DAP child."""

    def __init__(self, exe_path: str) -> None:
        self.proc = subprocess.Popen(
            [exe_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self._seq = 0
        self._events: "queue.Queue[dict]" = queue.Queue()
        self._responses: "dict[int, dict]" = {}
        self._cond = threading.Condition()
        threading.Thread(target=self._read_loop, daemon=True).start()
        threading.Thread(target=self._drain_stderr, daemon=True).start()

    # ---- framing ---------------------------------------------------

    def _read_loop(self) -> None:
        f = self.proc.stdout
        assert f is not None
        while True:
            header = b""
            while b"\r\n\r\n" not in header:
                b = f.read(1)
                if not b:
                    return
                header += b
            length = 0
            for line in header.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1].strip())
            body = f.read(length)
            try:
                msg = json.loads(body)
            except json.JSONDecodeError:
                continue
            kind = msg.get("type")
            if kind == "response":
                with self._cond:
                    self._responses[msg["request_seq"]] = msg
                    self._cond.notify_all()
            elif kind == "event":
                self._events.put(msg)

    def _drain_stderr(self) -> None:
        assert self.proc.stderr is not None
        for line in self.proc.stderr:
            sys.stderr.write("[cppdbg] " + line.decode(errors="replace"))

    # ---- requests / events ----------------------------------------

    def request(self, command: str, timeout: float = 10.0, **args) -> dict:
        self._seq += 1
        msg = {"seq": self._seq, "type": "request",
               "command": command, "arguments": args}
        body = json.dumps(msg).encode()
        header = f"Content-Length: {len(body)}\r\n\r\n".encode()
        assert self.proc.stdin is not None
        self.proc.stdin.write(header + body)
        self.proc.stdin.flush()
        seq = self._seq
        deadline = time.time() + timeout
        with self._cond:
            while seq not in self._responses:
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError(f"no response to {command!r}")
                self._cond.wait(left)
            return self._responses.pop(seq)

    def wait_event(self, name: str, timeout: float = 10.0) -> dict:
        """Pop events until one matches `name`. Discards non-matching events."""
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError(f"no event {name!r}")
            ev = self._events.get(timeout=left)
            if ev.get("event") == name:
                return ev

    def drain_events(self, until_event: str, timeout: float = 15.0) -> list[dict]:
        """Pop *every* event up to and including `until_event`. Useful when the
        caller needs to count or inspect intermediate events that wait_event
        would otherwise discard."""
        deadline = time.time() + timeout
        collected = []
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError(f"no event {until_event!r}")
            ev = self._events.get(timeout=left)
            collected.append(ev)
            if ev.get("event") == until_event:
                return collected

    def close(self) -> None:
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
