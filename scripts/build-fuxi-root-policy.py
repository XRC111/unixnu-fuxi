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
    'os': dict(dmg=OS_DMG, volume_uuid=OS_UUID,
               image_sha256='e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2',
               image_bytes=6077580800),
    'systemos': dict(dmg=SYSTEMOS_DMG, volume_uuid=SYSTEMOS_UUID,
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

    # The shape build-verified-root-probe.py re-reads before it will assemble the archive.
    (output / 'verification.json').write_text(json.dumps(dict(
        image_bytes=spec['image_bytes'], blocks_verified=record['block_count'],
        image_sha256=record['image_sha256'], volume_uuid=str(spec['volume_uuid']).upper(),
        generation=record['generation'], trusted_key_sha256=record['trusted_key_sha256'],
        real_c_signature_and_block_verification=True,
        original_image_modified=False, boot_root_authenticated=False,
        host_only=True, hardware_tested=False), indent=2) + '\n')
    print(f'{name}: signed {record["block_count"]} blocks for {spec["image_bytes"]} bytes '
          f'({record["image_sha256"][:16]}...)')
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ipsw-dir', type=Path,
                        default=Path(os.environ['UNIXNU_IPSW_DIR']) if os.environ.get('UNIXNU_IPSW_DIR') else None,
                        help='directory holding 044-76040-038.dmg and 044-76034-038.dmg '
                             '(default: $UNIXNU_IPSW_DIR, exported by ci/build.yml from ./unixnu ipsw fetch)')
    parser.add_argument('sources', nargs='*', help='ignored; board.toml entries stay uniform')
    args = parser.parse_args()

    keys = ROOT / 'build/verified-root-fuxi/keys'
    private, public = ensure_keys(keys)

    for name, spec in WINDOWS.items():
        sign_window(name, spec, args.ipsw_dir, keys, private, public)

    # build-verified-root-probe.py reads the SystemOS image from .cache/firmware,
    # so point that at the fetched copy instead of duplicating 3 GB.
    cache = ROOT / '.cache/firmware/target-volumes'
    cache.mkdir(parents=True, exist_ok=True)
    for spec in WINDOWS.values():
        source, target = args.ipsw_dir / spec['dmg'], cache / spec['dmg']
        if not target.is_file() and source.is_file():
            target.symlink_to(source)

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



