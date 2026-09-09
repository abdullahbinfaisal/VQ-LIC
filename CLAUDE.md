# Zynq / PixelPacker — project notes

## Toolchain paths — CHECK HERE FIRST, DO NOT GUESS

**Vivado 2020.2 is the ONLY tool that may produce numbers for the paper.**

```
C:\SPROJ\Vivado\2020.2\bin\vivado
```

MSYS/bash form: `/c/SPROJ/Vivado/2020.2/bin/vivado`
(also `xvlog`, `xelab`, `xsim` in the same `bin/`)

**`C:\Xilinx\Vivado\2020.2` IS A DECOY.** It holds only `data/`, `tps/` and
`win64/` — no `bin/`, no executables. Finding no binaries there does NOT mean
2020.2 is unavailable. It means you looked in the wrong root. The real install
is under `C:\SPROJ`, and it is the one the Start Menu shortcut launches:

```
%APPDATA%\Microsoft\Windows\Start Menu\Programs\Xilinx Design Tools\Vivado 2020.2\
```

If a Xilinx tool ever appears to be missing, resolve the Start Menu `.lnk`
target before concluding anything:

```powershell
$sh = New-Object -ComObject WScript.Shell
$sh.CreateShortcut("$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Xilinx Design Tools\Vivado 2020.2\Vivado 2020.2.lnk").TargetPath
```

### Vivado 2025.1 — simulation only, never for reported numbers

```
C:\Xilinx\2025.1\Vivado\bin\    (xvlog / xelab / xsim / vivado)
```

It has **no Zynq-7000 devices installed** (artix7, kintex7, spartan7, virtex7
only), so it cannot target `xc7z020clg484-1` at all. Functional simulation is
fine there. Synthesis, implementation, utilisation, timing and power must come
from 2020.2 on the real part.

The user has corrected this twice. Do not substitute 2025.1 numbers, do not
substitute a different 7-series part, and do not report a resource or timing
figure without saying which tool and which part produced it.

## Target device

`xc7z020clg484-1` — 220 DSP48E1 (the binding resource; the shipped design sits
at 220/220), 53,200 LUT, 106,400 FF, 140 BRAM36. PL at 100 MHz.

## The PW core source exists TWICE — keep both in sync

```
Zynq.srcs/src/pw_pixel_major_core.sv            <- the IP PACKAGES THIS ONE
Zynq.srcs/sources_1/new/pw_pixel_major_core.sv  <- the one you will open
```

They are byte-identical by convention, both tracked in git, and
`Zynq.srcs/component.xml` lists **`src/pw_pixel_major_core.sv`** while every
other file of the IP (`ppu.sv`, `pw_single_oc_axis.sv`,
`pw_single_oc_axis_axi.sv`) comes from `sources_1/new/`.

**Editing only `sources_1/new/` produces a tree that passes every simulation
and out-of-context synthesis run and then fails the real build**, because
xsim/OOC read `sources_1/new/` directly and the packaged IP reads `src/`. It
surfaces as e.g. `[Synth 8-7136] parameter 'VQ_K' ... is a localparam` — the
wrapper is new, the core is stale.

After any edit to `pw_pixel_major_core.sv`:

```bash
cp Zynq.srcs/sources_1/new/pw_pixel_major_core.sv Zynq.srcs/src/pw_pixel_major_core.sv
diff Zynq.srcs/sources_1/new/pw_pixel_major_core.sv Zynq.srcs/src/pw_pixel_major_core.sv
```

Only a FULL block-design build catches the divergence. OOC synthesis of the IP
cannot.

## Changing the bitstream: the PL keeps the LAST thing programmed

Repointing `Final_2/vitis-comp.json` at a new XSA and rebuilding the Vitis
*application* does NOT reprogram the FPGA. A Zynq PL holds its configuration
until something explicitly overwrites it, so the board goes on running the
previous bitstream and the new firmware talks to old hardware.

There is no extracted `.bit` under `Final_2/` newer than 2026-07-30 -- the
platform does not leave one lying around -- so "the app built" tells you
nothing about what is in the fabric.

After a platform repoint, all three steps are required:

1. regenerate/rebuild the PLATFORM component (not just the app),
2. rebuild the application,
3. **program the FPGA** with the new bitstream.

The current bitstream is extracted for convenience at
`Final_2/hw/pwvq_k64.bit` (md5 e9893daf486df604e8b10a47ea84c6c6, identical to
`Zynq.runs/impl_1/hw_wrapper.bit` of the build that produced it).

### Ask the hardware which bitstream it is, do not infer

`vq_pw_pl_probe_guard()` programs a geometry the post-2026-09-09 engine must
refuse and reads `cfg_err` back: 1 = new build, 0 = old build. The harness
calls it at bring-up and prints the verdict.

This exists because inference does not work here. An old bitstream running the
new firmware and a new bitstream with a real RTL bug look **identical** from
software: same codebook reload time (the driver writes the same registers
either way) and the same search time (the convolution schedule follows
`cout_run` and `N_OC`, not `VQ_K`, so 8 batches cost 272 cyc/group on both).
`cfg_err` reads 0 on an old build because the bit does not exist, and 0 on a
new build that accepted the geometry.

## Verification entry points

```bash
cd Final_code_2/test && XILINX_VIVADO=/c/SPROJ/Vivado/2020.2 ./run_vq_tests.sh --rtl
```

Runs the VQ suite at both quantiser profiles (`VQPW_PROFILE` 1 = deployed
M=4/K=64/Dm=16, 0 = legacy M=8/K=16/Dm=8): golden model, driver programming,
range-coder geometry, the RTL benches and the configuration guard.

## Evidence discipline

- `results/board_measured.py` holds MEASURED board data. Do not edit its values
  to match a model.
- `model/service_model.py` is frozen. If measurement disagrees with it, the
  disagreement is the finding — report it, do not fit a correction term.
- Label every number as measured, simulated or modelled, and say on what.
