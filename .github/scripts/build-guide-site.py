#!/usr/bin/env python3
"""Build the guide site from docs/guide/ and check it (#223).

Renders docs/guide/*.md and every docs/ page reachable from it by relative
links into static HTML, and fails if a relative link, an anchor or an image
points to nothing. It does not deploy anything: publishing is a separate
decision (#207).

Usage: build-guide-site.py [OUTPUT_DIR]   (default: site-guide)
Needs the Python "markdown" package.
"""

import html
import os
import re
import sys
from pathlib import Path

import markdown

REPO = Path(__file__).resolve().parents[2]
DOCS = REPO / "docs"
ENTRY = DOCS / "guide" / "README.md"
GITHUB = "https://github.com/ThomasPohl/TrailCamHub"
REF_RE = re.compile(r'(href|src)="([^"]*)"')
CSS = """body{font:16px/1.5 system-ui,sans-serif;max-width:52rem;margin:2rem auto;padding:0 1rem;color:#1b1b1b}
table{border-collapse:collapse;display:block;overflow-x:auto}
td,th{border:1px solid #ccc;padding:.3rem .6rem;text-align:left;vertical-align:top}
pre{background:#f4f4f4;padding:.8rem;overflow-x:auto}
code{background:#f4f4f4;padding:0 .2rem}pre code{padding:0}
img{max-width:100%}
"""


def slugify(value, separator):
    """GitHub's heading anchors: lower case, punctuation dropped, spaces to '-'."""
    value = re.sub(r"[^\w\- ]", "", value.strip().lower())
    return value.replace(" ", separator)


def out_path(src: Path) -> Path:
    """Where a page lands, relative to the site root."""
    rel = src.relative_to(DOCS)
    if src == ENTRY:
        return Path("guide/index.html")
    return rel.with_suffix(".html")


def flatten(tokens):
    for t in tokens:
        yield t["id"]
        yield from flatten(t["children"])


def render(src: Path):
    md = markdown.Markdown(
        extensions=["tables", "fenced_code", "toc"],
        extension_configs={"toc": {"slugify": slugify}},
    )
    body = md.convert(src.read_text(encoding="utf-8"))
    title = next((t["name"] for t in md.toc_tokens), src.stem)
    return body, title, set(flatten(md.toc_tokens))


def split_ref(ref: str):
    path, _, anchor = ref.partition("#")
    return path, anchor


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "site-guide")
    errors = []
    pages = {}  # source path -> (body, title, anchors)
    todo = [ENTRY] + sorted((DOCS / "guide").glob("*.md"))
    while todo:
        src = todo.pop(0)
        if src in pages:
            continue
        pages[src] = render(src)
        for _, ref in REF_RE.findall(pages[src][0]):
            path, _ = split_ref(ref)
            if not path or re.match(r"[a-z][a-z0-9+.-]*:", path):
                continue
            target = (src.parent / path).resolve()
            if target.suffix == ".md" and DOCS in target.parents and target.exists():
                todo.append(target)

    assets = set()
    for src, (body, title, _) in pages.items():
        here = out_path(src)

        def fix(match):
            attr, ref = match.group(1), match.group(2)
            path, anchor = split_ref(html.unescape(ref))
            if re.match(r"[a-z][a-z0-9+.-]*:", path):
                return match.group(0)  # external; not checked (no network)
            if not path:
                if anchor and anchor not in pages[src][2]:
                    errors.append(f"{src.relative_to(REPO)}: no heading for #{anchor}")
                return match.group(0)
            target = (src.parent / path).resolve()
            shown = src.relative_to(REPO)
            if REPO not in target.parents or not target.exists():
                errors.append(f"{shown}: broken link {ref}")
                return match.group(0)
            if target in pages:
                if anchor and anchor not in pages[target][2]:
                    errors.append(f"{shown}: {ref}: no heading for #{anchor}")
                rel = os.path.relpath(out_path(target), here.parent)
                return f'{attr}="{rel}{"#" + anchor if anchor else ""}"'
            if attr == "src" and DOCS in target.parents:
                assets.add(target)  # an image: copied next to the pages
                rel = os.path.relpath(target.relative_to(DOCS), here.parent)
                return f'{attr}="{rel}"'
            kind = "tree" if target.is_dir() else "blob"
            return f'{attr}="{GITHUB}/{kind}/main/{target.relative_to(REPO)}"'

        page = REF_RE.sub(fix, body)
        css = os.path.relpath("style.css", here.parent)
        doc = (
            f'<!doctype html>\n<html lang="en"><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width,initial-scale=1">'
            f"<title>{html.escape(title)} · TrailCamHub</title>"
            f'<link rel="stylesheet" href="{css}"></head><body>\n{page}\n</body></html>\n'
        )
        dest = out / here
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(doc, encoding="utf-8")

    for asset in assets:
        dest = out / asset.relative_to(DOCS)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(asset.read_bytes())
    (out / "style.css").write_text(CSS, encoding="utf-8")
    (out / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><meta http-equiv="refresh" '
        'content="0; url=guide/"><a href="guide/">Guide</a>\n',
        encoding="utf-8",
    )

    for error in sorted(set(errors)):
        print(f"error: {error}")
    print(f"{len(pages)} pages, {len(assets)} images, {len(set(errors))} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
