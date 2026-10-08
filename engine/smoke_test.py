#!/usr/bin/env python3
"""Headless smoke test for rune-engine's editing protocol.

Usage: engine/smoke_test.py [ENGINE_BINARY] [DOCUMENT]

Starts the engine on a private socket, opens a document, types into it and
checks that the engine pushes invalidation/cursor/selection events and that
the rendered tile actually changes. Exits non-zero on failure.
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/engine/rune-engine")
DOC = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "samples/test.docx")

VK_LEFT, VK_RIGHT, VK_HOME, VK_BACKSPACE = 1026, 1027, 1028, 1283
VK_SHIFT, VK_MOD1 = 0x1000, 0x2000


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
    state_home = os.path.join(tmp, "state")
    autosave_dir = os.path.join(state_home, "rune/autosave")
    env = dict(os.environ, XDG_STATE_HOME=state_home, RUNE_AUTOSAVE_INTERVAL="1")
    engine = subprocess.Popen([ENGINE, "--socket-path", sock_path], stderr=subprocess.PIPE, text=True, env=env)
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
        check(len(r["page_rects"]) == r["pages"] and r["page_rects"][0] == page,
              "open returns page_rects matching pages/page_rect")

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
        sizes = [e for e in a.events if e["event"] == "size_changed"]
        check(sizes and len(sizes[-1]["page_rects"]) == sizes[-1]["pages"] > 1,
              "size_changed carries the new page_rects")

        check(not a.call("key", doc_id=doc, type="down", char_code=65)["ok"], "bad key type rejected")
        check(not a.call("key", doc_id=999, type="input", char_code=65)["ok"], "unknown doc rejected")

        # --- .doc (binary format) ---
        doc_path = os.environ.get("RUNE_TEST_DOC", os.path.join(ROOT, "samples/test.doc"))
        if os.path.exists(doc_path):
            a.events.clear()
            r = a.call("open", path=doc_path)
            check(r["ok"], "open .doc")
            doc_doc = r["doc_id"]
            check(r["pages"] == len(r["page_rects"])
                  and r["page_rect"] == r["page_rects"][0],
                  ".doc open returns matching page_rects")
            t = a.call("tile", doc_id=doc_doc, x=r["page_rect"][0], y=r["page_rect"][1],
                        width=r["page_rect"][2], height=r["page_rect"][3], px_width=400)
            check(t["ok"] and t["tile"].startswith("iVBOR"), ".doc tile is a base64 PNG")
            check(a.call("key", doc_id=doc_doc, type="input", char_code=82)["ok"], ".doc key input")
            check(a.wait_events(lambda ev: "cursor_changed" in kinds(ev)),
                  ".doc typing pushes cursor_changed")
            check(a.call("close", doc_id=doc_doc)["ok"], ".doc close")
        else:
            print("SKIP .doc (no sample file)")

        # --- Markdown import/export ---
        a.events.clear()
        md = "# Rune Title\n\nFirst **bold** paragraph.\n\nSecond *italic* paragraph.\n"
        r = a.call("new_md", markdown=md)
        check(r["ok"], "new_md")
        md_doc, md_page = r["doc_id"], r["page_rect"]
        check(r["pages"] == len(r["page_rects"]) and r["page_rect"] == r["page_rects"][0],
              "new_md returns matching page_rects")
        t = a.call("tile", doc_id=md_doc, x=md_page[0], y=md_page[1],
                   width=md_page[2], height=md_page[3], px_width=400)
        blank = a.call("new_md", markdown="")
        check(blank["ok"], "new_md with empty markdown")
        bt = a.call("tile", doc_id=blank["doc_id"], x=md_page[0], y=md_page[1],
                    width=md_page[2], height=md_page[3], px_width=400)
        check(t["ok"] and t["tile"].startswith("iVBOR") and t["tile"] != bt["tile"],
              "new_md renders content (tile differs from empty doc)")
        check(a.call("close", doc_id=blank["doc_id"])["ok"], "close empty new_md doc")

        e = a.call("export_md", doc_id=md_doc)
        check(e["ok"], "export_md")
        paras = e.get("markdown", "").strip().split("\n\n")
        check(paras == ["Rune Title", "First bold paragraph.", "Second italic paragraph."],
              f"export_md returns blank-line-separated paragraphs: {paras!r}")
        check(not a.call("export_md", doc_id=999)["ok"], "export_md unknown doc rejected")
        check(not a.call("new_md")["ok"], "new_md without markdown rejected")
        check(a.call("close", doc_id=md_doc)["ok"], "close new_md doc")

        # --- Formatting: format / style / get_state ---
        fmt_path = os.path.join(tmp, "format.docx")
        shutil.copy(DOC, fmt_path)
        r = a.call("open", path=fmt_path)
        fdoc = r["doc_id"]
        time.sleep(2)  # let LOK deliver the full STATE_CHANGED batch
        r = a.call("get_state", doc_id=fdoc)
        check(r["ok"] and isinstance(r["state"], dict), "get_state returns a state object")
        check(len(r["state"]) > 0, "get_state has accumulated state after open")

        check(a.call("format", doc_id=fdoc, command=".uno:Bold")["ok"], "format .uno:Bold")
        time.sleep(0.3)
        check("Bold" in a.call("get_state", doc_id=fdoc)["state"],
              "get_state has Bold after format command")
        check(a.call("format", doc_id=fdoc, command=".uno:NoSuchCommand")["ok"],
              "format with unknown .uno: command returns ok")
        check(not a.call("format", doc_id=fdoc, command="macro:///Standard.Module1.Main")["ok"],
              "format with non-.uno: command rejected")
        check(not a.call("format", doc_id=fdoc)["ok"], "format without command rejected")

        check(a.call("style", doc_id=fdoc, name="Text Body")["ok"], "style Text Body")
        time.sleep(0.3)
        check("StyleApply" in a.call("get_state", doc_id=fdoc)["state"],
              "get_state has StyleApply after style command")
        check(a.call("style", doc_id=fdoc, name="Heading 1")["ok"], "style Heading 1")
        check(not a.call("style", doc_id=fdoc, name="Heading 9")["ok"], "style unknown name rejected")

        check(not a.call("format", doc_id=999, command=".uno:Bold")["ok"], "format unknown doc rejected")
        check(not a.call("style", doc_id=999, name="Heading 1")["ok"], "style unknown doc rejected")
        check(not a.call("get_state", doc_id=999)["ok"], "get_state unknown doc rejected")
        check(a.call("close", doc_id=fdoc)["ok"], "close formatting doc")

        # --- mouse / copy / cut ---
        a.events.clear()
        r = a.call("new_md", markdown="Hello clipboard world\n\nSecond paragraph here.\n")
        cdoc, cpage = r["doc_id"], r["page_rect"]
        time.sleep(0.5)

        def shift_right(n):
            for _ in range(n):
                a.call("key", doc_id=cdoc, type="input", key_code=VK_RIGHT | VK_SHIFT)
                a.call("key", doc_id=cdoc, type="up", key_code=VK_RIGHT | VK_SHIFT)
            time.sleep(0.5)

        r = a.call("copy", doc_id=cdoc)
        check(r["ok"] and r["text"] == "", "copy with no selection returns empty text")
        # new_md leaves the cursor at the end; Ctrl+Home to the start.
        a.call("key", doc_id=cdoc, type="input", key_code=VK_HOME | VK_MOD1)
        a.call("key", doc_id=cdoc, type="up", key_code=VK_HOME | VK_MOD1)
        shift_right(5)
        r = a.call("copy", doc_id=cdoc)
        check(r["ok"] and r["text"] == "Hello", f"copy returns the selection: {r.get('text')!r}")
        r = a.call("cut", doc_id=cdoc)
        check(r["ok"] and r["text"] == "Hello", f"cut returns the selection: {r.get('text')!r}")
        time.sleep(0.5)
        check(a.call("copy", doc_id=cdoc)["text"] == "", "selection is gone after cut")
        shift_right(10)
        check(a.call("copy", doc_id=cdoc)["text"] == " clipboard", "cut removed the text from the document")
        r = a.call("cut", doc_id=999)
        check(not r["ok"], "cut unknown doc rejected")

        # Click inside the first line (default 2 cm margins = 1134 twips).
        x0, y0 = cpage[0] + 1134 + 200, cpage[1] + 1134 + 100
        a.events.clear()
        check(a.call("mouse", doc_id=cdoc, type="down", x=x0, y=y0)["ok"], "mouse down")
        check(a.call("mouse", doc_id=cdoc, type="up", x=x0, y=y0)["ok"], "mouse up")
        check(a.wait_events(lambda ev: "cursor_changed" in kinds(ev)), "mouse click pushes cursor_changed")
        time.sleep(0.3)
        check(a.call("copy", doc_id=cdoc)["text"] == "", "mouse click clears the selection")

        a.events.clear()
        a.call("mouse", doc_id=cdoc, type="down", x=x0, y=y0)
        a.call("mouse", doc_id=cdoc, type="move", x=x0 + 1500, y=y0)
        a.call("mouse", doc_id=cdoc, type="up", x=x0 + 1500, y=y0)
        check(a.wait_events(lambda ev: any(e["event"] == "selection_changed" and e["rects"] for e in ev)),
              "mouse drag pushes a non-empty selection_changed")
        time.sleep(0.3)
        check(a.call("copy", doc_id=cdoc)["text"] != "", "copy after mouse drag returns text")

        check(not a.call("mouse", doc_id=cdoc, type="click", x=x0, y=y0)["ok"], "mouse unknown type rejected")
        check(not a.call("mouse", doc_id=cdoc, x=x0, y=y0)["ok"], "mouse without type rejected")
        check(not a.call("mouse", doc_id=cdoc, type="down", x="1", y=y0)["ok"], "mouse non-integer x rejected")
        check(not a.call("mouse", doc_id=cdoc, type="down", x=x0)["ok"], "mouse without y rejected")
        check(not a.call("mouse", doc_id=999, type="down", x=x0, y=y0)["ok"], "mouse unknown doc rejected")
        check(not a.call("copy", doc_id=999)["ok"], "copy unknown doc rejected")
        check(a.call("close", doc_id=cdoc)["ok"], "close clipboard doc")

        # --- AI manager (skeleton: no network) ---
        r = a.call("ai", action="list_providers")
        check(r["ok"] and sorted(r["providers"]) == ["chatgpt", "claude", "grok"],
              "ai list_providers returns 3 providers")
        r = a.call("ai", action="status")
        check(r["ok"] and r["providers"] == {p: {"configured": False} for p in ("claude", "chatgpt", "grok")},
              "ai status lists every provider as unconfigured")
        check(a.call("ai", action="set_token", provider="claude", token="sk-test")["ok"], "ai set_token")
        r = a.call("ai", action="status")
        check(r["providers"]["claude"]["configured"] and not r["providers"]["chatgpt"]["configured"],
              "ai set_token marks only that provider configured")
        check(not a.call("ai", action="set_token", provider="nope", token="x")["ok"],
              "ai set_token unknown provider rejected")
        r = a.call("ai", action="send", provider="claude", model="claude-sonnet-4-6",
                   system="You are a writing assistant.", user="Improve this paragraph: ...")
        check(r["ok"] and r["content"] == "[AI response will go here]"
              and r["model"] == "claude" and r["provider"] == "claude",
              "ai send returns placeholder content")
        check(not a.call("ai", action="send", provider="claude", model="m")["ok"],
              "ai send without user rejected")
        check(not a.call("ai", action="bogus")["ok"], "ai unknown action rejected")

        # --- save / autosave (on a copy; never touch the sample) ---
        def type_text(doc_id, text):
            for ch in text:
                a.call("key", doc_id=doc_id, type="input", char_code=ord(ch))
                a.call("key", doc_id=doc_id, type="up", char_code=ord(ch))
            time.sleep(0.5)  # let LOK apply the async key events

        orig = os.path.join(tmp, "orig.docx")
        shutil.copy(DOC, orig)
        r = a.call("open", path=orig)
        sdoc = r["doc_id"]
        type_text(sdoc, "Saved")
        mtime = os.stat(orig).st_mtime_ns
        r = a.call("save", doc_id=sdoc)
        check(r["ok"] and r["path"] == os.path.realpath(orig) and os.stat(orig).st_mtime_ns != mtime,
              "save to original path writes it")

        copy = os.path.join(tmp, "copy.docx")
        r = a.call("save", doc_id=sdoc, path=copy, format="docx")
        check(r["ok"] and r["path"] == os.path.realpath(copy) and os.path.getsize(copy) > 0,
              "save_as returns the new path")
        r = a.call("save", doc_id=sdoc)
        check(r.get("path") == os.path.realpath(copy), "later save goes to the save_as path")
        r2 = a.call("open", path=copy)
        e = a.call("export_md", doc_id=r2["doc_id"])
        check(r2["ok"] and "Saved" in e.get("markdown", ""), "saved file contains the typed text")
        a.call("close", doc_id=r2["doc_id"])

        odt = os.path.join(tmp, "copy.odt")
        r = a.call("save", doc_id=sdoc, path=odt, format="odt")
        with open(odt, "rb") as f:
            check(r["ok"] and b"application/vnd.oasis.opendocument.text" in f.read(200),
                  "save_as odt writes an ODF file")
        pdf = os.path.join(tmp, "copy.pdf")
        r = a.call("save", doc_id=sdoc, path=pdf, format="pdf")
        with open(pdf, "rb") as f:
            check(r["ok"] and f.read(5) == b"%PDF-", "save_as pdf writes a PDF")
        check(a.call("save", doc_id=sdoc).get("path") == os.path.realpath(odt),
              "pdf export does not change the save path")

        check(not a.call("save", doc_id=sdoc, path=os.path.join(tmp, "nope/x.docx"))["ok"],
              "save to nonexistent directory rejected")
        check(not a.call("save", doc_id=sdoc, path=copy, format="xlsx")["ok"], "save unknown format rejected")
        check(not a.call("save", doc_id=999)["ok"], "save unknown doc rejected")
        nd = a.call("new_md", markdown="")["doc_id"]
        check(not a.call("save", doc_id=nd)["ok"], "save of new_md doc without path rejected")
        a.call("close", doc_id=nd)

        check(a.call("autosave", doc_id=sdoc, enabled=True)["ok"], "autosave enable")
        check(a.call("autosave", doc_id=sdoc, enabled=False)["ok"], "autosave disable")
        check(not a.call("autosave", doc_id=sdoc)["ok"], "autosave without enabled rejected")
        check(not a.call("autosave", doc_id=999, enabled=True)["ok"], "autosave unknown doc rejected")

        auto_file = os.path.join(autosave_dir, f"{sdoc}_copy.odt")
        a.call("autosave", doc_id=sdoc, enabled=True)
        a.events.clear()
        type_text(sdoc, "A")
        check(a.wait_events(lambda ev: any(e["event"] == "autosaved" and e["doc_id"] == sdoc for e in ev), 15)
              and os.path.exists(auto_file), "autosave fires and writes the autosave file")
        ev = [e for e in a.events if e["event"] == "autosaved"][0]
        check(ev["path"] == auto_file, "autosaved event carries the autosave path")
        a.events.clear()
        time.sleep(2.5)
        check(not any(e["event"] == "autosaved" for e in a.events), "no autosave without new edits")
        check(a.call("save", doc_id=sdoc)["ok"] and not os.path.exists(auto_file),
              "save deletes the autosave file")

        type_text(sdoc, "B")
        check(a.wait_events(lambda ev: any(e["event"] == "autosaved" for e in ev), 15)
              and os.path.exists(auto_file), "autosave written before close")
        check(a.call("close", doc_id=sdoc)["ok"] and not os.path.exists(auto_file),
              "close with autosave active removes the autosave file")

        # Shutdown autosaves unsaved edits. Typed right before quit, so the
        # 1 s timer is unlikely to beat it, but either way the file must exist.
        r = a.call("open", path=orig)
        a.call("autosave", doc_id=r["doc_id"], enabled=True)
        type_text(r["doc_id"], "C")
        shutdown_file = os.path.join(autosave_dir, f"{r['doc_id']}_orig.docx")

        check(a.call("close", doc_id=doc)["ok"], "close")
        check(a.call("quit")["ok"], "quit")
        engine.wait(timeout=30)
        check(engine.returncode == 0, "engine exited 0")
        check(os.path.exists(shutdown_file), "engine shutdown keeps an autosave of unsaved edits")
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
