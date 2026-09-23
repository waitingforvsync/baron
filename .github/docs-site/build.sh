#!/usr/bin/env bash
# Build the documentation site (waitingforvsync.github.io/baron) from README.md and
# docs/*.md. Pure pandoc: each page standalone, .md cross-links rewritten to .html, a
# shared nav bar and stylesheet, and .nojekyll so GitHub Pages serves the files as they
# are (Jekyll's Liquid pass chokes on things like the {{1,3},{2,4}} in the reference).
#
# Usage: .github/docs-site/build.sh <output-dir>   (run from the repo root)

set -euo pipefail

out=${1:?output directory required}
here=$(dirname "$0")
mkdir -p "$out"

nav='<nav class="site-nav"><a href="index.html">Baron</a><a href="guide.html">Guide</a><a href="reference.html">Reference</a><a href="zero-page-allocation.html">ZP allocation</a><a href="zero-page-allocation-internals.html">ZP internals</a><a class="ext" href="https://github.com/waitingforvsync/baron">GitHub</a></nav>'
navfile=$(mktemp)
printf '%s\n' "$nav" > "$navfile"

render() {
    local src=$1 dest=$2 title=$3
    pandoc "$src" -f gfm -t html5 --standalone --css style.css \
        --metadata title="$title" --include-before-body="$navfile" -o "$dest"
}

# The four docs: rewrite sibling .md links (with optional #anchor) to .html.
for name in guide reference zero-page-allocation zero-page-allocation-internals; do
    title=$(head -1 "docs/$name.md" | sed 's/^# *//; s/ *#*[[:space:]]*$//')
    sed -E 's|\]\(([A-Za-z0-9-]+)\.md(#[A-Za-z0-9-]+)?\)|](\1.html\2)|g' "docs/$name.md" > "$out/.$name.md"
    render "$out/.$name.md" "$out/$name.html" "$title"
    rm "$out/.$name.md"
done

# The index is the README, with docs/ links pointed at the site pages and repo-relative
# links (LICENSE) pointed back at GitHub.
sed -E -e 's|\]\(docs/([A-Za-z0-9-]+)\.md(#[A-Za-z0-9-]+)?\)|](\1.html\2)|g' \
       -e 's|\]\(LICENSE\)|](https://github.com/waitingforvsync/baron/blob/main/LICENSE)|g' \
       README.md > "$out/.index.md"
render "$out/.index.md" "$out/index.html" "Baron"
rm "$out/.index.md"

cp "$here/style.css" "$out/style.css"
touch "$out/.nojekyll"
rm "$navfile"

echo "site built into $out:"
ls "$out"
