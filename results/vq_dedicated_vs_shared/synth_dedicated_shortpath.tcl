set_param general.maxThreads 1
foreach k {256 64} {
  create_project -in_memory -part xc7z020clg484-1
  read_verilog -sv [glob C:/Users/Fahad/AppData/Local/Temp/vqk64/src/*.sv]
  if {[catch {synth_design -top vq_pq_axi -part xc7z020clg484-1 -mode out_of_context -generic DSUB=16 -generic M=4 -generic K=$k -generic LANES=8 -generic NCH=64 -generic QUERY_LANES=2 -generic GROUP_BUFS=8 -generic SQ_STYLE=1} err]} {
    puts "### RESULT ded_k$k FAILED: $err"; close_project; continue
  }
  create_clock -period 10.000 -name clk [get_ports aclk]
  report_utilization -file C:/Users/Fahad/AppData/Local/Temp/vqk64/util_k$k.rpt
  report_timing_summary -file C:/Users/Fahad/AppData/Local/Temp/vqk64/timing_k$k.rpt
  set wns [get_property SLACK [get_timing_paths -delay_type max]]
  set dsp [llength [get_cells -hier -quiet -filter {REF_NAME =~ DSP48*}]]
  puts "### RESULT ded_k$k WNS=$wns DSPcells=$dsp"
  close_project
}
