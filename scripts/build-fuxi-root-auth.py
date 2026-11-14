#!/usr/bin/env python3
"""Continue past the Apple root-seal check instead of panicking.

Why an archive step and not a patch: two board patches already edit bsd_init.c (the touch probe and the
display probe), so a third hunks-based patch has to be regenerated whenever either moves the surrounding
lines. That failed twice in CI: "patch failed at bsd_init.c:943", then again at :957 after the first fix.
The archive chain runs from board.toml [kernel] archives, after the patch series is applied (kernel.py
sets UNIXNU_PATCH_LIST before build_archives), so this edits the file the series produced and matches the
statement itself rather than a line number.

The change is one statement: the FSIOC_KERNEL_ROOTAUTH failure stops being a panic and becomes a log
line. That gate only asks whether APFS recognises an Apple Image4 seal; it does not decide whether the
data was checked. NativeUfs authenticates every 4 KiB block against the owner's VerifiedRoot policy
before publishing the window, so a read of anything the owner did not sign still fails.
"""
import argparse
import os
import re
import sys
from pathlib import Path

ORIGINAL = 'panic("rootvp not authenticated after mounting");'

REPLACEMENT = '''/*
				 * fuxi (Xiaomi 13): the root volume on the unixnu partition is sealed by the
				 * owner's VerifiedRoot policy, not by Apple Image4, so APFS answers this ioctl
				 * with an error even though every block was checked against the owner's signed
				 * hash table. Report it and continue instead of panicking: refusing to boot here
				 * would leave the storage bring-up untestable, and the volume is the one the
				 * owner signed. Lowering this gate does not make an unverified volume readable:
				 * NativeUfs authenticates every 4 KiB block against the policy before publishing
				 * it, so a read of anything the owner did not sign still fails.
				 */
				printf("bsd_init: root volume is not Apple-sealed "
				    "(FSIOC_KERNEL_ROOTAUTH returned %d); continuing on the owner policy\\n", autherr);'''


def locate() -> Path:
    """The patch series is applied to the XNU working copy, which is not the tree this script runs in."""
    candidates = []
    for name in ('UNIXNU_XNU_WORK', 'XNU_WORK', 'UNIXNU_TREE'):
        value = os.environ.get(name)
        if value:
            candidates.append(Path(value))
    here = Path.cwd()
    candidates += [here / '.cache/xnu-work', here / 'build/xnu-work',
                   here.parent / '.cache/xnu-work', here.parent / 'build/xnu-work']
    for candidate in candidates:
        source = candidate / 'bsd/kern/bsd_init.c'
        if source.is_file():
            return source
    tried = '\n  '.join(str(c / 'bsd/kern/bsd_init.c') for c in candidates)
    raise SystemExit(f'build-fuxi-root-auth: bsd_init.c not found; tried\n  {tried}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sources', nargs='*', help='ignored; board.toml entries stay uniform')
    args = parser.parse_args()
    del args.sources

    source = locate()
    text = source.read_text(encoding='utf-8')

    if 'continuing on the owner policy' in text:
        print(f'build-fuxi-root-auth: already applied in {source}')
        return 0

    count = text.count(ORIGINAL)
    if count == 0:
        raise SystemExit(f'build-fuxi-root-auth: {ORIGINAL} not found in {source}; the kernel source moved')
    if count != 1:
        raise SystemExit(f'build-fuxi-root-auth: {count} matches in {source}, expected exactly one')

    # Require the ROOTAUTH ioctl shortly before the panic, so an unrelated panic with the same text is
    # never silently rewritten.
    match = re.search(
        r'(VNOP_IOCTL\(rootvnode, FSIOC_KERNEL_ROOTAUTH[^;]*;\s*\n\s*if \(autherr\) \{\s*\n)(\s*)'
        + re.escape(ORIGINAL), text)
    if not match:
        raise SystemExit('build-fuxi-root-auth: the ROOTAUTH block is not where it was; refusing to guess')

    head, indent = match.group(1), match.group(2)
    text = text[:match.start()] + head + indent + REPLACEMENT + text[match.end():]
    source.write_text(text, encoding='utf-8')

    result = source.read_text(encoding='utf-8')
    if 'continuing on the owner policy' not in result or ORIGINAL in result:
        raise SystemExit('build-fuxi-root-auth: the edit did not land')
    print(f'build-fuxi-root-auth: {source} now continues past the root seal check')
    return 0


if __name__ == '__main__':
    sys.exit(main())
