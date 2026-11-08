# GPU acceleration on davinci (Adreno 618): port plan

Goal: the window server (backboardd / CoreAnimation) drawing through Metal on the
Mi 9T's Adreno 618, using hakios's stack
([hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel): `drivers/gpu/`,
`hakios-metal/`, `userland/src/HakiosMetalCA.m`). hakios runs that stack on the
POCO X5 Pro's Adreno 642L (SC7280). Nothing here has run on an Adreno 618 yet.

Status 2026-11-09: **phase 0 only** (read-only probe, patch 0085). Today rendering
is the CPU path inherited from sweet (patch 0046 IOSurface/AppleCLCD + panel copy).

## Why it is a port, not a copy

hakios and this tree are both xnu-8796, so code moves as patches, but:

| | hakios (redwood, SC7280) | davinci (sdmmagpie / SDM730) | source |
|---|---|---|---|
| GPU | Adreno 642L, a6xx gen4 | **Adreno 618, a6xx gen1** | sdmmagpie-gpu.dtsi |
| chip id | 0x06030500 | **0x06010800** | `qcom,chipid` |
| 3D core regs | 0x3d00000 | **0x5000000** (0x40000) | kgsl-3d0 reg |
| cx_dbgc / cx_misc | 0x3d0f000.. / 0x3d9e000 | **0x5061000 / 0x509e000** | kgsl-3d0 reg |
| GMU | 650-family DTCM 0x10004000 | **0x506a000** (0x31000), a630-family GMU, `qcom,gpu-quirk-hfi-use-reg` (legacy register HFI boot) | gmu node |
| GMU PDC | | cfg 0xb290000, seq 0xb490000 | gmu node |
| GPU SMMU | 0x3da0000 | **0x5040000** (0x10000), user CB 0, secure CB 2 | kgsl-iommu |
| GPUCC | | **0x5090000**: GX GDSCR +0x100c, CX GDSCR +0x106c, cx_gmu CBCR +0x1098, cxo +0x109c, gmu RCGR +0x1120 | gpucc-sdmmagpie.c, sdmmagpie-gdsc.dtsi |
| GCC branches | | gpu_cfg_ahb 0x71004, memnoc_gfx 0x7100c, snoc_dvm 0x71018, **ddrss_gpu_axi 0x4452c** | gcc-sdmmagpie.c |
| cmd-db | reserved memory 0x80860000 | **pointer read from 0xc3f000c** | sdmmagpie.dtsi |
| zap shader | a660_zap, pas-id 13 | **a615_zap**, pas-id 13 | pil_gpu node |
| GMU fw / SQE | a660_gmu.bin / a660_sqe.fw | **a630_gmu.bin / a630_sqe.fw** (from /vendor/firmware) | Mesa/Linux a6xx tables |
| freqs | 550 MHz used | 180..825 MHz (speed-bin dependent), initial pwrlevel 7 | gpu pwrlevels |
| UBWC | | mode 2, highest bank bit 14 | kgsl-3d0 |

Mesa (ir3 in hakios-metal, Turnip) already knows a618 (`fd_dev_info` for
0x06010800), so the user-space change is mostly the chip id and GMEM size, not new
compiler work. The kernel side (power-up, GMU, HFI v1, zap, SMMU) is the real work.

## Phases (each verified on the phone before the next)

0. **Read-only probe** - patch 0085, boot-arg `davinci_gpuprobe=1`: GCC GPU clocks,
   GPUCC GDSCs/clocks (only when its AHB clock runs), cmd-db pointer. Tells us what
   ABL leaves on. *Built, not yet run.*
1. **Port hakios.adreno.c steps 1-6** (clocks, RPMh names gfx.lvl/cx.lvl/mx.lvl from
   cmd-db, CX GDSC on, frequency counter) with the table above; boot-arg gated, one
   step per boot (`adreno=N`), reversed at the end, like hakios.
2. **GMU**: a630-family GMU firmware load + legacy register HFI (Linux
   `a6xx_gmu.c` paths for `adreno_is_a618`), RPMh votes through the GMU.
3. **Zap + SQE**: TZ PAS id 13 with a615_zap.mdt/.bNN from the stock ROM's
   /vendor/firmware, SQE load, ring buffer, first `CP_NOP` submission.
4. **HakiosGPU IOKit service** (msm DRM requests + 0x7d/0x7e) on top of this tree's
   IOSurface implementation (0046/0068 differ from hakios's: needs a surface-ID ->
   physical-pages adapter).
5. **User space**: hakios-metal built for 0x06010800, the Metal plug-in and
   `HakiosMetalCA` injected into backboardd/SpringBoard of the real 16.7 image
   (hakios uses its own rc/inject; here it must go through launchd plists, like
   `tools/sweet-root-tweaks.py` does), plus `tools/ca-metallib.py`'s library.
6. Present frames: a command-mode panel needs MDP autorefresh (shim, already) or a
   proper kickoff per frame.

## What is needed from the phone/ROM

- `/vendor/firmware/a630_gmu.bin`, `a630_sqe.fw`, `a615_zap.mdt` + `a615_zap.b0*`
  (official ROM `vendor.img`, V12.1.4.0.RFJMIXM). Keep them out of git.
- Speed bin fuse (qfprom 0x780000) decides the frequency table.
