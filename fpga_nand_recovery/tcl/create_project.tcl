################################################################################
## create_project.tcl
## Vivado Tcl script to create the Arty Z7 NAND dumper project
##
## Usage (from Vivado Tcl console or command line):
##   cd <path_to>/fpga_nand_recovery
##   source tcl/create_project.tcl
##
## Or from command line:
##   vivado -mode batch -source tcl/create_project.tcl
##
## Prerequisites:
##   - Vivado 2023.2 or later (tested with Zynq-7000 support)
##   - Digilent board files installed (for Arty Z7-20 preset)
##     Install via: Vivado -> Tools -> Settings -> Board Repository
##     Add: https://github.com/Digilent/vivado-boards/archive/master.zip
##
## After running this script:
##   1. Open the generated project: vivado nand_dumper/nand_dumper.xpr
##   2. Click "Generate Bitstream" (synthesis + implementation happen automatically)
##   3. File -> Export -> Export Hardware (include bitstream)
##   4. Open Vitis: Tools -> Launch Vitis IDE
##   5. Create a new bare-metal application using the exported HW platform
##   6. Add sw/nand_dump.c to the application project
##   7. Build and program the board
################################################################################

set project_name "nand_dumper"
set project_dir  [file normalize [file dirname [info script]]/..]
set hdl_dir      "$project_dir/hdl"
set xdc_dir      "$project_dir/constraints"

# Target part: Zynq XC7Z020-1CLG400C (Arty Z7-20)
# For Arty Z7-10, change to: xc7z010clg400-1
set part "xc7z020clg400-1"

# Create project
create_project $project_name "$project_dir/$project_name" -part $part -force

# Set board preset if Digilent board files are installed
# catch: ignore error if board files not installed
catch {
    set_property board_part digilentinc.com:arty-z7-20:part0:1.1 [current_project]
}

# Add HDL sources
add_files -norecurse [glob $hdl_dir/*.vhd]
set_property library work [get_files *.vhd]

# Add constraints
add_files -fileset constrs_1 -norecurse $xdc_dir/arty_z7_pmod_nand.xdc

# Set top module
set_property top nand_dumper_top [current_fileset]

################################################################################
## Create Block Design with Zynq PS
################################################################################
create_bd_design "zynq_ps_bd"

# Add Zynq Processing System
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7

# Apply Arty Z7 board preset (configures DDR, MIO, clocks, UART)
catch {
    apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 \
        -config {make_external "FIXED_IO, DDR" Master "Disable" Slave "Disable"} \
        [get_bd_cells ps7]
}

# Configure PS: Enable M_AXI_GP0, FCLK_CLK0 at 100 MHz, UART1
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_EN_RST0_PORT             {1} \
    CONFIG.PCW_UART1_PERIPHERAL_ENABLE  {1} \
    CONFIG.PCW_UART1_BAUD_RATE          {921600} \
] [get_bd_cells ps7]

# Create AXI interconnect (PS GP0 master -> our AXI slave)
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_ic
set_property CONFIG.NUM_MI {1} [get_bd_cells axi_ic]
set_property CONFIG.NUM_SI {1} [get_bd_cells axi_ic]

# Connect clocks and resets
connect_bd_net [get_bd_pins ps7/FCLK_CLK0]    [get_bd_pins axi_ic/ACLK]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0]    [get_bd_pins axi_ic/S00_ACLK]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0]    [get_bd_pins axi_ic/M00_ACLK]
connect_bd_net [get_bd_pins ps7/FCLK_CLK0]    [get_bd_pins ps7/M_AXI_GP0_ACLK]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins axi_ic/ARESETN]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins axi_ic/S00_ARESETN]
connect_bd_net [get_bd_pins ps7/FCLK_RESET0_N] [get_bd_pins axi_ic/M00_ARESETN]

# Connect PS GP0 -> AXI interconnect
connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0] \
                    [get_bd_intf_pins axi_ic/S00_AXI]

# Make FCLK_CLK0 and reset external for RTL top-level use
make_bd_pins_external [get_bd_pins ps7/FCLK_CLK0]
make_bd_pins_external [get_bd_pins ps7/FCLK_RESET0_N]

# Make M00_AXI external (will connect to our RTL axi_nand_ctrl)
make_bd_intf_pins_external [get_bd_intf_pins axi_ic/M00_AXI]

# Set address map: AXI slave at 0x40000000, 64 KB
assign_bd_address -target_address_space /ps7/Data \
    [get_bd_addr_segs {M00_AXI/Reg}] \
    -range 64K -offset 0x40000000

# Validate and save
validate_bd_design
save_bd_design

# Generate block design output products
generate_target all [get_files zynq_ps_bd.bd]

# Create HDL wrapper for the block design
make_wrapper -files [get_files zynq_ps_bd.bd] -top
add_files -norecurse [glob $project_dir/$project_name/$project_name.gen/sources_1/bd/zynq_ps_bd/hdl/zynq_ps_bd_wrapper.vhd]

################################################################################
## Synthesis and Implementation Settings
################################################################################
set_property strategy Flow_PerfOptimized_high [get_runs synth_1]
set_property strategy Performance_ExploreWithRemap [get_runs impl_1]

puts ""
puts "================================================================"
puts "  Project created: $project_dir/$project_name/$project_name.xpr"
puts ""
puts "  NOTE: The top-level entity (nand_dumper_top.vhd) instantiates"
puts "  a component 'zynq_ps_wrapper'. You need to rename the generated"
puts "  block design wrapper to match, or update the top-level to use"
puts "  the generated wrapper name (zynq_ps_bd_wrapper)."
puts ""
puts "  Next steps:"
puts "    1. Open the project in Vivado"
puts "    2. Update component instantiation names if needed"
puts "    3. Run synthesis and implementation"
puts "    4. Generate bitstream"
puts "    5. Export hardware to Vitis"
puts "    6. Build bare-metal app with sw/nand_dump.c"
puts "================================================================"
