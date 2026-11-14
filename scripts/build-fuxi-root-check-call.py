"""Insert the root-layout probe call into bsd_init, next to the display probe.

Same reasoning as build-fuxi-root-auth.py: the patch series already edits bsd_init.c twice, so a third
hunks-based patch would drift. This runs as an archive step against the file the series produced and
matches the anchor statement instead of a line number.

The call goes after the display probe and before the file systems come up, so its report lands in the
boot log immediately before launchd's own output.
"""
import argparse
import os
import sys
from pathlib import Path

ANCHOR = '\tfuxi_display_probe();\n'

INSERTION = (
    '\n'
    '\t/* fuxi (Xiaomi 13): report which paths the mounted root really has, off unless the boot argument\n'
    '\t * fuxi-root-check is set. Read-only; package/FuxiRootCheck.cpp of the fuxi device repository. */\n'
    '\textern void fuxi_root_check(void);\n'
    '\tfuxi_root_check();\n'
)

MARKER = 'fuxi_root_check();'


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
    raise SystemExit(f'build-fuxi-root-check-call: bsd_init.c not found; tried\n  {tried}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sources', nargs='*', help='ignored; board.toml entries stay uniform')
    args = parser.parse_args()
    del args.sources

    source = locate()
    text = source.read_text(encoding='utf-8')

    if MARKER in text:
        print(f'build-fuxi-root-check-call: already present in {source}')
        return 0

    if text.count(ANCHOR) != 1:
        raise SystemExit(f'build-fuxi-root-check-call: {text.count(ANCHOR)} display-probe anchors, '
                         'expected exactly one')

    text = text.replace(ANCHOR, ANCHOR + INSERTION, 1)
    source.write_text(text, encoding='utf-8')

    result = source.read_text(encoding='utf-8')
    if MARKER not in result or result.count('fuxi_display_probe();') != 1:
        raise SystemExit('build-fuxi-root-check-call: the edit did not land as intended')
    print(f'build-fuxi-root-check-call: {source} now calls fuxi_root_check after the display probe')
    return 0


if __name__ == '__main__':
    sys.exit(main())
