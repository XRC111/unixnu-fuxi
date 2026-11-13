# 2026-11-13 first boot: fuxi-touch report reached on Xiaomi 13 (HyperOS 4.0)

Owner: XRC111. Device: Xiaomi 13 (fuxi, SM8550, model 2211133C, China), 8 GiB RAM + 256 GB storage,
slot a, bootloader unlocked, HyperOS 4.0.0.12.XMCCNXM (Android 17 base).

## Result

One `fastboot boot` (temporary, nothing written to the device) reached the touch probe's panic
report on the first attempt, matching or exceeding the furthest public records
(chipppppppps evidence/2026-11-02, images s2-smp / s3-selfipi / h4-hid / r3-ramblock) in a
single image:

| Stage | Observed | Record comparison |
| --- | --- | --- |
| ABL accepts the v0 image | `Sending 'boot.img' ... OKAY [5.303s]` / `Booting OKAY` | Same as run 41273003819. **First confirmation that the HyperOS 4.0 ABL (OS4.0.0.12) still takes the v0 container** — the stock base is newer than the one the firmware DTB was synthesized from (OS3.0.309). |
| DEBUG firmware log on the panel, ending at `SecurityLock ... LockCapsuleInterface` | Same text as the record | exit-boot-services normal message; DARWINBOOT lines covered by the bands. |
| UniXNU menu, DarwinBoot hand-off | Green band, then blue, then magenta | Matches the entry-probe sequence. |
| Touch probe triggered by a real tap | Report below. | One image covers what the records showed across s2/s3/h4/r3. |

## Touch report transcription (photograph IMG of 2026-11-13 21:4x, owner's unit)

```
fuxi-touch report (stage 4 of 4)
FTS s4/4 st10 e0 vote443c0200 h00000000 80000002 0004000 00
     00400 00004000 gpe000005
FTS p0000085 0000085 0000085 0000097 rst0003f2c3 irq000
     00c3 hu30090000 L2/4 12000000Hz
FTS pw vdd40900 avdd41b00 tcs2 rc0 w-1 id3640 rc0 rst0/1 sp
     40 ev03000 sense0
FTS rd56 ev57 in1 mv53 up1 er0/00 rdy0 rearm0 c0 6319,15011
     hid44 irq55/0 xf0
SMP run7 c1 b0 cpu5 av8 st7777777 f0000000 psci0,0,0,0,0,0,0
     ipi5453/5458
SMP tmr10/2/2/53265/25908/1074/2881/23 ipi32/6/1/527/3820/10
     46/21/6
HID st6 f0 fam0 evd0 sz208 res1 meta0 dev4 drv1 up1 tk340/1
     rep44/0 edge1
SYS root apfs /dev/disk1s1 1:launchd 3:restored_externa 4:mo
     unt_tmpfs n3 @FuxiTouchProbe.cpp:590
frames fffffff007a2a128 fffffff0079840a0 fffffff0073fb4a4
pages=0x000000000011eec0
free=0x000000000010ee59
```

Reading:

- `id3640`: the FTM5 chip id over GENI SPI, rails on (`vdd40900 avdd41b00`, RPMh tcs2), reset done
  (`rst0/1`) — the power-rail and reset path of the touch controller works on this unit.
- `in1 ... up1 ... c0 6319,15011 ... irq55/0`: one contact, lift, coordinates in the panel's native
  range, the interrupt fired, `er0/00` (no transfer errors) — the full sense→IRQ→probe chain works.
- `SMP run7 ... st7777777 f0000000 psci0,0,0,0,0,0,0`: all seven secondary cores `CPU_ON` returned 0
  and reached step 7 (running), no failure bits — same as image s3-selfipi, plus per-core
  timer/IPI counters on the tmr/ipi row (all cores taking timer interrupts and IPIs).
- `HID ... drv1 up1 ... rep44/0`: the HID event driver attached and 44 contact sets went out as HID
  reports (record h4-hid showed drv1 rep68).
- `SYS root apfs /dev/disk1s1 1:launchd 3:restored_externa 4:mount_tmpfs n3`: the APFS root from the
  packed restore ramdisk mounted, userland runs (launchd, restored_external, mount_tmpfs) — the
  record r3-ramblock state.
- `@FuxiTouchProbe.cpp:590`: the probe's own planned panic (`fuxi-touch-panic=2` after the first
  lift), not a fault.

## Image identity

- Built by the Build workflow of `XRC111/unixnu-fuxi` at commit `ed31173e88e730e37e93aa8ec30f59f3602d7e76`
  (run 43220330720, `fuxi-boot-image-encrypted-1`), which is chipppppppps/unixnu-fuxi `main`
  (33d132c) with only `ci/boot-image-recipient.pem` replaced by the owner's key.
  Upstream pins: OddlyLabs/UniXNU fuxi @ 6ae4c252937ae1920ab68eaded10fc78447e0194,
  OddlyLabs/uefi-fuxi fuxi @ c5793bc2c60a2e2f5d9fbaad8717ed3f2736bfd2.
- boot.img sha256 `f4813a48cb7ca90dfb95e95c9b1894a67fbbea27302c4292bce67a80d9d9dacb`,
  kernel sha256 `1232b4f4ef0f83a1073299f683a2e6c6a4e7beb07581a3403aacf6d233af9115` (boot.json),
  prelinked kernel.macho sha256 `8fae41439b28180b7c24fbca0caa9aaa646b8247ca59a8b8531bbd12c3d9bc30`.
- Boot args: `-v serial=3 debug=0x14e kt_fixed_zones=120 sheng-pmgr=2 sheng-smp=1 armcpu-spi-ipi=1
  armcpu-self-ipi=1 fuxi-touch=4 fuxi-touch-hid=1 sheng-touch-hid=1 fuxi-touch-panic=2 rd=md0-ramblock`.
- Platform seed is the bring-up value (`0x0123456789abcdef`), not this unit's own.

## Notes for the next attempt

- The HyperOS 4.0 ABL accepting the v0 container extends the firmware DTB / ABL compatibility range
  recorded for OS3.0.309; worth re-checking after every HyperOS OTA.
- The USB receiver path was not exercised (the payload rode in the boot image); `FuxiBootMemoryDxe`
  published it, per boot.json `payload_carrier: image`, `payload_bytes: 201326592`.
- Next milestones: boot without the touch panic (`fuxi-touch=0`) to let bsd_init continue past the
  probe (record's next stop is the root device wait → watchdogd on the ramdisk root); replace the
  bring-up seed with the unit's own before any payload carries user data.
