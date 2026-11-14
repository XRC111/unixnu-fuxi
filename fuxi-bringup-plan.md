# fuxi bring-up plan

What is verified on the device, what is not, and what each remaining layer needs, with the values
already extracted. Every number names where it came from: the live device tree
(`facts/gpu_memory_facts.json`, `facts/panel_facts.json`, `/proc/cmdline`), the image reports
(`userdata-root.json`, `boot.json`), or a device run recorded under `evidence/`.

Status as of 2026-11-14 evening:

| Layer | State | Evidence |
| --- | --- | --- |
| Firmware → boot manager → DarwinBoot | works | `evidence/2026-11-13-first-boot.md` |
| Touch (FTS) probe + HID reports | works | same, `facts/touch_facts.json` |
| RAM-block root (restore ramdisk) | works | `evidence/2026-11-02-first-boot.md` |
| UFS transport, GPT read | works | `evidence/2026-11-14-ufs-gpt.md` |
| `unixnu` partition (32 GiB) + F2FS shrink | works | `evidence/2026-11-14-dual-system-partition.md` |
| Root image written and read back | works | `evidence/2026-11-14-ufs-root-windows.md` |
| Owner-signed policy linked into the kernel | in progress | `scripts/build-fuxi-root-policy.py` |
| UFS root mount (no RAM disk) | not yet | needs the policy + root mode |
| Display bring-up (panel init, DPU) | not started | values below |
| GPU (Adreno 740 + Metal) | not started | ZAP/SQE/GMU pinned |
| Battery, audio, Wi-Fi, Bluetooth, USB | not started | nodes below |

## 1. Storage: three steps left

The partition, the image and the manifest are done. What remains:

**1.1 Policy archive on the link line.** `scripts/build-fuxi-root-policy.py` signs both windows and
builds `build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a`; `configs/fuxi_defconfig` puts it
on the kernel link through `CONFIG_XNU_EXTRA_LD_ARCHIVES`, and `ci/build.yml` fetches the IPSW
windows first (`./unixnu ipsw fetch OS "Cryptex1,SystemOS"`, exported as `UNIXNU_IPSW_DIR`). The
archive carries ~70 MB of block hashes, so the prelinked kernel grows and the boot image has to stay
inside what DarwinBoot accepts.

**1.2 Root mode.** Replace in `board.toml` `[payload] kernel_args`:

```
-native-ufs-probe=11 +native-ufs-root=1
```

and in `[payload.adt]` set

```
root_match_volume_uuid = "04769D26-2CA0-4825-B443-E2E5F88300F2"
```

That UUID is the OS root volume of iPadOS 17.7.11 (iPad7,1) and is what the signed policy's header
carries (`scripts/build-fuxi-root-policy.py`, `OS_UUID`), so the volume the driver publishes and the
one the kernel asks for match. `rd=md0-ramblock` can stay for the first attempt: it only selects the
RAM-disk root when a RAM disk is present, and with `native-ufs-root=1` the UFS windows publish
first.

**1.3 Data seed.** `build payload --data-ramdisk` wants a Data image sized for the Data allocation
range; `scripts/seed-data-grow.py --gib N` grows the packed seed on macOS. The Data window lives in
the same `unixnu` partition after the two signed windows, so the seed must fit in
32 GiB − (OS + SystemOS + manifest).

## 2. Display: everything measured, nothing implemented

`NativeLegacyDisplay` or the firmware GOP framebuffer are the two paths; sheng uses
GopFramebuffer for bring-up and hands the panel to NativeLegacyDisplay later.

Panel (from the live tree, `qcom,mdss_dsi_m3_38_0c_0a_dsc_cmd`, and `/proc/cmdline`
`msm_drm.dsi_display0=` selects it at runtime):

| Item | Value |
| --- | --- |
| Resolution | 1080 × 2400 |
| Mode | DSI command mode, burst, DSC 8 bit/component, 8 bit/pixel |
| DSC slices | 540 × 40, 2 slices per packet |
| DSI clock | 1,100,000,000 Hz |
| Timings | h front/back/pulse 124/124/32, v front/back/pulse 128/132/24, skew 0 |
| Frame rates | timing@0 60 Hz, timing@1 120 Hz, timing@2 90 Hz |
| Physical size | 66.3 mm × 147.3 mm (mdss-pan-physical-*-dimension) |
| Reset | `qcom,mdss-dsi-reset-sequence = <0 1 1 10>` |
| Backlight | `bl-min-level` 4, `bl-max-level` 4095, PMIC controlled, inverted dBV |
| Panel commands | `facts/panel_cmds_*.txt` (on/off/timing-switch/doze/HBM/FOD/dimming) |

Controller nodes: `qcom,mdss_mdp@ae00000` (IRQ 83), `qcom,mdss_dsi_ctrl0@ae94000`,
`qcom,mdss_dsi_phy0@ae95500`, panel rails in `dsi_panel_pwr_supply_M3`, TE GPIO in
`qcom,dsi-display-primary`.

Order of work: get a static frame out of the firmware framebuffer first (already the current state),
then panel init (on-command 718 bytes), then DPU scanout with the 120 Hz timing.

## 3. GPU: firmware in hand, driver not wired

Pinned in `scripts/data/fuxi-gpu-inputs.json`, files under `.cache/firmware/fuxi-gpu/`:

| File | Bytes | sha256 |
| --- | --- | --- |
| `a740_zap.mbn` | 12088 | `d7b9ec43…` |
| `a740_sqe.fw` | 76724 | `96fee336…` |
| `gmu_gen70200.bin` | 66844 | `1a2a419c…` |

The GMU image is named after the GMU generation, not the GPU: searching for `a740_gmu.bin` finds
nothing on this device.

From the tree:

- `qcom,kgsl-3d0@3d00000`: reg windows `0x3d00000+0x40000`, `0x3d61000+0x800` (cx_dbgc),
  `0x3d50000+0x10000` (rscc), `0x3d8b000+0x2000` (isense), `0x3d9e000+0x1000` (cx_misc),
  then the QDSS windows at `0x10900000`, `0x10048000`, `0x10b05000`; IRQ 300;
  `qcom,initial-pwrlevel` 4.
- `qcom,gmu@3d69000`: IRQ 304 and 305, `qcom,gmu-freq-table = <220000000 0x40 550000000 0x80>`,
  iommu specifier `<0xdd 0x5 0x0>`.
- Power levels (9): 692, 680, 590, 475, 401, 348, 348, 295, 220 MHz.
- DDR bus table: `0 0x209a8e 0x2dc6c0 0x5caf6a 0x65ce03 0x7cb163 0xa3140c 0xbdf5c2 0xdbb3e5 0xfbc520`.
- ZAP carveout: `gpu_micro_code_region@9b09a000`, 8 KiB.

Order of work: add `Adreno740` + `AdrenoMetal` + `Sm8550Platform` to `[components]`, feed the three
pinned images, then bring up CP/GMU and submit one command buffer before touching Metal.

## 4. Platform identity

`ro.serialno` is `a3130473`; `ro.boot.chip_id` is empty on this build; `persist` is
`/dev/block/sda18` (64 MiB) and `devinfo` is `/dev/block/sda15` (16 MiB). The payload currently
takes the bring-up seed `0x0123456789abcdef` from `ci/build.yml`; the real value has to be decided
before userland, either derived from `persist` or fixed per unit. It feeds the device tree the
payload hands to XNU, so changing it changes the board identity the kernel sees.

## 5. Later tracks, where to start

- **Battery**: TI/Qualcomm gauge nodes are in the tree; aston reads the pack's bq28z610 directly
  over I2C. fuxi's charger and gauge nodes still need naming from the live tree.
- **Wi-Fi/Bluetooth**: Qualcomm `kiwi`/`kiwi_v2` (WCN7850 class); `/vendor/firmware/wlan/qca_cld/`
  holds only links, the MAC comes from `/mnt/vendor/persist`. The firmware itself is in the vendor
  image, not in `/vendor/firmware`, so this track needs a vendor_boot/vendor dump first.
- **USB**: the port is DR-mode; the stock kernel runs `qcom,usb` glue that is not part of this port.

## 6. Order that keeps the risk low

1. Policy archive links and the kernel boots with it (no root mode) — one CI run.
2. Root mode with the OS UUID and no RAM disk — the first boot that can reach userland from UFS.
3. Data seed, so `/data` survives a reboot.
4. Display: firmware framebuffer is already the fallback, so the port keeps booting while panel
   init is being worked on.
5. GPU, then Metal, then the rest.
