# 2026-11-14 the driver roots at unixnu: NativeUfs probe 10 on the device

Owner: XRC111. Device: Xiaomi 13 (fuxi, SM8550, 2211133C), 8 GiB + 256 GB, second unit.
Image: run 43558937264, built from `b152354` (device repository), boot.img sha256
`251959f4f326e1f25d9a503fca2ef18418ae1a135b560a7708923a4fd42959fd`.

Method: `fastboot boot` only.

## What changed since the survey

`scripts/build-arm-driver.py` (the device override) now substitutes the root partition name for
the ShengUfs build: ShengUfs 99d8bcd hardcodes

```c
static const char wanted[] = "userdata";
```

in `RootLayout.h` and has no build switch for it (upstream `main` is identical), so the build
rewrites that one line to `"unixnu"` in the throwaway build tree. That is the same arrangement
the aston port reaches by vendoring the driver with a `SHENG_UFS_ROOT_PARTITION` macro. The
match is exact and the build fails loudly if a revision rewords the line.

`board.toml` then runs `native-ufs-probe=10`, which asks for the read-only root check: adopt the
controller, read the LUN 0 GPT, find the named partition, read its first block and parse the
`SHNGROOT` manifest.

## Result

```
NATIVE_UFS root check
userdata first=32fc300 last=3afc2ff
block0 magic 00000000 00000000 ext4magic=0000 ext4blocks=0
manifest status=1 windows=0 @NativeUfs.cpp:2182
pages=0x000000000011ee7c
free=0x00000000001128fc
```

- `first=0x32fc300 last=0x3afc2ff` are exactly the `unixnu` bounds that
  `evidence/2026-11-14-dual-system-partition.md` created (LBA 53461760..61850367), so the
  driver matched the new partition, not `userdata` (which is now 3110976..53461759).
- `block0 magic 00000000` — the partition's first block is zeros, which is what is left of the
  old userdata tail at that offset: nothing has been written to `unixnu` yet.
- `manifest status=1` is `no manifest magic`, the documented "nothing is flashed here yet"
  answer. `windows=0` follows from it.
- The panic label still reads "userdata first/last" because that string is part of the message,
  not of the lookup; the numbers are the ones that matter.

## What this does and does not prove

Proved: the UFS transport, the SMMU stream, the GPT parse and the partition-name match all work
with the partition this board now has, read-only, with no window published and nothing written.

Not proved yet: the window path (`native-ufs-probe=11`) and root publication. Both need a real
root image in `unixnu`: the `SHNGROOT` manifest at offset 0, then the stripped `OS` and
`Cryptex1,SystemOS` disk images of the target IPSW. Root publication additionally needs an
owner-signed per-capacity policy bundle, because `NativeUfsBlock::signedVolumeUuid()` only
answers for a verified policy view and the board currently links an empty table.
