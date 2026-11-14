# 2026-11-14 Dual-system partition: a 32 GiB unixnu carved from the end of userdata

Owner: XRC111. Device: Xiaomi 13 (fuxi, SM8550, 2211133C), 8 GiB RAM + 256 GB, bootloader
unlocked, HyperOS 4.0.0.12.XMCCNXM. Second unit (the first one of `2026-11-13-first-boot.md`
stays untouched).

This is the first operation that changed the device. Everything before it was `fastboot boot`
only, and `AGENTS.md` otherwise forbids writing to the device. The owner asked for this route
by name (the aston layout: a partition carved from the end of `userdata`), confirmed that a
backup already existed, and approved the write; it is recorded here as an owner-authorized
exception, not as a change of the standing rule.

The procedure is UniXNU's documented **UniXNU partition (experimental)** flow
(`docs/zh/porting/dual-system-partition`, `scripts/dual_sys/partition.py`), applied with one
extension described below.

## Why the stock script refused

```
STOP: no free GPT entry
```

The fuxi GPT declares 32 entries and every one of them is occupied (indices 1..32, `userdata`
is 32). The script needs one free slot for `unixnu`.

The header, however, already reserves **16 KiB** for the entry array even though it declares
only 32 entries: `SizeOfPartitionEntry` 128 x 32 = 4096 bytes, but the usable range starts at
LBA 6, so LBAs 2..5 (16 KiB) belong to the primary array, and LBAs `total-5..total-2` belong to
the backup array with the header in `total-1`. That is exactly the UEFI layout for **128**
entries.

So the entry count can grow inside the space the disk already reserves:

- only the header `NumberOfPartitionEntries` (offset 80) changes 32 -> 128,
- the entry array is zero-padded from 4 KiB to 16 KiB with all 32 existing entries kept in
  place,
- the two array CRCs and the two header CRCs are recomputed,
- `FirstUsableLBA` (6), `LastUsableLBA` (61850618), the reserved sector counts, the protective
  MBR and every existing partition entry stay **byte-identical**.

Verified offline with the official parser before touching the device (`GPT(...)` re-checks
geometry, both copies, CRCs, ordering and GUID uniqueness):

```
current: count=32 blocks=4 usable=6..61850618
EXPANDED OK: count=128 blocks=4 usable=6..61850618 entries=32
free entry indices after expansion: first=33 total=96
userdata unchanged: index 32, 3110976..61850618
```

The patch is `scripts/dual_sys-gpt-entry-growth.patch` in this repository; upstream is
`OddlyLabs/UniXNU scripts/dual_sys/gpt.py` at the `fuxi` branch.

## The run

Recovery: TWRP 3.7.1_12-Xiaomi13_v9.0_A15, kernel 5.15.195-MorizukiNeko-260620LTSC, ADB root,
`/data` decryptable (mapped as `/dev/block/dm-8`). Host tool: `scripts/dual_sys/partition.py`
with the growth patch; the aarch64 static `f2fs-shrink` was cross-compiled in CI from
`scripts/dual_sys/f2fs-shrink.c` (Ubuntu `gcc-aarch64-linux-gnu -static`, self-tested under
`qemu-aarch64-static`).

`plan --size-gib 32`:

| | before | after |
| --- | --- | --- |
| `userdata` | LBA 3110976..61850618, 224.074 GiB | LBA 3110976..53461759, **192.073 GiB** |
| `unixnu` | - | LBA 53461760..61850367, **32.000 GiB** (GPT index 33, `/dev/block/sda33`) |
| F2FS | 58,739,639 blocks, 106.66 GiB free | 50,350,519 blocks (partition limit 50,350,784) |
| tail alignment gap | - | 1,028,096 bytes (0.98 MiB) |

`apply` ran twice. The first run did the F2FS shrink and then stopped:

```
VERIFIED: both F2FS superblocks fit the planned partition
STOP: userdata or a bind mount is still mounted (errno=0: Success); do not change GPT or blindly retry
```

TWRP had `/dev/block/dm-8` mounted at `/sdcard` (the decrypted mapping), and `umount /data` had
already succeeded, so the helper's "nothing may be mounted" gate fired. That is the documented
safe failure: **GPT still original**, filesystem already smaller than its partition, no data
moved back. `/sdcard` was unmounted, `/data` remounted so that the script's own `umount /data`
would succeed, and the second run completed:

```
VERIFIED: F2FS already fits; no resize needed
VERIFIED: unmounted filesystem fits the planned userdata partition
Writing backup GPT ... Backup GPT verified; writing primary GPT
DONE: leave /data unmounted. Reboot into recovery to refresh partition and encryption mappings.
GPT readback passed.
```

`verify --plan` after a recovery restart:

```
32  userdata   3110976   53461759   192.073
33  unixnu     53461760  61850367    32.000
PASS: GPT copies, kernel partition ranges and mounted F2FS size match the plan.
```

## Android afterwards

Cold boot to HyperOS succeeded: `sys.boot_completed=1`, `/data` reports 192G with 117G used and
75G free, 432 packages installed, and the user files are all readable
(`/storage/emulated/0`: DCIM/Camera 90 entries, DCIM/Screenshots 200, Download 138, Pictures 15,
39.6 GiB in total). `/dev/block/by-name/unixnu` is visible to Android as `sda33`; nothing was
formatted in it, so it still holds the old userdata bytes at that offset.

`/sdcard` (`/storage/self/primary`) is not listable from the `shell` user; `/storage/emulated/0`
is. That is pre-existing scoped-storage behaviour, not a result of this operation.

## State of the two units

- First unit: untouched by this operation.
- Second unit (this one): GPT primary and backup rewritten, `userdata` F2FS shrunk by 32 GiB,
  `unixnu` created and unformatted.

The bundle kept from the run (`plan.json`, `gpt-primary.before/after`, `gpt-backup.before/after`,
`apply.log`, `SHA256SUMS`) is the only record of the exact bytes that were replaced.
