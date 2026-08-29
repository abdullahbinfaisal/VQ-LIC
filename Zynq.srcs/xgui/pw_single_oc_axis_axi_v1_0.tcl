# Definitional proc to organize widgets for parameters.
proc init_gui { IPINST } {
  ipgui::add_param $IPINST -name "Component_Name"
  #Adding Page
  set Page_0 [ipgui::add_page $IPINST -name "Page 0"]
  ipgui::add_param $IPINST -name "ACC_WIDTH" -parent ${Page_0}
  ipgui::add_param $IPINST -name "CIN_MAX" -parent ${Page_0}
  ipgui::add_param $IPINST -name "C_S_AXI_ADDR_WIDTH" -parent ${Page_0}
  ipgui::add_param $IPINST -name "C_S_AXI_DATA_WIDTH" -parent ${Page_0}
  ipgui::add_param $IPINST -name "DATA_WIDTH" -parent ${Page_0}
  ipgui::add_param $IPINST -name "IN_FIFO_DEPTH" -parent ${Page_0}
  ipgui::add_param $IPINST -name "OUT_FIFO_DEPTH" -parent ${Page_0}
  ipgui::add_param $IPINST -name "TILE_PIXELS_MAX" -parent ${Page_0}

  ipgui::add_param $IPINST -name "S_AXIS_DATA_WIDTH"
  ipgui::add_param $IPINST -name "N_LANES"
  ipgui::add_param $IPINST -name "M_AXIS_DATA_WIDTH"
  ipgui::add_param $IPINST -name "N_OC"

}

proc update_PARAM_VALUE.ACC_WIDTH { PARAM_VALUE.ACC_WIDTH } {
	# Procedure called to update ACC_WIDTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.ACC_WIDTH { PARAM_VALUE.ACC_WIDTH } {
	# Procedure called to validate ACC_WIDTH
	return true
}

proc update_PARAM_VALUE.CIN_MAX { PARAM_VALUE.CIN_MAX } {
	# Procedure called to update CIN_MAX when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.CIN_MAX { PARAM_VALUE.CIN_MAX } {
	# Procedure called to validate CIN_MAX
	return true
}

proc update_PARAM_VALUE.COUT_MAX { PARAM_VALUE.COUT_MAX } {
	# Procedure called to update COUT_MAX when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.COUT_MAX { PARAM_VALUE.COUT_MAX } {
	# Procedure called to validate COUT_MAX
	return true
}

proc update_PARAM_VALUE.C_S_AXI_ADDR_WIDTH { PARAM_VALUE.C_S_AXI_ADDR_WIDTH } {
	# Procedure called to update C_S_AXI_ADDR_WIDTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.C_S_AXI_ADDR_WIDTH { PARAM_VALUE.C_S_AXI_ADDR_WIDTH } {
	# Procedure called to validate C_S_AXI_ADDR_WIDTH
	return true
}

proc update_PARAM_VALUE.C_S_AXI_DATA_WIDTH { PARAM_VALUE.C_S_AXI_DATA_WIDTH } {
	# Procedure called to update C_S_AXI_DATA_WIDTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.C_S_AXI_DATA_WIDTH { PARAM_VALUE.C_S_AXI_DATA_WIDTH } {
	# Procedure called to validate C_S_AXI_DATA_WIDTH
	return true
}

proc update_PARAM_VALUE.DATA_WIDTH { PARAM_VALUE.DATA_WIDTH } {
	# Procedure called to update DATA_WIDTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.DATA_WIDTH { PARAM_VALUE.DATA_WIDTH } {
	# Procedure called to validate DATA_WIDTH
	return true
}

proc update_PARAM_VALUE.IN_FIFO_DEPTH { PARAM_VALUE.IN_FIFO_DEPTH } {
	# Procedure called to update IN_FIFO_DEPTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.IN_FIFO_DEPTH { PARAM_VALUE.IN_FIFO_DEPTH } {
	# Procedure called to validate IN_FIFO_DEPTH
	return true
}

proc update_PARAM_VALUE.M_AXIS_DATA_WIDTH { PARAM_VALUE.M_AXIS_DATA_WIDTH } {
	# Procedure called to update M_AXIS_DATA_WIDTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.M_AXIS_DATA_WIDTH { PARAM_VALUE.M_AXIS_DATA_WIDTH } {
	# Procedure called to validate M_AXIS_DATA_WIDTH
	return true
}

proc update_PARAM_VALUE.N_LANES { PARAM_VALUE.N_LANES } {
	# Procedure called to update N_LANES when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.N_LANES { PARAM_VALUE.N_LANES } {
	# Procedure called to validate N_LANES
	return true
}

proc update_PARAM_VALUE.N_OC { PARAM_VALUE.N_OC } {
	# Procedure called to update N_OC when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.N_OC { PARAM_VALUE.N_OC } {
	# Procedure called to validate N_OC
	return true
}

proc update_PARAM_VALUE.OUT_FIFO_DEPTH { PARAM_VALUE.OUT_FIFO_DEPTH } {
	# Procedure called to update OUT_FIFO_DEPTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.OUT_FIFO_DEPTH { PARAM_VALUE.OUT_FIFO_DEPTH } {
	# Procedure called to validate OUT_FIFO_DEPTH
	return true
}

proc update_PARAM_VALUE.S_AXIS_DATA_WIDTH { PARAM_VALUE.S_AXIS_DATA_WIDTH } {
	# Procedure called to update S_AXIS_DATA_WIDTH when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.S_AXIS_DATA_WIDTH { PARAM_VALUE.S_AXIS_DATA_WIDTH } {
	# Procedure called to validate S_AXIS_DATA_WIDTH
	return true
}

proc update_PARAM_VALUE.TILE_PIXELS_MAX { PARAM_VALUE.TILE_PIXELS_MAX } {
	# Procedure called to update TILE_PIXELS_MAX when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.TILE_PIXELS_MAX { PARAM_VALUE.TILE_PIXELS_MAX } {
	# Procedure called to validate TILE_PIXELS_MAX
	return true
}


proc update_MODELPARAM_VALUE.DATA_WIDTH { MODELPARAM_VALUE.DATA_WIDTH PARAM_VALUE.DATA_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.DATA_WIDTH}] ${MODELPARAM_VALUE.DATA_WIDTH}
}

proc update_MODELPARAM_VALUE.ACC_WIDTH { MODELPARAM_VALUE.ACC_WIDTH PARAM_VALUE.ACC_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.ACC_WIDTH}] ${MODELPARAM_VALUE.ACC_WIDTH}
}

proc update_MODELPARAM_VALUE.CIN_MAX { MODELPARAM_VALUE.CIN_MAX PARAM_VALUE.CIN_MAX } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.CIN_MAX}] ${MODELPARAM_VALUE.CIN_MAX}
}

proc update_MODELPARAM_VALUE.TILE_PIXELS_MAX { MODELPARAM_VALUE.TILE_PIXELS_MAX PARAM_VALUE.TILE_PIXELS_MAX } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.TILE_PIXELS_MAX}] ${MODELPARAM_VALUE.TILE_PIXELS_MAX}
}

proc update_MODELPARAM_VALUE.IN_FIFO_DEPTH { MODELPARAM_VALUE.IN_FIFO_DEPTH PARAM_VALUE.IN_FIFO_DEPTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.IN_FIFO_DEPTH}] ${MODELPARAM_VALUE.IN_FIFO_DEPTH}
}

proc update_MODELPARAM_VALUE.OUT_FIFO_DEPTH { MODELPARAM_VALUE.OUT_FIFO_DEPTH PARAM_VALUE.OUT_FIFO_DEPTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.OUT_FIFO_DEPTH}] ${MODELPARAM_VALUE.OUT_FIFO_DEPTH}
}

proc update_MODELPARAM_VALUE.C_S_AXI_DATA_WIDTH { MODELPARAM_VALUE.C_S_AXI_DATA_WIDTH PARAM_VALUE.C_S_AXI_DATA_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_S_AXI_DATA_WIDTH}] ${MODELPARAM_VALUE.C_S_AXI_DATA_WIDTH}
}

proc update_MODELPARAM_VALUE.C_S_AXI_ADDR_WIDTH { MODELPARAM_VALUE.C_S_AXI_ADDR_WIDTH PARAM_VALUE.C_S_AXI_ADDR_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_S_AXI_ADDR_WIDTH}] ${MODELPARAM_VALUE.C_S_AXI_ADDR_WIDTH}
}

proc update_MODELPARAM_VALUE.S_AXIS_DATA_WIDTH { MODELPARAM_VALUE.S_AXIS_DATA_WIDTH PARAM_VALUE.S_AXIS_DATA_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.S_AXIS_DATA_WIDTH}] ${MODELPARAM_VALUE.S_AXIS_DATA_WIDTH}
}

proc update_MODELPARAM_VALUE.N_LANES { MODELPARAM_VALUE.N_LANES PARAM_VALUE.N_LANES } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.N_LANES}] ${MODELPARAM_VALUE.N_LANES}
}

proc update_MODELPARAM_VALUE.M_AXIS_DATA_WIDTH { MODELPARAM_VALUE.M_AXIS_DATA_WIDTH PARAM_VALUE.M_AXIS_DATA_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.M_AXIS_DATA_WIDTH}] ${MODELPARAM_VALUE.M_AXIS_DATA_WIDTH}
}

proc update_MODELPARAM_VALUE.N_OC { MODELPARAM_VALUE.N_OC PARAM_VALUE.N_OC } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.N_OC}] ${MODELPARAM_VALUE.N_OC}
}

proc update_MODELPARAM_VALUE.COUT_MAX { MODELPARAM_VALUE.COUT_MAX PARAM_VALUE.COUT_MAX } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.COUT_MAX}] ${MODELPARAM_VALUE.COUT_MAX}
}

