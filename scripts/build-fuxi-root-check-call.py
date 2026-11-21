"""Insert the root-layout probe call into bsd_init, after the root vnode is set.

Same reasoning as build-fuxi-root-auth.py: the patch series already edits bsd_init.c twice, so a third
hunk-based patch would drift. This runs as an archive step against the file the series produced and
matches an anchor statement instead of a line number.

The anchor matters. The call first went next to the display probe, which sits in the device probe block
near the top of bsd_init, and vfs_mountroot() does not run until much later: rootvnode is still NULL
there, so the probe printed one line about the missing root and returned, and the panel never showed a
report. It belongs after set_rootvnode() (the first point where rootvnode names the mounted volume) and
before the ROOTAUTH gate, so the report describes the state launchd is about to meet.
"""
import argparse
import os
import sys
from pathlib import Path

ANCHOR = '\tinit_rootvnode = NULLVP;  /* use rootvnode after this point */\n'

INSERTION = (
    '\n'
    '\t/* fuxi (Xiaomi 13): report what the mounted root really contains, off unless the boot argument\n'
    '\t * fuxi-root-check is set. Read-only; package/FuxiRootCheck.cpp of the fuxi device repository. */\n'
    '\t{\n'
    '\t\textern void fuxi_root_check(void);\n'
    '\t\tfuxi_root_check();\n'
    '\t}\n'
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
        raise SystemExit(f'build-fuxi-root-check-call: {text.count(ANCHOR)} set_rootvnode anchors, '
                         'expected exactly one')

    text = text.replace(ANCHOR, ANCHOR + INSERTION, 1)
    source.write_text(text, encoding='utf-8')

    result = source.read_text(encoding='utf-8')
    if MARKER not in result or result.count(ANCHOR) != 1:
        raise SystemExit('build-fuxi-root-check-call: the edit did not land as intended')

    # The call has to sit after the anchor: check the ordering, not just presence.
    if result.index(MARKER) < result.index(ANCHOR):
        raise SystemExit('build-fuxi-root-check-call: the call ended up before the anchor')
    print(f'build-fuxi-root-check-call: {source} now calls fuxi_root_check after set_rootvnode')
    return 0


if __name__ == '__main__':
    sys.exit(main())
