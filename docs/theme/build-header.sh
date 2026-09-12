#!/usr/bin/env bash
#
# Generates docs/theme/header.html from whatever Doxygen is installed, then
# patches in the theme's own additions.
#
# Why this is generated rather than committed: Doxygen's header template
# carries version-specific placeholders. A header written by 1.18 contains
# `$mermaidjs`, which 1.9.8 does not recognise and therefore emits as literal
# text at the top of every page -- with the side effect that Mermaid never
# loads and every diagram renders as raw source. Generating the header with
# the same Doxygen that will consume it removes that whole class of skew.
#
# Run from the repository root:  bash docs/theme/build-header.sh

set -euo pipefail

cd "$(dirname "$0")/../.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

doxygen -w html "$tmp/header.html" "$tmp/footer.html" "$tmp/style.css" >/dev/null

python3 - "$tmp/header.html" docs/theme/header.html <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
html = open(src, encoding="utf-8").read()

fonts_and_scripts = '''<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Playfair+Display:wght@400;500;600&family=Source+Sans+3:wght@300;400;500;600&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
<script type="text/javascript" src="$relpath^doxygen-awesome-darkmode-toggle.js"></script>
<script type="text/javascript" src="$relpath^doxygen-awesome-fragment-copy-button.js"></script>
<script type="text/javascript" src="$relpath^doxygen-awesome-paragraph-link.js"></script>
<script type="text/javascript" src="$relpath^doxygen-awesome-interactive-toc.js"></script>
<script type="text/javascript">
    DoxygenAwesomeDarkModeToggle.init()
    DoxygenAwesomeFragmentCopyButton.init()
    DoxygenAwesomeParagraphLink.init()
    DoxygenAwesomeInteractiveToc.init()
</script>
</head>'''

# Our own mermaid initialization goes BEFORE the $mermaidjs placeholder, so
# it renders every diagram first, with themeVariables pulled from the exact
# same palette as the rest of the site. `startOnLoad: false` plus an explicit
# `mermaid.run()` means ours does the real rendering; Doxygen's own
# $mermaidjs-substituted init call still runs afterward (it cannot be
# suppressed -- it is generated per-page, not from this template) but finds
# every diagram already marked processed and is a harmless no-op.
mermaid_leka = """<script type="module">
import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';
mermaid.initialize({
    startOnLoad: false,
    theme: 'base',
    themeVariables: {
        fontFamily: "'Source Sans 3', -apple-system, sans-serif",
        primaryColor: '#17322a',
        primaryTextColor: '#f3f1ec',
        primaryBorderColor: '#2fbf8f',
        lineColor: '#63666a',
        secondaryColor: '#1c1e20',
        tertiaryColor: '#17191b',
        background: '#0c0d0e',
        mainBkg: '#17322a',
        nodeBorder: '#2fbf8f',
        clusterBkg: '#141618',
        clusterBorder: 'rgba(243,241,236,0.12)',
        edgeLabelBackground: '#0c0d0e',
        textColor: '#f3f1ec',
        actorBkg: '#17322a',
        actorBorder: '#2fbf8f',
        actorTextColor: '#f3f1ec',
        signalColor: '#c4c6c8',
        signalTextColor: '#f3f1ec',
        labelBoxBkgColor: '#17322a',
        labelBoxBorderColor: '#2fbf8f',
        labelTextColor: '#f3f1ec',
        loopTextColor: '#9b9ea1',
    },
    flowchart: { htmlLabels: true, curve: 'basis' },
});
window.addEventListener('DOMContentLoaded', () => mermaid.run());
</script>
"""
html = html.replace("$mermaidjs", mermaid_leka + "$mermaidjs", 1)

if "</head>" not in html:
    sys.exit("generated header has no </head>")
html = html.replace("</head>", fonts_and_scripts, 1)

# Standing link to the replay viewer, directly under the project title.
viz_link = '''<!--END TITLEAREA-->
<div id="leka-viz-wrap"><a id="leka-viz-link" href="$relpath^viz/">&#9654; Replay viewer</a></div>'''
if "<!--END TITLEAREA-->" in html:
    html = html.replace("<!--END TITLEAREA-->", viz_link, 1)

open(dst, "w", encoding="utf-8").write(html)
print(f"header written from doxygen {sys.argv[0] and ''}".strip() or "header written")
PY

echo "docs/theme/header.html regenerated for $(doxygen --version)"
