#!/usr/bin/env python3
"""Headless smoke test for rune-engine's editing protocol.

Usage: engine/smoke_test.py [ENGINE_BINARY] [DOCUMENT]

Starts the engine on a private socket, opens a document, types into it and
checks that the engine pushes invalidation/cursor/selection events and that
the rendered tile actually changes. Exits non-zero on failure.
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/engine/rune-engine")
DOC = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "samples/test.docx")

VK_LEFT, VK_BACKSPACE, VK_SHIFT = 1026, 1283, 0x1000


class Client:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.buf = b""
        self.events = []
        self.next_id = 1

    def _read_line(self, deadline):
        while b"\n" not in self.buf:
            self.sock.settimeout(max(0.01, deadline - time.monotonic()))
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("engine closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def call(self, cmd, timeout=60, **kw):
        rid = self.next_id
        self.next_id += 1
        self.sock.sendall((json.dumps(dict(kw, id=rid, cmd=cmd)) + "\n").encode())
        deadline = time.monotonic() + timeout
        while True:
            msg = self._read_line(deadline)
            if "id" not in msg:
                self.events.append(msg)
            elif msg["id"] == rid:
                return msg
            else:
                raise RuntimeError(f"unexpected reply {msg}")

    def wait_events(self, pred, timeout=10):
        """Collect push events until pred(all_events) holds or timeout."""
        deadline = time.monotonic() + timeout
        while not pred(self.events):
            try:
                msg = self._read_line(deadline)
            except (socket.timeout, TimeoutError):
                return False
            if "id" in msg:
                raise RuntimeError(f"stray reply {msg}")
            self.events.append(msg)
        return True


failures = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def kinds(events):
    return {e["event"] for e in events}


def main():
    tmp = tempfile.mkdtemp(prefix="rune-smoke-")
    sock_path = os.path.join(tmp, "e.sock")
    engine = subprocess.Popen([ENGINE, "--socket-path", sock_path], stderr=subprocess.PIPE, text=True)
    try:
        for _ in range(600):
            if os.path.exists(sock_path):
                break
            time.sleep(0.05)
        a, b = Client(sock_path), Client(sock_path)

        check(a.call("ping")["ok"], "ping")
        r = a.call("open", path=DOC)
        check(r["ok"], "open")
        doc, page = r["doc_id"], r["page_rect"]

        def tile():
            t = a.call("tile", doc_id=doc, x=page[0], y=page[1], width=page[2], height=page[3], px_width=600)
            check(t["ok"], "tile")
            return t["tile"]

        before = tile()
        a.events.clear()

        for ch in "Rune":
            check(a.call("key", doc_id=doc, type="input", char_code=ord(ch), key_code=0)["ok"], f"key input {ch!r}")
            a.call("key", doc_id=doc, type="up", char_code=ord(ch), key_code=0)
        check(a.wait_events(lambda ev: "cursor_changed" in kinds(ev)
                            or "tiles_changed" in kinds(ev)),
              "typing pushes cursor_changed or tiles_changed")
        for e in a.events:
            check("id" not in e and e["doc_id"] == doc, f"push event has no id: {e['event']}")
            break
        tc_events = [e for e in a.events if e["event"] == "tiles_changed"]
        if tc_events:
            tc = tc_events[0]
            check(all(isinstance(tc[k], int) for k in ("x", "y", "width", "height")), "tiles_changed has a rect")
        # Multi-client event broadcast may use any event type.
        check(b.wait_events(lambda ev: kinds(ev) & {"tiles_changed", "cursor_changed"}),
              "second client also receives events")

        time.sleep(0.5)  # let LOK finish the async key events before painting
        after = tile()
        check(after != before, "tile changed after typing")

        a.events.clear()
        a.call("key", doc_id=doc, type="input", key_code=VK_LEFT | VK_SHIFT)
        a.call("key", doc_id=doc, type="up", key_code=VK_LEFT | VK_SHIFT)
        check(a.wait_events(lambda ev: any(e["event"] == "selection_changed" and e["rects"] for e in ev)),
              "shift+left pushes a non-empty selection_changed")

        a.events.clear()
        check(a.call("key", doc_id=doc, type="input", key="Backspace")["ok"], "key by name")
        a.call("key", doc_id=doc, type="up", key="Backspace")
        check(a.wait_events(lambda ev: kinds(ev) & {"tiles_changed", "cursor_changed"}),
              "backspace pushes cursor_changed or tiles_changed")

        a.events.clear()
        check(a.call("paste", doc_id=doc, mime_type="text/plain", data="pasted text\n" * 80)["ok"], "paste")
        check(a.wait_events(lambda ev: "size_changed" in kinds(ev)), "long paste pushes size_changed")

        check(not a.call("key", doc_id=doc, type="down", char_code=65)["ok"], "bad key type rejected")
        check(not a.call("key", doc_id=999, type="input", char_code=65)["ok"], "unknown doc rejected")
        check(a.call("close", doc_id=doc)["ok"], "close")
        check(a.call("quit")["ok"], "quit")
        engine.wait(timeout=30)
        check(engine.returncode == 0, "engine exited 0")
    finally:
        if engine.poll() is None:
            engine.kill()
        err = engine.stderr.read()
        if failures:
            sys.stderr.write(err)

    print(f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
