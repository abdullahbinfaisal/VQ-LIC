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

### There is no system Python -- use Vivado's

`python`, `python3` and `py` all fail on this machine (the Windows Store stub
intercepts them). Vivado ships one:

```
/c/SPROJ/Vivado/2020.2/tps/win64/python-3.8.3/python.exe
```

Also note: the Bash tool's heredoc collapses a DOUBLED backslash to a single one, so a script written
through `<<'EOF'` must contain **no doubled backslashes**. Use forward slashes
in paths, and build escape sequences as `chr(92) + 'n'` rather than writing
them literally. Single backslashes inside C string literals survive intact.

**Apostrophes anywhere in a Bash command can stop it before it runs.** The
command is pre-scanned for single-quote balance -- including the body of a
QUOTED heredoc and characters inside double quotes. An odd count (a C comment
saying "the guide's", a `grep -c "'"`, even `\x27` in a grep pattern) fails
with `unexpected EOF while looking for matching '`, and NOTHING in the command
executes, so a multi-file write can silently do nothing. Keep Bash commands
free of apostrophes, and write source files with the Write tool instead of a
heredoc.

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

The platform does not leave a `.bit` lying around, so "the app built" tells
you nothing about what is in the fabric. Extracted copies under `Final_2/hw/`
are put there BY HAND after a build; treat them as a convenience, never as
evidence, and md5 them against `Zynq.runs/impl_1/hw_wrapper.bit` before
believing any of them is what the PL holds.

After a platform repoint, all three steps are required:

1. regenerate/rebuild the PLATFORM component (not just the app),
2. rebuild the application,
3. **program the FPGA** with the new bitstream.

### The JSON repoint alone cannot change what gets PROGRAMMED

`Final_code_2/_ide/launch.json` has `"programDevice": true` and a **hardcoded
absolute path**:

```json
"bitstreamFile": "C:\Users\Fahad\Zynq\Zynq.runs\impl_1\hw_wrapper.bit",
```

It programs the FPGA from the **Vivado project's own impl_1 output**, NOT from
the platform XSA. So the platform JSON governs what the software is COMPILED
against, and `Zynq.runs/impl_1/hw_wrapper.bit` governs what the fabric actually
RUNS. They are independent, and repointing the JSON has historically appeared
to "fix" things only because implementation was normally run in this project,
which kept that path current as a side effect.

**Therefore: if a build is produced anywhere other than this project's own
`impl_1` — a scratch copy, a different machine — the bitstream must be copied
to `Zynq.runs/impl_1/hw_wrapper.bit`, or the launch config repointed. Otherwise
new firmware runs against old fabric and the only symptom is wrong results.**

Check both, they are different questions:

```bash
md5sum Zynq.runs/impl_1/hw_wrapper.bit    # what the board will run
grep -oE 'pwvq[^\"]*\.xsa' Final_2/vitis-comp.json   # what the app compiles against
```

Do not trust a filename written down here -- it goes stale the next time the
design is rebuilt. Derive it:

```bash
grep -oE 'pwvq[^\"]*\.xsa' Final_2/vitis-comp.json      # the XSA in force
md5sum Zynq.runs/impl_1/hw_wrapper.bit                  # what will be programmed
unzip -p Final_2/hw/<that>.xsa hw.hwh | grep -oE 'VQ_K" VALUE="[0-9]*"'
```

All three must agree, and the .bit inside the XSA must md5-match
`Zynq.runs/impl_1/hw_wrapper.bit`.

### Rebuilding: the IP cache and ip_repo_paths, both learned the hard way

A full build consumes the packaged IP COPIES under

```
Zynq.gen/sources_1/bd/hw/ipshared/<n>/sources_1/new/pw_single_oc_axis.sv
Zynq.gen/sources_1/bd/hw/ipshared/<n>/src/pw_pixel_major_core.sv
```

not `Zynq.srcs/` directly -- `Zynq.runs/synth_1/runme.log` names these paths.
The copies go STALE: before the 2026-09-10 build they were a pre-K64 snapshot
(1439 lines against 1748). Editing a source and building WITHOUT forcing a
regenerate silently reuses that snapshot and the change never reaches the
fabric. So a rebuild must do, in order:

```tcl
update_ip_catalog -rebuild
upgrade_ip [get_ips -filter {IS_LOCKED == 1}]   ;# stale IPs report as LOCKED
reset_target all $bd
generate_target all $bd
reset_run synth_1
```

**"Stale IP file detected" is the GOOD sign** -- the IP is stale precisely
because the edit landed. `upgrade_ip` is what makes the BD adopt it. Verify
afterwards that the ipshared copy matches the source (line count, and grep for
the thing you changed) BEFORE trusting the build.

**Do not narrow `ip_repo_paths`.** The project needs all three:

```
C:/Users/Fahad/Zynq/Zynq.srcs
C:/Users/Fahad/ip_repo/DW_conv_accel_1.0
C:/Users/Fahad/ip_repo
```

Setting only `Zynq.srcs` makes `dw_fused_axi` report "IP definition not found"
and locks the BD -- it lives in `C:/Users/Fahad/ip_repo`, OUTSIDE the project.
Restore the property explicitly rather than reverting `Zynq.xpr`, which carries
unrelated changes.

**Build in the real project, not a scratch copy**, so
`Zynq.runs/impl_1/hw_wrapper.bit` -- the path `launch.json` hardcodes -- is the
thing that changes. A scratch build leaves it stale and the board runs old
fabric against new firmware.

### An RTL-only change does not need a platform repoint

If a rebuild changes internal logic but not the register map, parameters or
port list, the application is unaffected: it compiles against the same
interface, and `launch.json` programs from `impl_1`. The platform JSON may go
on naming an older XSA and that is FINE -- but say so out loud, because it
otherwise looks like the stale-platform trap above. Check the parameters really
are unchanged:

```bash
unzip -p Final_2/hw/<xsa> hw.hwh | grep -oE '(VQ_K|VQ_NORM_D|COUT_MAX)" VALUE="[0-9]*"' | sort -u
```

### There is a THIRD copy of the XSA name, and Vitis owns it

```
Final_2/vitis-comp.json          <- edit this by hand
Final_2/export/Final_2/Final_2.xpfm   <- Vitis REGENERATES this
```

Editing the JSON does not update the export. Until the PLATFORM component is
rebuilt in Vitis, `Final_2.xpfm` still names the previous XSA, and that is what
the application actually compiles against. A repoint is not finished until

```bash
grep -oE 'pwvq[^\"<>]*\.xsa' Final_2/vitis-comp.json
grep -oE 'pwvq[^\"<>]*\.xsa' Final_2/export/Final_2/Final_2.xpfm
```

print the SAME name. If they differ, the platform has not been rebuilt yet.

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
range-coder geometry, the RTL benches and the configuration guard -- plus the
rANS ladder (guide vectors, round trip, stage 3 self-check, edge glue, the
divide-free encoder). All 16 host checks must pass.

## rANS entropy coder -- read before touching `Final_code_2/src/rans*.c`

Full description: `results/report.md`, "rANS entropy coder -- how it works".

- The spec is `RANS_GUIDE.md` + `CONTEXT_CODEC.md`. There is NO Python
  reference in this repo -- do not go looking for entropy.py or codec.py. The
  acceptance criterion is byte-identical output against the CS team's
  reference, and it has NOT been met: `rans_stage3.exe DIR` needs their
  `rom.bin`, `slots.bin`, `idx.bin`, `payload.bin`. Never call the coder
  validated against the reference.
- Mode 3 (context) and mode 0 (raw fallback) only. Do not change the fallback.
- Ids are coded 256-wide; `slot_of_id` is table ADDRESSING. The k -> id map and
  the tables in `rans_edge.c` are SYNTHETIC, fitted at boot: timing is real,
  payload bytes are not a rate.
- The board runs the divide-free encoder (`T->recip` set). The A9 has no
  hardware divide, so a `/` or `%` in a per-token loop is an `__aeabi_uidivmod`
  call -- check the disassembly. The divide encoder stays as the reference
  (`T->recip = NULL`, `re_set_fast_divide(0)`), and any encoder change must
  keep `rans_recip_test` passing: it is what proves both write the same bytes.
- Planes and work buffers in `rans_edge.c` are static: ONE live `re_job_t`.
- The power loop (`edge_pipe_frame`) must run the same schedule as
  `edge_one_pipelined`, entropy under the search included. PWRSUM prints
  AGREE/DISAGREE for exactly this; DISAGREE means the energy figure pairs power
  with the wrong schedule.

## VQ -- current state (snapshot 2026-09-14; re-derive before quoting)

- Deployed: the SHARED PW engine, M=4 K=64 Dm=16, bitstream pwvq_k64c. Codebook
  reload 0.96 ms per frame (it shares the analysis weight BRAM), search 4.90 ms
  accelerator / 5.28 ms driver bracket, II 20.84 ms, 42.84 mJ per frame --
  board, 2026-09-11.
- The dedicated engine (`vq_pq_axi`) is NOT in the block design (removed in
  62cbfbc). Its IP is outside the repo, `C:/Users/Fahad/ip_repo/vq_pq_axi_1.0`,
  and as shipped only elaborates at K=256 (`vq_pq_top.sv:329`).
- Dedicated vs shared, OOC, 2020.2, xc7z020: dedicated K=64 is 8,312 LUT /
  8,561 FF / 9 BRAM36 / 0 DSP; the shared VQ branch is +301 LUT / +497 FF /
  0 BRAM / 0 DSP. Evidence and latency projections:
  `results/vq_dedicated_vs_shared/` and `results/report.md`, "VQ -- current
  state, and dedicated vs shared".

### Vivado OOC batch runs: the first synth_design can die tool-side

`synth_design -mode out_of_context` in a `vivado -mode batch` process has
repeatedly failed on its FIRST call with `couldn't read file
".../realtime/<top>.tcl": No error` (or `retarget_vhdl.tcl`,
`unimacro_verilog.tcl`), before elaboration. The second call in the same
process succeeded every time. It is not an RTL error: wrap each call in
`catch`, put a duplicate configuration first, and read the log before blaming
the design.

## Evidence discipline

- `results/board_measured.py` holds MEASURED board data. Do not edit its values
  to match a model.
- `model/service_model.py` is frozen. If measurement disagrees with it, the
  disagreement is the finding — report it, do not fit a correction term.
- Label every number as measured, simulated or modelled, and say on what.
