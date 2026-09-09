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
