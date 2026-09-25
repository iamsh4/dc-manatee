#!/usr/bin/env python3
"""Build the static Dreamcast audio / Manatee driver explainer site: wraps site/src/<page>.html bodies in the shared template.

Each source file starts with a line `<!-- title: Page title -->`. Output: site/<page>.html.
The navigation itself is generated client-side by site/assets/site.js (single source of truth).
"""
import pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / 'site/src'
OUT = ROOT / 'site'

LOGO = '''<svg class="logo" viewBox="0 0 32 32" aria-hidden="true">
<circle cx="16" cy="16" r="15" fill="none" stroke="var(--c-aica)" stroke-width="2"/>
<path d="M5 16 C8 6, 11 6, 13 16 S18 26, 20 16 S25 8, 27 16" fill="none" stroke="var(--c-arm)" stroke-width="2.4" stroke-linecap="round"/>
</svg>'''

TEMPLATE = '''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} · Dreamcast audio &amp; the Manatee driver</title>
<link rel="stylesheet" href="assets/style.css">
<script src="assets/site.js"></script>
</head>
<body>
<header class="topbar">
  <button id="nav-toggle" class="icon-btn" aria-label="Menu">☰</button>
  <a class="brand" href="index.html">{logo}<span>Manatee <small>· Dreamcast audio &amp; sound driver, explained</small></span></a>
  <div class="spacer"></div>
  <button id="theme-toggle" class="icon-btn" aria-label="Toggle theme"></button>
</header>
<div class="layout">
  <nav class="sidebar" aria-label="Pages"></nav>
  <main>
    <article class="content">
{body}
      <nav class="pager" aria-label="Previous and next page"></nav>
      <div class="footer">Reference build: the Manatee driver shipped in Soul Calibur (USA), v1.1 build 0x3F. Built from the decompilation in this repository (<code>src/arm/</code>, <code>docs/</code>). Hover diagram elements for details.</div>
    </article>
  </main>
  <aside class="toc" aria-label="On this page"></aside>
</div>
</body>
</html>
'''

def main():
    n = 0
    for f in sorted(SRC.glob('*.html')):
        text = f.read_text()
        m = re.match(r'<!--\s*title:\s*(.*?)\s*-->\n', text)
        title = m.group(1) if m else f.stem
        body = text[m.end():] if m else text
        (OUT / f.name).write_text(TEMPLATE.format(title=title, logo=LOGO, body=body))
        n += 1
    print(f'built {n} pages into {OUT}')

if __name__ == '__main__':
    sys.exit(main())
