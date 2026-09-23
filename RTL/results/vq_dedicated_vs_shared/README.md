# Dedicated vs shared VQ -- evidence, 2026-09-14

Analysis and tables: `results/report.md`, "VQ -- current state, and dedicated vs
shared". Every synthesis here is out-of-context, Vivado 2020.2
(`C:/SPROJ/Vivado/2020.2`), `xc7z020clg484-1`, 10 ns clock.

| file | what |
|---|---|
| `util_dedicated_k64.rpt` | `vq_pq_axi`, M=4 K=64 DSUB=16 QUERY_LANES=2 (scratch copy, see below) |
| `util_dedicated_k256.rpt` | `vq_pq_axi` at its shipped K=256 |
| `util_shared_pw_vq.rpt` | `pw_single_oc_axis_axi`, deployed generics, `USE_PW_VQ=1` |
| `util_shared_pw_novq.rpt` | the same, `USE_PW_VQ=0` |
| `wns_results.txt` | the `### RESULT` lines; the FAILED lines are tool-side helper failures before elaboration, not design errors |
| `synth_pwvq.tcl` | produced both PW reports |
| `synth_ded.tcl` | produced the K=256 report |
| `synth_dedicated_shortpath.tcl` | produced the K=64 report (short working directory, single thread) |
| `copy_vq_k64.py` | builds the scratch copy of the dedicated IP with `vq_pq_top.sv:329` generalised to K < 256 |
| `dedicated_projection.py` | latency and energy projection, shared (measured) vs dedicated D1/D2 |
| `frame_overlap_energy_breakdown.py` | serial-frame, overlap and energy breakdown of the 2026-09-11 board run |

The scripts were run from the session scratchpad, so their hard-coded paths
point there. To reproduce, change `outdir` / `src` to local directories; the
dedicated IP sources are in `C:/Users/Fahad/ip_repo/vq_pq_axi_1.0/src`, outside
this repository.
