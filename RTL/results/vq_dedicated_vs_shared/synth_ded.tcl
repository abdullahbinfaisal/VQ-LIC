# Dedicated VQ engine (vq_pq_axi) out of context, Vivado 2020.2, xc7z020clg484-1.
# Reads the scratch copy made by copy_vq_k64.py: identical to ip_repo except the
# index slice at vq_pq_top.sv:329, which is identical at K=256.
# K=64 is the deployed geometry; K=256 reproduces the recorded 8,074-LUT run.
# usage: vivado -mode batch -source synth_ded.tcl -tclargs <outdir>
set outdir [lindex $argv 0]
set src    C:/Users/Fahad/AppData/Local/Temp/claude/C--Users-Fahad-Zynq-Zynq-srcs-sources-1-new/2ba9a201-644e-401b-85ad-6761a18893ac/scratchpad/vq_src_k64
set part   xc7z020clg484-1
foreach k {64 256} {
  puts "### SYNTH ded_k$k"
  create_project -in_memory -part $part
  read_verilog -sv [glob $src/*.sv]
  if {[catch {synth_design -top vq_pq_axi -part $part -mode out_of_context \
      -generic DSUB=16 -generic M=4 -generic K=$k -generic LANES=8 -generic NCH=64 \
      -generic QUERY_LANES=2 -generic GROUP_BUFS=8 -generic SQ_STYLE=1} err]} {
    puts "### RESULT ded_k$k FAILED: $err"
    close_project
    continue
  }
  create_clock -period 10.000 -name clk [get_ports aclk]
  report_utilization -file $outdir/util_ded_k$k.rpt
  report_timing_summary -file $outdir/timing_ded_k$k.rpt
  set wns [get_property SLACK [get_timing_paths -delay_type max]]
  set dsp [llength [get_cells -hier -quiet -filter {REF_NAME =~ DSP48*}]]
  puts "### RESULT ded_k$k WNS=$wns DSPcells=$dsp"
  close_project
}
puts "### DONE"
