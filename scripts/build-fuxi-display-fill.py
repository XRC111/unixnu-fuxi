#!/usr/bin/env python3
"""Compile the fuxi scanout filler against the recorded XNU compile flags.

Same shape as scripts/build-fuxi-display.py: take the compile command XNU recorded for one of its own
objects, swap this file in, and put the result in a small archive the kernel link pulls in.

FuxiDisplayFill.cpp writes the frame buffer the firmware handed over. It does not subclass the Apple
display class, which keeps the compile surface to IOKit, pexpert and IOLog.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# bsd_init.o pulls in pexpert and IOLog, which is the surface this file uses.
RECIPE_OBJECT = 'bsd/DEVELOPMENT/bsd_init.o.json'


def fail(message):
    raise SystemExit('build-fuxi-display-fill: ' + message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path,
                        default=Path(os.environ['UNIXNU_TREE']) if os.environ.get('UNIXNU_TREE') else None,
                        help='UniXNU build tree (default: $UNIXNU_TREE)')
    parser.add_argument('sources', nargs='*', help='ignored; board.toml entries stay uniform')
    args = parser.parse_args()
    del args.sources
    tree = args.tree.resolve() if args.tree else ROOT

    recipe = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / RECIPE_OBJECT
    if not recipe.is_file():
        fail(f'{recipe} is missing; run "unixnu build kernel" once so XNU records its compile flags')
    record = json.loads(recipe.read_text().strip().rstrip(','))
    arguments = record['arguments']

    source = tree / 'kits/fuxi/FuxiDisplayFill.cpp'
    if not source.is_file():
        fail(f'{source} is missing')
    out = tree / 'build/fuxi-display-fill'
    out.mkdir(parents=True, exist_ok=True)
    destination = out / 'FuxiDisplayFill.o'

    command, index = [], 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            # The recipe comes from a .c object, so a .cpp replacement needs -x c++ before the input file;
            # after it clang answers "-x c++ after last input file has no effect".
            command.extend(['-x', 'c++', str(source)])
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1

    # No -Werror: the kernel headers warn under the flags the recipe carries, and those warnings are not
    # this file's to fix. Its own mistakes still show up in compile.log.
    result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (out / 'compile.log').write_text(result.stdout)
    if result.returncode:
        fail(result.stdout[-3500:])

    blob = destination.read_bytes()
    if blob[:4] != b'\xcf\xfa\xed\xfe':
        fail(f'{destination} is not a 64-bit Mach-O object')
    archive = out / 'libFuxiDisplayFill.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, str(destination)], check=True)
    print(f'build-fuxi-display-fill: {archive.relative_to(tree)} '
          f'({archive.stat().st_size} bytes from {len(blob)} of object)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
