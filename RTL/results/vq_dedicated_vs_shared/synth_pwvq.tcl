# Shared PW engine with and without its VQ branch, same generics as synth.tcl
# (the deploy column of report.md section 9). Vivado 2020.2, xc7z020clg484-1.
# usage: vivado -mode batch -source synth_pwvq.tcl -tclargs <outdir>
set outdir [lindex $argv 0]
set src    C:/Users/Fahad/Zynq/Zynq.srcs/sources_1/new
set part   xc7z020clg484-1
foreach use {0 1} {
  puts "### SYNTH pw_vq$use"
  create_project -in_memory -part $part
  read_verilog -sv [list $src/pw_pixel_major_core.sv $src/ppu.sv \
                         $src/pw_single_oc_axis.sv $src/pw_single_oc_axis_axi.sv]
  synth_design -top pw_single_oc_axis_axi -part $part -mode out_of_context \
      -generic DATA_WIDTH=8 -generic ACC_WIDTH=24 \
      -generic CIN_MAX=240 -generic COUT_MAX=256 \
      -generic TILE_PIXELS_MAX=32768 \
      -generic IN_FIFO_DEPTH=2048 -generic OUT_FIFO_DEPTH=8192 \
      -generic S_AXIS_DATA_WIDTH=64 -generic M_AXIS_DATA_WIDTH=64 \
      -generic N_LANES=8 -generic N_OC=32 -generic USE_PW_VQ=$use \
      -generic VQ_K=64 -generic VQ_NORM_D=256 -generic VQ_SCORE_W=21
  create_clock -period 10.000 -name clk [get_ports s_axi_aclk]
  report_utilization -file $outdir/util_pw_vq$use.rpt
  report_timing_summary -file $outdir/timing_pw_vq$use.rpt
  set wns [get_property SLACK [get_timing_paths -delay_type max]]
  puts "### RESULT pw_vq$use WNS=$wns"
  close_project
}
puts "### DONE"
