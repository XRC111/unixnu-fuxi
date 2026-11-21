# 2026-11-21 the root image dissected offline: the dyld cache is in the other window

Owner: XRC111. Device: Xiaomi 13 (fuxi, SM8550, 2211133C), 8 GiB + 256 GB.

Image: run 44886794079, built from `15bdc0b` of `XRC111/unixnu-fuxi`. Booted with `fastboot boot`
only, nothing written. The panel stopped on the same text as 2026-11-14:

```
initproc failed to start -- exit reason namespace 6 subcode 0x1
description: Library not loaded: /usr/lib/libSystem.B.dylib
 Referenced from: <4B525992-8C69-315E-A51A-CC9433E45B51> /sbin/launchd
 Reason: tried: '/usr/lib/libSystem.B.dylib' (no such file, no dyld cache)
```

That is the same abort the previous run reached — but this time the root image was read on the host, and
the reason is no longer a guess.

## What the two windows actually contain

`userdata-root.img` (9,457,106,944 bytes, sha256 `f7c29dc9…`) was read with `scripts/probe-apfs.mjs`
and `scripts/probe-gpt.mjs`, which take the window offsets from `userdata-root.json` and report what
sits at them.

| Window | Offset | Container superblock | Structure |
| --- | --- | --- | --- |
| `OS` | `0x100000` | **none** (`nx_magic` = 0) | `EFI PART` protective MBR + GPT header (rev `0x10000`, 128 entries × 128 bytes), one partition `7C3457EF-0000-11AA-AA11-00306543ECAC` at LBA 34..11870241 (5796.0 MiB) |
| `SystemOS` | `0x16a600000` | `0x4253584e` (`NXSB`), 806400 blocks × 4096 = 3.08 GiB | bare APFS container |

The `OS` window is a **GPT disk**, not a container: its APFS volume superblock is inside the
partition, which is why `vfs_mountroot` mounts it and `/sbin/launchd` resolves. This matches the
`gpt=20494645 54524150` the driver reported in `2026-11-14-ufs-root-windows.md` — that value is the
partition table's sector 1, `"EFI " "PART"` little-endian.

## Where the dyld cache is

A full scan of each window for `dyld_shared_cache` and for the mapping header magic `dyld_v1`:

| Window | `dyld_shared_cache` occurrences | `dyld_v1` magic | `libSystem.B.dylib` occurrences |
| --- | --- | --- | --- |
| `OS` | **0** | none | 4 |
| `SystemOS` | **253** | present | 3345 |

The `SystemOS` names are the real cache, not references to it:

```
dyld_shared_cache_arm64.01 … dyld_shared_cache_arm64.52
dyld_shared_cache_arm64.32.dylddata
dyld_shared_cache_arm64.34.dyldlinkedit
dyld_shared_cache_arm64.symbols
dyld_shared_cache_arm64
```

So `(no such file, no dyld cache)` is literally correct about the volume that is mounted: the OS
window has no cache, and the cache is in `SystemOS`, which nothing mounts.

This settles the two hypotheses the previous entry left open. It is not "the wrong volume was mounted
as root" — `launchd` comes from the mounted one and resolves — it is "the second volume is not mounted
at all".

## Why the probe report is not on the panel

`FuxiRootCheck` runs in `bsd_init` after the root vnode is set, and the boot reached
`initproc failed to start` afterwards, which is later than the probe's call site. The probe therefore
ran and its `FuxiRoot:` lines went to the serial console and the verbose log; by the time the panel
stopped on the abort, they had scrolled away. The report has to survive the stop to be readable, so
`fuxi-root-check=2` now panics with the summary (the panic text stays on the panel).

The probe also prints the volume it mounted (`rootvnode->v_mount->mnt_stat.f_mntfromname`, its
filesystem type, and `rootdevice`), because "which window answered" is the fact the next decision
turns on.

## What the mount has to be

XNU's own route to a second volume is `kernel_mount()` / `mount_common()` in `bsd/vfs/vfs_syscalls.c`;
the user-space `mount` syscall is a thin wrapper over the same function. The public helper
`vfs_mount_at_path()` refuses anything except `lifs` and `nfs`, so a cryptex mount would have to call
`kernel_mount` directly.

The shape Apple uses for cryptexes is in `bsd/sys/_types/_graftdmg_un.h`:

```c
typedef struct secure_boot_cryptex_args {
    u_int32_t sbc_version;
    u_int32_t sbc_4cc;
    int sbc_authentic_manifest_fd;
    int sbc_user_manifest_fd;
    int sbc_payload_fd;
    u_int64_t sbc_flags;
} secure_boot_cryptex_args_t;
```

The payload arrives as **file descriptors**, not as a device path, and the authentication types
(`CRYPTEX1_AUTH_ENV_GENERIC`, `CRYPTEX_AUTH_PDI_NONCE`, …) appear in the open-source tree only as
declarations in `bsd/sys/mount.h` — the implementation lives with the APFS component, not in XNU. So a
kernel-side cryptex mount is not a matter of calling an existing open function with a path.

The practical consequence for this port is that the second volume has to be reachable as a block
device the APFS component will accept, which is a different problem from "call mount somewhere else",
and the next entry starts from that.

## Not measured

- Whether the `SystemOS` container's own volume is the one holding `/System/Cryptexes/OS`: the scan
  proves the cache is in that window, not which path inside it.
- Whether the APFS component in the prelinked kernel will mount a bare container that is not a GPT
  partition (the `OS` window is a GPT disk, `SystemOS` is not).
- Any mount attempt: this run only read paths.
