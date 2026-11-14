# 2026-11-14 header-only root policies, and the ABL's 192 MiB limit

Owner: XRC111. Device: Xiaomi 13 (fuxi, SM8550, 2211133C), 8 GiB + 256 GB, second unit.

This entry records why the first signed image could not boot, what the bootloader actually
enforces, and the shape the policy has to take. It is the companion of
`2026-11-14-ufs-root-windows.md` (the windows read correctly) and
`2026-11-14-dual-system-partition.md` (the partition exists).

## The failure

The first image carrying real owner-signed policies was built from `f36001d` and measured
213,508,096 bytes (203.6 MiB). `fastboot boot` answered:

```
Sending 'boot.img' (208504 KB)                     OKAY [  8.915s]
Booting                                            FAILED (remote: 'Failed to load/authenticate boot image: Load Error')
```

Two facts came out of it:

**1. The ABL will not load an image larger than the `boot_a` partition.** `boot_a` on this
device is 201,326,592 bytes (192.0 MiB), and the image has to fit inside it even for
`fastboot boot`, which copies into RAM. Nothing in the documentation says this; the measured
numbers are the evidence. The earlier working images were 144,723,968 bytes (138.0 MiB), so the
limit had never been reached before.

**2. A refused boot marks the slot unbootable.** After the failure `fastboot getvar` showed:

```
slot-unbootable:a:yes
slot-successful:a:yes
slot-retry-count:a:6
```

so every reboot returned to the bootloader. `fastboot set_active a` clears it (`slot-unbootable:a:no`,
`slot-retry-count:a:7`) and the system then booted normally with `/data` unchanged (192 GiB, 117 GiB
used; the user files all present). **Recovery from a refused boot is one command, not a reflash.**

## Where the 65.6 MiB came from

`kits/VerifiedRoot/tools/manifest.py` writes a policy as a 160-byte header, a 384-byte RSA-3072
PKCS#1 v1.5 signature, and then one SHA-256 per 4 KiB block of the image:

| Window | Blocks | Table |
| --- | --- | --- |
| OS | 1,483,785 | 47,481,120 bytes |
| SystemOS | 806,400 | 25,804,800 bytes |
| **total** | **2,290,185** | **73,285,920 bytes (69.9 MiB)** |

SHA-256 digests do not compress, so `gzip` around the kernel image cannot help: the kernel grew
from 9,603,216 to 78,406,976 bytes compressed, and the boot image from 138.0 to 203.6 MiB.

## The header-only shape

`NativeUfs` already supports a second form. From `NativeUfs.cpp`:

```cpp
// A header-only policy carries its hash table in the root image instead of the boot image.
const bool headerOnly = manifestBytes == VR_HEADER_BYTES + VR_SIGNATURE_BYTES;
if (headerOnly) {
    uint64_t count = 0;
    for (unsigned i = 0; i < 8; ++i) count |= static_cast<uint64_t>(manifest[32 + i]) << (8 * i);
    hashBytes = count * 32;
    ...
    readPolicyTable(label, ..., hashBytes);   // from <window>.vroot
}
```

So the envelope stays 544 bytes in the kernel and the table moves into the root image as a
manifest **version 2 data object** named after the manifest's own window name:

| Object | Bytes | Offset in the image |
| --- | --- | --- |
| `OS.vroot` | 47,481,120 | `0x22f400000` |
| `SystemOS.vroot` | 25,804,800 | `0x232200000` |

`readPolicyTable` matches the name exactly (`"%s.vroot"` with the window name from the manifest),
so the objects are `OS.vroot` and `SystemOS.vroot`, not the names of the build directories. The
object size has to equal `block_count * 32` exactly, or the driver refuses the window.

`./unixnu build userdata --object NAME=FILE` writes them as version 2 objects with
`block_size 4096`, `flags 3`, which is the shape the loader expects; the image went from
9,382,658,048 to 9,457,106,944 bytes (8.81 GiB) by exactly the two tables.

## What the tooling has to do

- `scripts/build-fuxi-root-policy.py` signs a window, then writes `manifest.bin` as the 544-byte
  envelope and `<window>.vroot` as the table. `--no-archive` stops after that, because the kernel
  archive step needs `bsd_init.o.json`, which only a completed `unixnu build kernel` writes.
- The script works on the **build tree** (`out/<device>/<profile>/<config>/tree`), not on the
  device repository it lives in: `kits/VerifiedRoot`, `scripts/build-verified-root-probe.py` and
  `build/` are all the driver's. `--tree` (or `UNIXNU_TREE`) says which tree, and
  `./unixnu tree` assembles it without a kernel build.
- `.github/workflows/userdata-image.yml` does: `unixnu tree` → sign with `--no-archive` → read
  `build/verified-root-header-only.json` from the tree → `build userdata` with the two objects.

Run 43579306488 did exactly that and produced the image with both objects recorded; the log line
`userdata: object OS.vroot 47481120 bytes at 0x22f400000` is the receipt.

## Still open

The kernel half has to be built with the header-only envelope and the payload back at 192 MiB,
then booted against this image to see whether `configureVerification` accepts it and
`signedVolumeUuid` starts answering. That is the first boot where root publication becomes
possible; every earlier run only proved the reads.
