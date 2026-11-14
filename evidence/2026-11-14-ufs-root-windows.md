# 2026-11-14 the flashed root windows read back correctly (probe 11)

Owner: XRC111. Device: Xiaomi 13 (fuxi, SM8550, 2211133C), 8 GiB + 256 GB, second unit.
Image: run 43567192243, built from `e445536` (device repository), boot.img sha256
`…` / kernel as delivered in artifact 14102618109. Booted with `fastboot boot` only.

## What was on the device before this run

`evidence/2026-11-14-dual-system-partition.md` created `unixnu` (LBA 53461760..61850367,
32 GiB, `/dev/block/sda33`) and shrank Android's `userdata` to 192.073 GiB. `userdata-root.img`
(8.74 GiB, sha256 `fd43ee7035ce185ce4215ac93fdc006fd784a54b3b7442e21457c226cbe03093`) was then
written into it through TWRP's root adb:

```
adb exec-in 'dd of=/dev/block/by-name/unixnu bs=8M conv=fsync'
```

Windows fastboot cannot do this: it buffers the whole image and exits with
`libc++abi: terminating due to uncaught exception of type std::bad_alloc`. `adb shell` cannot
do it either, because its PTY corrupts a binary stdin (dd then reports a short read and the pipe
dies with `write EOF`); `adb exec-in` has no PTY and worked. dd reported the full
9,382,658,048 bytes. The claimed throughput (~1 GiB/s) is not credible for USB, so the write was
verified from the device side instead: the partition's first bytes are the `SHNGROOT` magic, and
this probe recomputes each window's SHA-256 from the transport.

## Result

```
NATIVE_UFS root windows
userdata first=32fc300 last=3afc2ff manifest status=0 windows=2
w0 OS        off=100000     size=16a408600  gpt=20494645 54524150 read=1
w1 SystemOS  off=16a600000  size=c4e00000   gpt=00000000 00000000 0 read=1
w0 sha e5634d04 a8ac35f0 24845f58 3394e0a3
w1 sha b4908cde 41d23e6b c05e6af5 1a2c9eeb @NativeUfs.cpp:2215
```

Checked against the image report (`userdata-root.json`) and the IPSW pins:

| Field | Probe | Report |
| --- | --- | --- |
| `manifest status` | `0` (ok) | — |
| `windows` | `2` | 2 |
| OS offset | `0x100000` | 1048576 |
| OS size | `0x16a408600` = 6,077,580,800 | 6077580800 |
| SystemOS offset | `0x16a600000` | 6079643648 |
| SystemOS size | `0xc4e00000` = 3,303,014,400 | 3303014400 |
| OS sha256 prefix | `e5634d04 a8ac35f0 24845f58 3394e0a3` | `e5634d04…` |
| SystemOS sha256 prefix | `b4908cde 41d23e6b c05e6af5 1a2c9eeb` | `b4908cde…` |

`gpt=20494645 54524150` is the OS window's sector 1, `"EFI " "PART"` little-endian: the OS window
holds a real GPT disk. The SystemOS window has zeros there because it is a bare APFS container,
not a GPT disk, which is expected.

So the storage path is complete: the partition geometry, the image bytes, the `SHNGROOT` manifest,
the manifest's window placement, and the transport's reads all agree with the signed-off report,
and the SHA-256 the driver computes over each window is the SHA-256 of Apple's original image.

## What is still missing

`configureVerification` is what turns these windows into a published root, and it needs the
owner-signed policy bundle (the `VROOT01` envelope that `kits/VerifiedRoot/tools/manifest.py`
produces). This run exercised the probe path, which never reads the policy. The policy plumbing
is `scripts/build-fuxi-root-policy.py` plus `CONFIG_XNU_EXTRA_LD_ARCHIVES` pointing at
`build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a`; the next run tests the link, and the
one after it turns `native-ufs-probe` into `native-ufs-root`.
