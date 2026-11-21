# 2026-11-21 the OS window holds one APFS volume, and the Data volume is a separate input

Owner: XRC111. Device: Xiaomi 13 (fuxi, SM8550, 2211133C), 8 GiB + 256 GB.

This entry closes the question the previous two left open. It reads the APFS container inside the OS
window directly, compares this board's configuration with the port that boots the same image to the home
screen, and records what the Data volume actually is.

## The OS window is one APFS container with one volume

`probe-gpt.mjs` had reported a container superblock at window offset `0x4420`, and that offset is
`nx_magic`, not the object start: a byte dump shows `4e585342` (`NXSB` little-endian) there, so the
container begins at window `+ 0x4400`. Reading it:

```
container at 1048576+0x4400: magic 0x4253584e block_size 4096 block_count 1483776
  container 5.66 GiB, window 5.66 GiB
```

`probe-apfs-volumes.mjs` then scans that container for volume superblocks, and there is exactly **one**:

```
volume at block 15896: name "Update21H461.J120J121J207J208OS"
                        uuid 04769d262ca04825b443e2e5f88300f2  role 1
```

Three things follow:

- The UUID is `04769D26-2CA0-4825-B443-E2E5F88300F2`, byte for byte the `root_match_volume_uuid` in
  `board.toml`. The kernel mounts this volume because it asks for exactly this identity.
- `role 1` is the system role. There is **no second volume in this container**, so there is no Data
  volume, and no dyld cache: the earlier scan of the whole window found zero `dyld_shared_cache` strings
  and no `dyld_v1` header, and this is why.
- The name ends in `OS` and starts with `Update21H461`, the update volume naming APFS uses for the
  staged system. That is consistent with the window being the `OS` component of the IPSW.

So the `5/10` probe result is now fully explained: `launchd`, `/usr/lib/dyld`, `/System/Cryptexes`,
`/private/var` and `/private/preboot` exist on the system volume, while `libSystem.B.dylib` and both dyld
cache directories do not, because the volume that would carry them is not in the image at all.

## The Data volume is not in the root image

`docs/zh/concepts/userdata.md` says it plainly:

> 可写 Data 卷独立于 userdata 镜像，来自负载中的 Data ramdisk 种子。

That is the missing step, and it is a build input rather than something to mount: the Data volume is a
seed file passed to the payload (`./unixnu build payload --data-ramdisk data.img`), grown to size with
`scripts/seed-data-grow.py --gib N base.dbr out.dbr`. Growing it needs `hdiutil` and `diskutil`, so it
runs on a macOS host, and the seed is a `.cdr` raw APFS container that no repository here carries — it
has to be produced once and kept as a build input.

The kernel side is ready for it: `devicetree.py` writes `ephemeral-storage = 1` into `/chosen` when a
seed is present and `persistent_data` is not set, which is what marks the store as throwaway, and
`--persistent-data` is what keeps it across boots.

## What the working port does differently

`Kerfand/UniXNU-Fuxi-Arm64e-iOS17` boots this same iPadOS 21H461 image on this same board to the home
screen. Its `device/board.toml` matches this repository's on every value that decides which volume is
the root:

| Key | Here | There |
| --- | --- | --- |
| `root_match_volume_uuid` | `04769D26-…` | `04769D26-…` (same) |
| `[userdata] windows` | `OS`, `Cryptex1,SystemOS` | same |
| `owned_memory_size` | `0x130000000` | `0x130000000` (same) |
| `packed_data_ramdisk` | `true` | `true` (same) |
| `persistent_data` | absent | `true` |
| `ipad_identity` | absent | `true` |
| arguments | `native-ufs-root=1` | `-apfs_shared_datavolume`, `offline-apfs-load=1`, `sheng-data-persist=2` |

Two of those differences are not copyable as they stand:

- **`ipad_identity` does nothing under schema 2.** This board is `schema_version = 2`, and its ADT comes
  from `scripts/unixnu/devicetree.py`, which never reads that key; it is consumed only by the older
  shell path in `payload.py` that calls `make-adt.py --ipad-identity`. Copying it would look like a
  change and change nothing.
- **`persistent_data` is conditional.** `devicetree.py` only reads it when a Data seed is present, so
  without `--data-ramdisk` it is silently inert.

What is genuinely usable now is the pair `-apfs_shared_datavolume` with a Data seed, plus `persistent_data`
once that seed exists.

## Also worth keeping

The reference port's `board.toml` notes that a 320 MiB low window did boot into launchd but left about
32 MiB free, after which jetsam kept killing SpringBoard and powerd; that is why it asks for
`0x130000000` here too. It also states the platform seed is not stored in any repository, which matches
`unique_chip_id = "required"` in this one.

## Not measured

- Whether the `SystemOS` window (the cryptex, 3.08 GiB, a bare container) has anything the Data seed
  needs. The two are separate: the seed carries the writable half, the cryptex carries system content.
- Any Data seed: none exists here yet, so `persistent_data` and `data-persist=2` are untested on this
  device.
