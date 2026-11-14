#!/usr/bin/env python3
"""Sign the OS and SystemOS windows of the fuxi unixnu root image.

NativeUfs publishes a root window only when a verified policy view exists for
it, and `verified_root_policies` is what the kernel links. This script produces
the two owner-signed policies for the windows that `[userdata] windows` puts in
the image, then asks UniXNU's own `scripts/build-verified-root-probe.py` to
build the archive that carries them.

Inputs are the unchanged IPSW disk images (fetched with
`./unixnu ipsw fetch OS "Cryptex1,SystemOS"`); outputs are under `build/`:

    build/verified-root-os/{manifest.bin,header.bin,signature.bin,hashes.bin,
                            trusted-key.bin,manifest.json,verification.json}
    build/verified-root-systemos/...                   (the same set)
    build/verified-root-fuxi/keys/{owner.pem,owner-public.pem}
    build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a

The owner key is generated once and kept in build/; the same key's public half
is compiled into the kernel as `verified_root_key`, so a rebuilt policy set has
to be signed with the same key or the kernel must be rebuilt with the new one.

The manifest is version 1 of kits/VerifiedRoot/tools/manifest.py: a 160-byte
header, a 384-byte RSA-3072 PKCS#1 v1.5 SHA-256 signature over that header, and
the image's per-4 KiB SHA-256 table inline. The header carries the signed image
size, the block count, and the APFS volume UUID the kernel has to match, which
is why it cannot be derived offline or borrowed from another device.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The signed envelope: kits/VerifiedRoot/tools/manifest.py writes a 160-byte header followed by a
# 384-byte RSA-3072 PKCS#1 v1.5 SHA-256 signature.
ENVELOPE_BYTES = 160 + 384

OS_DMG = '044-76040-038.dmg'
SYSTEMOS_DMG = '044-76034-038.dmg'
# The APFS volume UUIDs of the two windows in iPadOS 17.7.11 (21H461) for
# iPad7,1. They are properties of Apple's images, not of this board: the first
# is the OS root volume the payload's root_match_volume_uuid selects.
OS_UUID = uuid.UUID('04769D26-2CA0-4825-B443-E2E5F88300F2')
SYSTEMOS_UUID = uuid.UUID('8846FE78-2910-4297-AF97-D59BB6F8FC16')

# Pins of the exact images: the same numbers `unixnu build userdata` records in
# userdata-root.json, so a policy can never describe a different image than the
# one that was flashed.
WINDOWS = {
    'os': dict(dmg=OS_DMG, window='OS', volume_uuid=OS_UUID,
               image_sha256='e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2',
               image_bytes=6077580800),
    'systemos': dict(dmg=SYSTEMOS_DMG, window='SystemOS', volume_uuid=SYSTEMOS_UUID,
                     image_sha256='b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f',
                     image_bytes=3303014400),
}


def fail(message: str) -> None:
    raise SystemExit('build-fuxi-root-policy: ' + message)


def ensure_keys(keys: Path):
    keys.mkdir(parents=True, exist_ok=True)
    private, public = keys / 'owner.pem', keys / 'owner-public.pem'
    if private.is_file() and public.is_file():
        return private, public
    subprocess.run(['openssl', 'genrsa', '-out', str(private), '3072'], check=True)
    subprocess.run(['openssl', 'rsa', '-in', str(private), '-pubout', '-out', str(public)],
                   check=True, capture_output=True)
    return private, public


def attach(dmg: Path) -> str:
    out = subprocess.run(['hdiutil', 'attach', '-nomount', '-readonly', '-noverify', '-noautofsck', str(dmg)],
                         capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        match = re.match(r'(/dev/disk\d+)\s', line)
        if match:
            return match.group(1)
    fail(f'could not find the attached device for {dmg}')


def detach(device: str) -> None:
    subprocess.run(['hdiutil', 'detach', device], capture_output=True, check=True)


def sign_window(name: str, spec: dict, ipsw: Path, keys: Path, private: Path, public: Path) -> Path:
    output = ROOT / 'build' / ('verified-root-' + name)
    metadata = output / 'manifest.json'
    if metadata.is_file():
        record = json.loads(metadata.read_text())
        if (record.get('image_sha256') == spec['image_sha256']
                and record.get('image_bytes') == spec['image_bytes']
                and record.get('volume_uuid') == str(spec['volume_uuid']).upper()):
            print(f'{name}: policy already present ({spec["image_sha256"][:16]}...)')
            return output

    dmg = ipsw / spec['dmg']
    if not dmg.is_file():
        fail(f'{dmg} is missing; run ./unixnu ipsw fetch OS "Cryptex1,SystemOS" first')

    spec_from_file = importlib.util.spec_from_file_location('verified_manifest',
                                                           ROOT / 'kits/VerifiedRoot/tools/manifest.py')
    module = importlib.util.module_from_spec(spec_from_file)
    spec_from_file.loader.exec_module(module)

    output.mkdir(parents=True, exist_ok=True)
    device = attach(dmg)
    try:
        raw = Path(device.replace('/dev/disk', '/dev/rdisk'))
        with raw.open('rb', buffering=1024 * 1024) as stream:
            record = module.make(stream, spec['image_bytes'], private, public,
                                 spec['volume_uuid'], 1, output)
    finally:
        detach(device)

    if record['image_sha256'] != spec['image_sha256']:
        fail(f'{name}: the raw image digest is {record["image_sha256"]}, not the pinned one')

    # build-verified-root-probe.py audits four real blocks of the original image: block 0, block 1,
    # the middle block and the last one. The OS image is not a whole number of 4 KiB blocks, so its
    # last sample is short and the assembler gets it zero-padded (the shape the sheng port uses).
    block_count = record['block_count']
    indices = [0, 1, block_count // 2, block_count - 1]
    lengths = [4096, 4096, 4096, spec['image_bytes'] - (block_count - 1) * 4096]
    device = attach(dmg)
    try:
        raw = Path(device.replace('/dev/disk', '/dev/rdisk'))
        with raw.open('rb', buffering=1 << 20) as stream:
            blocks = []
            for index, length in zip(indices, lengths):
                stream.seek(index * 4096)
                data = stream.read(length)
                if len(data) != length:
                    fail(f'{name}: short sample read at block {index}')
                blocks.append(data.ljust(4096, b'\0'))
    finally:
        detach(device)
    (output / 'original-samples.bin').write_bytes(b''.join(blocks))

    # Header-only shape. manifest.bin keeps just the 544-byte envelope (160-byte header plus the
    # 384-byte RSA-3072 signature) and the per-block SHA-256 table moves to the root image as the
    # "<window>.vroot" data object.
    #
    # Why: the two windows come to 1,483,785 + 806,400 blocks, so the inline tables are
    # 73,285,920 bytes of incompressible digests. Carried in the kernel they pushed boot.img to
    # 203.6 MiB and the fuxi ABL refused it ("Failed to load/authenticate boot image: Load Error":
    # it will not boot an image larger than the 192 MiB boot_a partition). NativeUfs reads the
    # header-only form by itself: it sees manifestBytes == VR_HEADER_BYTES + VR_SIGNATURE_BYTES,
    # takes the block count from header offset 32 and loads the table from the object named after
    # the manifest's own window name plus ".vroot".
    envelope = (output / 'manifest.bin').read_bytes()
    if len(envelope) != ENVELOPE_BYTES + block_count * 32:
        fail(f'{name}: manifest.bin is {len(envelope)} bytes, expected the envelope plus {block_count} digests')
    head, table = envelope[:ENVELOPE_BYTES], envelope[ENVELOPE_BYTES:]
    (output / 'manifest.bin').write_bytes(head)
    vroot = output / f'{spec["window"]}.vroot'
    vroot.write_bytes(table)
    (output / 'hashes.bin').write_bytes(table)

    # The shape build-verified-root-probe.py re-reads before it will assemble the archive.
    (output / 'verification.json').write_text(json.dumps(dict(
        image_bytes=spec['image_bytes'], blocks_verified=block_count,
        image_sha256=record['image_sha256'], volume_uuid=str(spec['volume_uuid']).upper(),
        generation=record['generation'], trusted_key_sha256=record['trusted_key_sha256'],
        actual_sample_indices=indices, actual_sample_lengths=lengths,
        real_c_signature_and_block_verification=True,
        original_image_modified=False, boot_root_authenticated=False,
        host_only=True, hardware_tested=False), indent=2) + '\n')
    print(f'{name}: signed {block_count} blocks for {spec["image_bytes"]} bytes '
          f'({record["image_sha256"][:16]}...); header-only envelope {len(head)} bytes, '
          f'table {len(table)} bytes -> {vroot.name}')
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ipsw-dir', type=Path,
                        default=Path(os.environ['UNIXNU_IPSW_DIR']) if os.environ.get('UNIXNU_IPSW_DIR') else None,
                        help='directory holding 044-76040-038.dmg and 044-76034-038.dmg '
                             '(default: $UNIXNU_IPSW_DIR, exported by ci/build.yml from ./unixnu ipsw fetch)')
    parser.add_argument('--no-archive', action='store_true',
                        help='write only the header-only envelopes and .vroot objects, skipping the kernel '
                             'archive (that step needs bsd_init.o.json, which only "build kernel" writes, '
                             'so an image-only run cannot do it)')
    parser.add_argument('sources', nargs='*', help='ignored; board.toml entries stay uniform')
    args = parser.parse_args()
    if not args.ipsw_dir:
        fail('pass --ipsw-dir or export UNIXNU_IPSW_DIR')

    keys = ROOT / 'build/verified-root-fuxi/keys'
    private, public = ensure_keys(keys)

    for name, spec in WINDOWS.items():
        sign_window(name, spec, args.ipsw_dir, keys, private, public)

    report = dict(shape='header-only', envelope_bytes=ENVELOPE_BYTES,
                  windows=[dict(window=spec['window'], directory=name,
                                envelope=str((ROOT / 'build' / f'verified-root-{name}' / 'manifest.bin').relative_to(ROOT)),
                                vroot=str((ROOT / 'build' / f'verified-root-{name}' / f'{spec["window"]}.vroot').relative_to(ROOT)),
                                block_count=(ROOT / 'build' / f'verified-root-{name}' / 'hashes.bin').stat().st_size // 32)
                           for name, spec in WINDOWS.items()])
    report['total_table_bytes'] = sum(entry['block_count'] * 32 for entry in report['windows'])
    (ROOT / 'build/verified-root-header-only.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'header-only: {report["total_table_bytes"]} table bytes kept out of the kernel')
    for entry in report['windows']:
        print(f'  --object {entry["window"]}.vroot={entry["vroot"]}')

    if args.no_archive:
        print('build-fuxi-root-policy: envelopes and .vroot objects only (--no-archive)')
        return 0

    # build-verified-root-probe.py reads the SystemOS image from .cache/firmware,
    # so point that at the fetched copy instead of duplicating 3 GB.
    cache = ROOT / '.cache/firmware/target-volumes'
    cache.mkdir(parents=True, exist_ok=True)
    for spec in WINDOWS.values():
        source, target = args.ipsw_dir / spec['dmg'], cache / spec['dmg']
        if not target.is_file() and source.is_file():
            target.symlink_to(source)

    # build-verified-root-probe.py compiles with bsd_init's recorded flags, which carry
    # -I build/xnu/ios-obj/EXPORT_HDRS/libsa; XNU only creates that directory for some targets, and
    # clang treats a missing -I as an error under -Werror.
    (ROOT / 'build/xnu/ios-obj/EXPORT_HDRS/libsa').mkdir(parents=True, exist_ok=True)

    recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json'
    if not recipe.is_file():
        fail(f'{recipe} is missing; the kernel archive needs a completed "unixnu build kernel", '
             'or run with --no-archive for the image-only outputs')

    result = subprocess.run([sys.executable, str(ROOT / 'scripts/build-verified-root-probe.py'),
                             '--component', 'os', '--include-systemos'], cwd=ROOT)
    if result.returncode:
        fail('build-verified-root-probe.py failed')

    archive = ROOT / 'build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a'
    if not archive.is_file():
        fail(f'{archive} was not produced')
    print(f'build-fuxi-root-policy: {archive.relative_to(ROOT)} is ready for the kernel link')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())




