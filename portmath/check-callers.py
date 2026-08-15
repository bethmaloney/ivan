#!/usr/bin/env python3
"""Fail if game code calls the host libm directly instead of going through portmath.

This is a policy check, not a correctness check. A raw sin() compiles and runs
perfectly well; it just quietly makes the native and WASM builds disagree, and
nothing downstream notices until a frame hash moves for reasons nobody can
localise. Cheaper to catch here.

Comments and string literals are stripped before matching. Doing it by regex
instead produced four false positives on the first run -- all of them prose
inside block comments that happened to mention sin(x) -- which is exactly the
failure mode that trains people to ignore a checker.

Usage: check-callers.py [dir ...]        (default: Main FeLib)
"""
import re
import sys
from pathlib import Path

# Not listed, on purpose:
#   sqrt   IEEE-754 mandates correct rounding, so every platform agrees, and
#          WASM has a native f64.sqrt instruction.
#   fmod   also exact -- it is a remainder, computed rather than approximated.
#   floor ceil fabs abs round trunc   exact.
# Everything below is approximated, and no standard says to what precision.
FUNCS = """sin cos tan asin acos atan atan2 sincos exp exp2 expm1
           log log2 log10 log1p pow cbrt hypot sinh cosh tanh""".split()

CALL = re.compile(r'(?<![A-Za-z0-9_:.>])(' + '|'.join(FUNCS) + r')\s*\(')


def strip(src):
    """Blank out comments and string literals, preserving line structure."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in src[i:j])); i = j
        elif c in '"\'':
            j, q = i + 1, c
            while j < n and src[j] != q:
                j += 2 if src[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(''.join(ch if ch == '\n' else ' ' for ch in src[i:j])); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def main():
    dirs = sys.argv[1:] or ['Main', 'FeLib']
    repo = Path(__file__).resolve().parent.parent
    hits = []

    for d in dirs:
        for path in sorted((repo / d).rglob('*')):
            if path.suffix not in ('.cpp', '.h') or not path.is_file():
                continue
            try:
                src = path.read_text(errors='replace')
            except OSError:
                continue
            for lineno, line in enumerate(strip(src).splitlines(), 1):
                m = CALL.search(line)
                if m and 'pm_' not in line and 'portmath::' not in line:
                    hits.append((path.relative_to(repo), lineno, m.group(1),
                                 src.splitlines()[lineno - 1].strip()))

    for path, lineno, fn, text in hits:
        print(f'{path}:{lineno}: calls {fn}() directly -- {text}')

    if hits:
        print(f'\n{len(hits)} direct host-libm call(s).', file=sys.stderr)
        print('Game code must call portmath (portmath/portmath.h), not the host libm.',
              file=sys.stderr)
        print('The host libm is not correctly rounded for these, so a native build',
              file=sys.stderr)
        print('and a WASM build compute different numbers. See portmath/README.md.',
              file=sys.stderr)
        return 1

    print(f'portmath: no direct host-libm calls in {" ".join(dirs)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
