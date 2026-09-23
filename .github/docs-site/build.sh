#!/usr/bin/env bash
# Build the documentation site (waitingforvsync.github.io/baron) from README.md and
# docs/*.md. Pure pandoc: each page standalone, .md cross-links rewritten to .html, a
# shared Cayman-style hero header and stylesheet, and .nojekyll so GitHub Pages serves
# the files as they are (Jekyll's Liquid pass chokes on things like the {{1,3},{2,4}}
# in the reference).
#
# Usage: .github/docs-site/build.sh <output-dir>   (run from the repo root)

set -euo pipefail

out=${1:?output directory required}
here=$(dirname "$0")
mkdir -p "$out"

# The hero header is the same on every page: the project, not the document. The document's
# own title is its first heading, below. pagetitle fills the <title> tag only - passing
# `title` instead would render a second visible heading block, which looked silly.
before=$(mktemp)
cat > "$before" << 'HTML'
<header class="site-header">
  <h1 class="project-name"><a href="index.html">Baron</a></h1>
  <p class="project-tagline">A cross-platform 6502 assembler targeting the BBC Micro</p>
  <nav class="header-nav">
    <a class="btn" href="guide.html">Guide</a>
    <a class="btn" href="reference.html">Reference</a>
    <a class="btn" href="zero-page-allocation.html">ZP allocation</a>
    <a class="btn" href="zero-page-allocation-internals.html">ZP internals</a>
    <a class="btn" href="https://github.com/waitingforvsync/baron">View on GitHub</a>
  </nav>
</header>
<main class="main-content">
HTML

after=$(mktemp)
cat > "$after" << 'HTML'
</main>
<footer class="site-footer">
  Baron is released under the <a href="https://github.com/waitingforvsync/baron/blob/main/LICENSE">MIT licence</a>.
</footer>
HTML

render() {
    local src=$1 dest=$2 pagetitle=$3
    pandoc "$src" -f gfm -t html5 --standalone --css style.css \
        --metadata pagetitle="$pagetitle" \
        --include-before-body="$before" --include-after-body="$after" -o "$dest"
}

# The four docs: rewrite sibling .md links (with optional #anchor) to .html.
for name in guide reference zero-page-allocation zero-page-allocation-internals; do
    title=$(head -1 "docs/$name.md" | sed 's/^# *//; s/ *#*[[:space:]]*$//')
    sed -E 's|\]\(([A-Za-z0-9-]+)\.md(#[A-Za-z0-9-]+)?\)|](\1.html\2)|g' "docs/$name.md" > "$out/.$name.md"
    render "$out/.$name.md" "$out/$name.html" "Baron - $title"
    rm "$out/.$name.md"
done

# The index is the README with its own `# Baron #` heading dropped (the hero already says
# so), docs/ links pointed at the site pages, and repo-relative links back at GitHub.
sed -E -e '1{/^# Baron #?[[:space:]]*$/d}' \
       -e 's|\]\(docs/([A-Za-z0-9-]+)\.md(#[A-Za-z0-9-]+)?\)|](\1.html\2)|g' \
       -e 's|\]\(LICENSE\)|](https://github.com/waitingforvsync/baron/blob/main/LICENSE)|g' \
       README.md > "$out/.index.md"
render "$out/.index.md" "$out/index.html" "Baron"
rm "$out/.index.md"

cp "$here/style.css" "$out/style.css"
touch "$out/.nojekyll"
rm "$before" "$after"

echo "site built into $out:"
ls "$out"
