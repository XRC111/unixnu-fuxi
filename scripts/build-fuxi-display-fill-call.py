"""Insert the scanout fill call next to the step-1 display probe.

Same reasoning as build-fuxi-root-check-call.py: the patch series already edits bsd_init.c twice, so a
hunk-based patch would drift. This runs as an archive step against the file the series produced and
matches an anchor statement.

The fill goes right after the step-1 probe, which is where the handoff record has just been read: the
probe reports the mode, the fill paints it, and both describe one boot.
"""
import argparse
import os
import sys
from pathlib import Path

ANCHOR = '\tfuxi_display_probe();\n'

INSERTION = (
    '\n'
    '\t/* fuxi (Xiaomi 13): paint the scanout the firmware handed over, off unless the boot argument\n'
    '\t * fuxi-display-fill is set. package/FuxiDisplayFill.cpp of the fuxi device repository. */\n'
    '\t{\n'
    '\t\textern void fuxi_display_fill(void);\n'
    '\t\tfuxi_display_fill();\n'
    '\t}\n'
)

MARKER = 'fuxi_display_fill();'


def locate() -> Path:
    here = Path.cwd()
    candidates = []
    for name in ('UNIXNU_XNU_WORK', 'XNU_WORK'):
        value = os.environ.get(name)
        if value:
            candidates.append(Path(value))
    candidates += [here / '.cache/xnu-work', here / 'build/xnu-work']
    for candidate in candidates:
        source = candidate / 'bsd/kern/bsd_init.c'
        if source.is_file():
            return source
    tried = '\n  '.join(str(c / 'bsd/kern/bsd_init.c') for c in candidates)
    raise SystemExit(f'build-fuxi-display-fill-call: bsd_init.c not found; tried\n  {tried}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sources', nargs='*', help='ignored; board.toml entries stay uniform')
    args = parser.parse_args()
    del args.sources

    source = locate()
    text = source.read_text(encoding='utf-8')

    if MARKER in text:
        print(f'build-fuxi-display-fill-call: already present in {source}')
        return 0

    if text.count(ANCHOR) != 1:
        raise SystemExit(f'build-fuxi-display-fill-call: {text.count(ANCHOR)} display-probe anchors, '
                         'expected exactly one')

    text = text.replace(ANCHOR, ANCHOR + INSERTION, 1)
    source.write_text(text, encoding='utf-8')

    result = source.read_text(encoding='utf-8')
    if MARKER not in result or result.count('fuxi_display_probe();') != 1:
        raise SystemExit('build-fuxi-display-fill-call: the edit did not land as intended')
    if result.index(MARKER) < result.index(ANCHOR):
        raise SystemExit('build-fuxi-display-fill-call: the call ended up before the anchor')
    print(f'build-fuxi-display-fill-call: {source} now calls fuxi_display_fill after the display probe')
    return 0


if __name__ == '__main__':
    sys.exit(main())
