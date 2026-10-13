#!/usr/bin/env python3
"""Builds the Rune guide (docs/guide/*.html) from tools/guide/pages/.

    tools/guide/build.py

Each page in pages/ is an HTML fragment (the content of <main>) whose first
line is a header comment:

    <!-- title: Getting started | blurb: One line for the guide's index -->

Pages appear in file-name order (01-..., 02-...); 00-index.html is the
guide's front page. Every <h2 id="..."> becomes a link under the page in the
sidebar. The screenshots it uses are made by tools/demo/make-media.py guide.
Standard library only.
"""

import html
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEST = ROOT / "docs" / "guide"

MARK = ('<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg" aria-hidden="true"><path d="M30 15 L30 85 '
        'M30 15 L68 38 L30 56 M68 38 L78 85" stroke="#d98f4e" stroke-width="7" fill="none" stroke-linecap="square"/></svg>')
FAVICON = ("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'%3E%3Crect width='100' "
           "height='100' fill='%230a0908'/%3E%3Cpath d='M30 20 L30 80 M30 20 L62 40 L30 55 M62 40 L70 80' "
           "stroke='%23d98f4e' stroke-width='6' fill='none' stroke-linecap='square'/%3E%3C/svg%3E")

SHELL = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{title} — Rune Guide</title>
<meta name="description" content="{blurb}">
<meta property="og:title" content="{title} — Rune Guide">
<meta property="og:description" content="{blurb}">
<meta property="og:image" content="https://rune.ninepointlabs.com/images/og.jpg">
<link rel="icon" href="{favicon}">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Cinzel:wght@500;600;700&family=Spectral:ital,wght@0,400;0,500;0,600;1,400&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
<link rel="stylesheet" href="guide.css">
</head>
<body>
<!-- Built by tools/guide/build.py from tools/guide/pages/{source}; edit that, not this. -->
<header>
  <div class="nav">
    <a href="../" class="wordmark">{mark} RUNE <span class="sub">Guide</span></a>
    <nav class="nav-links">
      <a href="../" class="wide">Home</a>
      <a href="./" aria-current="page">Guide</a>
      <a href="../roadmap.html" class="wide">Roadmap</a>
      <a href="../#get-started">Install</a>
      <a href="https://github.com/ninepointlabs/rune">GitHub</a>
    </nav>
  </div>
</header>
<div class="layout">
  <nav class="toc" aria-label="Guide">
    <h2>The guide</h2>
    <ol>
{toc}
    </ol>
  </nav>
  <main>
{body}
{pager}
  </main>
</div>
<footer>
  <p>Rune is MIT-licensed and built by <a href="https://ninepointlabs.com">Nine Point Labs</a>. Something wrong or missing here? <a href="https://github.com/ninepointlabs/rune/issues">Open an issue</a>.</p>
</footer>
</body>
</html>
"""


def load():
    pages = []
    for path in sorted((HERE / "pages").glob("*.html")):
        text = path.read_text()
        header = re.match(r"<!--(.*?)-->\n", text)
        if not header:
            sys.exit(f"{path.name}: needs a header comment")
        meta = dict(part.strip().split(":", 1) for part in header.group(1).split("|"))
        meta = {k.strip(): v.strip() for k, v in meta.items()}
        slug = re.sub(r"^\d+-", "", path.stem)
        pages.append({
            "source": path.name,
            "slug": slug,
            "file": "index.html" if slug == "index" else f"{slug}.html",
            "title": meta["title"],
            "blurb": meta.get("blurb", ""),
            "body": text[header.end():].rstrip(),
            "sections": re.findall(r'<h2 id="([^"]+)">(.*?)</h2>', text),
        })
    return pages


def toc(pages, current):
    lines = []
    for page in pages:
        here = page is current
        attr = ' aria-current="page"' if here else ""
        name = "Overview" if page["slug"] == "index" else page["title"]
        lines.append(f'      <li><a href="{page["file"]}"{attr}>{html.escape(name)}</a>')
        if here and page["sections"]:
            lines.append('        <ol class="sections">')
            for anchor, label in page["sections"]:
                lines.append(f'          <li><a href="#{anchor}">{label}</a></li>')
            lines.append("        </ol>")
        lines.append("      </li>")
    return "\n".join(lines)


def pager(pages, i):
    parts = []
    if i > 0:
        prev = pages[i - 1]
        name = "Overview" if prev["slug"] == "index" else prev["title"]
        parts.append(f'<a class="prev" href="{prev["file"]}"><span class="dir">Previous</span>{html.escape(name)}</a>')
    if i + 1 < len(pages):
        nxt = pages[i + 1]
        parts.append(f'<a class="next" href="{nxt["file"]}"><span class="dir">Next</span>{html.escape(nxt["title"])}</a>')
    return '    <nav class="pager" aria-label="Pages">' + "".join(parts) + "</nav>" if parts else ""


def check_links(page):
    """Local links and images must exist."""
    missing = []
    for ref in re.findall(r'(?:src|href)="([^"#:]+)(?:#[^"]*)?"', page["body"]):
        if not (DEST / ref).exists():
            missing.append(ref)
    return missing


def main():
    pages = load()
    if not pages or pages[0]["slug"] != "index":
        sys.exit("pages/00-index.html must come first")
    problems = []
    for i, page in enumerate(pages):
        out = SHELL.format(title=html.escape(page["title"]), blurb=html.escape(page["blurb"]), favicon=FAVICON,
                           mark=MARK, source=page["source"], toc=toc(pages, page), body=page["body"],
                           pager=pager(pages, i))
        (DEST / page["file"]).write_text(out)
        print(f"wrote docs/guide/{page['file']}")
    # After all are written, so links between pages resolve.
    for page in pages:
        problems += [f'{page["source"]}: missing {ref}' for ref in check_links(page)]
    if problems:
        sys.exit("\n".join(problems))


if __name__ == "__main__":
    main()
