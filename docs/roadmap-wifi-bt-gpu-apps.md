# Roadmap: GPU, Wi-Fi, Bluetooth, apps (and porting to other phones)

Written 2026-11-09 for whoever (human or agent) continues this port or starts one
for another Snapdragon phone. Read `NOTES.md` first; this file explains **what is
missing, what it would take, and roughly how big each piece is**. Sizes are
estimates from comparable code in this tree and in
[hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) (cloned to
`../refs/hakios-xnu-kernel`); they are not measurements of finished work.

## Where davinci stands (observed on the phone)

| Area | State |
|---|---|
| Boot | ABL -> shim (boot partition, v0 Image.gz-dtb, LZ4 payload) -> XNU -> HFSX root in userdata |
| CPU | 8 cores (`cpus=8`), CPU rendering only |
| UI | lock screen / home screen, SpringBoard, Settings; iPhone X display scaled onto 1080x2340 (patch 0086) |
| Touch, power key | work (GT9886 patch 0082 + 0084, SPMI keys) |
| GPU | **off**: ABL leaves CX/GX GDSCs collapsed and every GPUCC clock off (patch 0085 probe) |
| Wi-Fi, Bluetooth, cellular, audio, camera, sensors | none; their daemons relaunch in a loop |
| App Store / Apple ID | not supported (see below) |

The whole existing kernel side is 86 patches, ~28 800 added lines, of which
~19 700 are Apple's own IOHIDFamily sources (0058). Real driver code written for
the port is ~9 000 lines. Use that as the yardstick below.

---

## 1. Lessons for porting to another phone

These cost most of a day on davinci; check them first on any new device.

1. **`fastboot boot` may lie.** davinci's ABL answers `Booting OKAY` and stays in
   fastboot (even for pmOS's U-Boot). Test with `fastboot oem device-info` 5 s
   later: if ABL answers, nothing ran. Then use `fastboot flash boot` + reboot.
2. **vbmeta**: stock vbmeta (verification on) gives `Failed to load/authenticate
   boot image: Load Error` for every unsigned image. Flash a vbmeta with flags=3
   (the user does this; it is their security setting).
3. **dtbo must be blank** (or match): ABL merges the dtbo partition's overlays
   into the DTB you ship; Xiaomi's overlays do not apply to a mainline DTB, and
   ABL silently refuses to boot. pmOS leaves dtbo erased; do the same.
4. **The DTB must carry the board's `qcom,msm-id` and `qcom,board-id`** (davinci:
   `<0x16d 0>`, `<0x28 0>`). Take them from the stock DTB or a working
   pmOS/LineageOS boot image, not from recovery images (OrangeFox's had board-id 0).
5. **Boot image format**: match what the device's ABL already boots (davinci:
   header v0, kernel = gzip(Image) + appended DTB). `shim/build.sh BOOTFMT=`.
6. **Ramdisk size limit** (~16 MiB on davinci): XNU payloads are LZ4-wrapped
   (`tools/lz4payload.py`, decoder in `shim/loader.c`).
7. **Decompress/scratch addresses must avoid carve-outs.** List
   `reserved-memory` from the DTB (`dtc`), pick plain RAM (davinci: 0xc0000000).
   Writing into modem/TZ memory does not fault: the bus just hangs.
8. **Command-mode panels** need MDP autorefresh for CPU framebuffer writes to
   show; davinci's ABL leaves it working. `shim.c cmd_panel_autorefresh()` only
   touches MDP when its clocks and GDSC are on.
9. **Touch controllers have extra enables and sleep modes**: read the vendor
   DTS (`goodix,avdd-gpio`) and the vendor driver's power-on order, not just the
   I2C address.
10. **First boot**: lsd rebuilds the LaunchServices database and SpringBoard
    blocks on it (`__LAUNCH_SERVICES_IS_RESEEDING_ITS_DATABASE_AND_MAY_BLOCK__`);
    give it >10 min before calling it a hang. `sweetsample=` + `ipsw dyld a2s`
    (address - slide) symbolicate any stuck thread in seconds.

Per-device work for a new SM7150/SM8150-class phone, if the above go right:
a few hundred lines (DTB choice, touch driver variant, panel geometry, key GPIOs).

---

## 2. GPU acceleration (Adreno 618 here)

**Goal**: CoreAnimation renders through Metal on the Adreno instead of the CPU.
Only then is a steady 60 fps realistic.

**What exists to reuse**: hakios runs exactly this on the Adreno 642L (SC7280):
- kernel: `drivers/gpu/hakios.adreno.c` + `hakios.gpu.cpp`, **~5 200 lines**:
  clocks, GDSCs, RPMh rail votes via cmd-db, GMU firmware + HFI, zap shader via
  TrustZone PAS, SQE microcode, ring buffer, SMMU page tables, an IOKit user
  client speaking Linux msm-DRM ioctls (+ IOSurface import);
- user space: `hakios-metal/` (**~19 000 lines** C++) = Metal command model on
  PM4 packets with Mesa's ir3 compiler, the Metal plug-in `HakiosMetal.m`, and
  `HakiosMetalCA.m` for the window server; QuartzCore needs a `default.metallib`
  built by `tools/ca-metallib.py`.
They report 90-118 fps at 750x1624 on SC7280.

**What changes for davinci** (full table: `docs/gpu-a618-port.md`):
chip id 0x06010800 (a6xx **gen1**, hakios targets gen4), GPU at 0x5000000, GMU at
0x506a000 with the a630-family GMU and **legacy register HFI**
(`qcom,gpu-quirk-hfi-use-reg`), SMMU at 0x5040000, GPUCC at 0x5090000, firmware
`a630_gmu.bin`, `a630_sqe.fw`, `a615_zap.mdt/.b0x` (pas-id 13) from the stock
`vendor.img`, cmd-db pointer at 0xc3f000c.

**Steps and size**

| # | Step | Size (est.) | Verify on phone by |
|---|---|---|---|
| 0 | read-only probe | done (0085) | log lines `DAVINCI GPU:` |
| 1 | clocks, CX GDSC, RPMh gfx/cx/mx levels | port ~800 lines of hakios.adreno.c steps 1-6 | frequency counter reads 19.2 MHz |
| 2 | GMU: a630 firmware load, legacy HFI, power-up of GX | ~1 000 lines new (Linux `a6xx_gmu.c` a618 paths) | GMU `0xbabeface`, HFI init ack |
| 3 | zap shader (TZ PAS 13), SQE, ring, first CP_NOP | ~600 lines | ring read pointer advances |
| 4 | SMMU + IOKit user client (msm-DRM ioctls, IOSurface import over **this tree's** IOSurface 0046/0068) | ~2 000 lines | `hakios-metal-probe` passes |
| 5 | user space: build hakios-metal / Mesa ir3 for 0x06010800, plug-in into backboardd via launchd plist / root tweak | build work + ~300 lines | window server draws via Metal |
| 6 | present: Metal-rendered surfaces to the panel (scaler of 0086 or MDP) | ~200 lines | fps counter |

Total: **~5 000 kernel lines (mostly ported) + build/integration of ~19 000 user
lines**; many phone boots (each step is `adreno=N`-gated, as in hakios). Without the
GPU, the best the CPU path can do is the 8-core / 60 Hz copy already in place.

---

## 3. Wi-Fi (WCN3990)

**Hardware** (`sdmmagpie.dtsi`): WCN3990 "SNOC" Wi-Fi at 0x18800000 (12 copy-engine
IRQs 414-425), MSA memory 0x97700000 (1 MiB), supplies L9A (cx-mx 0.64 V),
L1C (1.8 V xo), L2C (1.3 V rfa), L10C (3.3 V ch0). **The WLAN firmware runs on
the modem DSP (MPSS)**, so the modem must be booted first.

**What it takes**
1. **Remoteproc for MPSS**: load `modem.mdt` + segments (from the `modem`
   partition / `firmware_mnt`) into the mpss carve-out (0x8b000000), authenticate
   through TrustZone PAS (pas-id 4), release, handle SMP2P + the QMI service
   directory. hakios has this for redwood's modem ("the modem boots, sees the
   network"): reuse.
2. **QMI WLFW** handshake (MSA info, BDF/board data `bdwlan.bin` from vendor,
   mode on).
3. **ath10k_snoc + mac80211 + cfg80211** compiled into XNU through a
   Linux-compat layer: hakios did exactly this for **ath11k** on WCN6750
   (`wifi/lx`: `lx_base.c`, `lx_bus.c`, `lx_80211.c`, `lx_wpa.c`, ~160 KB glue, plus
   the unmodified Linux sources). ath10k needs the same layer with SNOC/CE glue
   instead of AHB/PCI.
4. **WPA** in kernel (hakios `lx_wpa.c`) or a ported wpa_supplicant.
5. **iOS integration**, two levels:
   - *Network works* (what hakios has): an `en0` ifnet, credentials from a
     boot-arg/config file, configd sees an Ethernet-like interface. Needs the
     IONetworkInterface stand-in sweet started for USB (`NOTES.md`, 0075/0080).
   - *Settings > Wi-Fi works*: wifid/WiFiManager drive Apple's `IO80211Family`
     interface (Apple80211 ioctls: scan, associate, RSSI, events). That means an
     IO80211 controller emulation mapping onto cfg80211. Large, undocumented ABI.

**Size**: ~1 500 lines remoteproc/QMI (reusable from hakios) + the lx layer
ported to ath10k (~3 000-5 000 lines glue) + Linux ath10k/mac80211 sources
(~60 000 lines, compiled, not written). Settings integration on top is open-ended.
**Order**: after the modem boots; independent of the GPU.

---

## 4. Bluetooth (same WCN3990)

**Hardware**: HCI over UART on **QUP SE3** (`qupv3_se3_4uart` @0x88c000,
enabled in `xiaomi-sdmmagpie-common.dtsi`), supplies L10A (io 1.8 V), L2C (core
1.3 V), L10C (pa 3.3 V), L1C (xtal 1.8 V). Firmware `crbtfw21.tlv` + `crnv21.bin`
(vendor).

**What it takes**
1. RPMh votes for the four rails (extend `sweet_rpmh.c`, which already votes L6C).
2. GENI UART driver for SE3 (the I2C one in `sweet_touch.c` shows the GENI FIFO
   handling; UART is a different protocol/firmware on the same engine).
3. QCA bring-up: Linux `btqca.c` sequence (version query, TLV patch + NVM
   download, baud change) - ~800 lines.
4. **iOS side, the hard part**: `bluetoothd` expects Apple's transport and a
   Broadcom/Apple controller (vendor HCI commands, Apple's firmware model).
   A kernel service that presents the UART the way bluetoothd opens it, and
   handling or stubbing Apple-specific vendor commands, is unknown territory.
   Nobody has done it (hakios: "step one only").

**Size**: ~2 000 lines kernel for a working HCI link; iOS acceptance unknown,
possibly needs patching bluetoothd. **Lowest priority of the four.**

---

## 4b. Cellular / RIL (calls, SMS, mobile data)

**Status**: none. CommCenter is removed from launchd.plist by
`tools/sweet-root-tweaks.py` (on sweet it blocked Setup and SpringBoard waiting for a
baseband); Phone/Messages block in telephony XPC for the same reason.

**What it takes**
1. **Boot the modem** (MPSS remoteproc, shared with Wi-Fi, section 3): `modem.mdt`
   into the mpss carve-out 0x8b000000, TZ PAS id 4, SMP2P, QMI service directory,
   plus the `modemst1/2`, `fsg`, `fsc` EFS partitions served to the modem (rmtfs;
   davinci DT: rmtfs at 0xf2e01000). hakios on redwood: modem boots, sees the network,
   SIM bound, **no registration yet**.
2. **Data only** (easier): QMI WDS to start a session, IPA hardware data path, an
   ifnet (`pdp_ip0`-like) that configd accepts. hakios has IPA + `en1` in place.
   ~2 000-4 000 lines beyond the modem boot.
3. **Calls/SMS through iOS (the wall)**: CommCenter speaks Apple's baseband
   protocols (ARI for Intel, Apple's QMI variant over its own IPC transport) and needs
   activation/SEP and carrier bundles. A Xiaomi modem speaks stock QMI with Xiaomi
   firmware. It would need either a translation layer that impersonates Apple's
   baseband to CommCenter, or a replacement for CommCenter + the telephony daemons.
   Nobody has done either; size unknown, research project.

**Back up EFS first** (`modemst1/2`, `fsg`, `fsc`, `persist`: already in
`backup/`); a modem bring-up that corrupts EFS can lose the IMEI/calibration.
**Order**: after Wi-Fi (same modem boot), data before voice.

---

## 5. App Store and apps

- **App Store, Apple ID, iCloud, FaceTime/iMessage: not achievable** and not a
  goal. They need genuine activation, the Secure Enclave (SEP, absent here; we
  stub AppleKeyStore) and FairPlay keys bound to real Apple hardware.
  `fairplayd.H2` relaunching in the log is that. Do not try to bypass Apple's
  services; it is out of scope for this project.
- **App Store IPAs** are FairPlay-encrypted: they cannot run without fairplayd,
  even when copied over.
- **What can work**:
  - the stock apps on the system volume (fix them one by one from crash logs: sweet
    found Settings' PodcastsUI force-unwrap of `volumeTotalCapacity` on HFS, and
    Phone/Photos/Messages blocking on telephony XPC);
  - **your own or open-source apps** built for arm64 iOS 16, ad-hoc signed:
    AMFI is permissive, so copy the `.app` to `/Applications` (in the HFS image or
    via a future SSH/USB path), then let lsd reseed (reboot) so SpringBoard shows it.
    Containers come from containermanagerd; test with a trivial app first.
- **Needed plumbing**: a way to get files onto the phone while iOS runs (USB
  networking exists in the kernel, 0075/0076; configd integration is the missing
  piece), and a script that adds apps to the root image offline
  (`tools/sweet-root-tweaks.py` style).

**Size**: per-app fixes are small (tens of lines each, root tweaks or tiny kernel
stand-ins); an "install apps" path is ~300 lines of tooling.

---

## Suggested order

1. Root/app plumbing (USB networking -> configd, offline app install script): small, unblocks testing.
2. GPU phases 1-6 (biggest UX win, biggest job).
3. Modem remoteproc -> Wi-Fi (network level first, Settings later).
4. Bluetooth.

Record every boot in `NOTES.md` (observations vs guesses), commit patches as
`xnu/patches/NNNN-davinci-*.patch`, build kernels on GitHub Actions (`macos-15`;
re-run if the job is "not acquired" for runner capacity).
