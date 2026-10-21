#!/usr/bin/env python3
"""Print a Baron source as one self-contained file, for the fuzz corpus.

The fuzz harness assembles a single string, so an INCLUDE or INCBIN in it finds
no file, and everything after one never runs. Here each INCLUDE is replaced by
the text it names (resolved against the including file's directory, as Baron
resolves it), and each INCBIN by an EQUB of a slice of the file - small enough
that a compressing transform over it stays quick under the sanitizers.

Usage: self_contained.py <source>   (the result goes to stdout)
"""

import re
import sys
from pathlib import Path

SLICE = 384          # bytes an INCBIN keeps, from the middle of the file
MAX_DEPTH = 16       # nested INCLUDEs followed, in case of a cycle

INCLUDE = re.compile(r'^\s*include\s+"([^"]+)"', re.IGNORECASE)
INCBIN = re.compile(r'\bincbin\s+"([^"]+)"', re.IGNORECASE)


def equb(path):
    """An EQUB of a slice from the middle of the file (a picture's edges are often blank)."""
    data = path.read_bytes()
    start = max(0, (len(data) - SLICE) // 2)
    return 'EQUB ' + ', '.join('&%02X' % b for b in data[start:start + SLICE])


def inline(path, depth=0):
    """The file's text with its INCLUDEs spliced in and its INCBINs turned into data."""
    lines = []
    for line in path.read_text(encoding='latin-1').splitlines():
        m = INCLUDE.match(line)
        if m and depth < MAX_DEPTH and (path.parent / m.group(1)).is_file():
            lines.append(inline(path.parent / m.group(1), depth + 1))
            continue
        lines.append(INCBIN.sub(
            lambda b: equb(path.parent / b.group(1)) if (path.parent / b.group(1)).is_file() else b.group(0),
            line))
    return '\n'.join(lines)


if __name__ == '__main__':
    sys.stdout.buffer.write((inline(Path(sys.argv[1])) + '\n').encode('latin-1'))
