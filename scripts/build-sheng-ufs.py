#!/usr/bin/env python3
"""Build ShengUfs's NativeUfs for the arm64 bring-up kernel.

Upstream ``build-arm-driver.py`` resolves the catalog id ShengUfs to its legacy
runtime name and then, for every arm64 (non-arm64e) build, compiles NativeUfs
with ``-DNATIVE_VERIFIED_BLOCK=1`` and ``-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1``
unconditionally.  That pulls in ``vr_open`` / ``vr_verify_block`` /
``vr_xnu_crypto`` and the policy data ``verified_root_key`` /
``verified_root_policies`` / ``verified_root_policy_count`` from
``kits/VerifiedRoot``.  The policy data is a real signed volume policy, which a
CI build does not carry (the image job removes the proprietary inputs), and the
UFS probes this board uses are default off and never read a block, so the
authenticated path is not linked here.

``boot_progress_poll`` is an optional observer: NativeUfs.cpp declares it strong
under NATIVE_UFS_SOURCE_API and weak otherwise.  A weak no-op definition in the
archive satisfies both forms, so the link holds either way.
"""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sheng-ufs'
OUT.mkdir(parents=True, exist_ok=True)

recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
if not recipe.is_file():
    raise SystemExit(f'missing compile recipe: {recipe}')
record = json.loads(recipe.read_text().strip().rstrip(','))
source = ROOT / 'drivers/ShengUfs/NativeUfs.cpp'
if not source.is_file():
    raise SystemExit(f'missing source: {source}')

# Reuse the source-kernel flags, but drop the authenticated policy handoff and
# the source-runtime ABI switch: neither has a policy archive behind it here.
DROP = ('-DNATIVE_VERIFIED_BLOCK', '-DNATIVE_UFS_SOURCE_API')
flags = []
index = 0
arguments = record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        index += 1
        continue
    if value == '-o':
        index += 2
        continue
    if value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
        index += 2
        continue
    if value.startswith('-W') or value.startswith(DROP):
        index += 1
        continue
    flags.append(value)
    index += 1

flags.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
              '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
              '-Wno-unnecessary-virtual-specifier',
              '-I', str(ROOT / '.cache/xnu-work/osfmk'),
              '-I', str(ROOT / 'drivers/ShengUfs')])
storage_abi = ROOT / 'build/storage-abi/include'
if storage_abi.is_dir():
    flags.extend(['-I', str(storage_abi)])

noop = OUT / 'boot-progress.c'
noop.write_text('/* Optional UFS boot observer; the source runtime archive normally carries it. */\n'
                '__attribute__((weak)) void boot_progress_poll (void) { }\n')


def compile_one(src, obj):
    command = flags + [str(src), '-o', str(obj)]
    (OUT / (obj.stem + '-command.json')).write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, cwd=record['directory'],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (OUT / (obj.stem + '.log')).write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-3500:])


native_obj = OUT / 'NativeUfs.o'
noop_obj = OUT / 'boot-progress.o'
compile_one(source, native_obj)
compile_one(noop, noop_obj)

archive = OUT / 'libShengUfs.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(native_obj), str(noop_obj)], check=True)
print('built', archive.relative_to(ROOT))
