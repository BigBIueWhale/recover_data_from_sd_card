################################################################################
## run_sim.tcl
## Vivado simulation script for the NAND controller testbench
##
## Usage from Vivado Tcl console:
##   cd <project_dir>/fpga_nand_recovery
##   source sim/run_sim.tcl
##
## Or standalone with xsim:
##   cd fpga_nand_recovery/sim
##   xvhdl ../hdl/nand_pkg.vhd
##   xvhdl ../hdl/nand_flash_ctrl.vhd
##   xvhdl tb_nand_flash_ctrl.vhd
##   xelab tb_nand_flash_ctrl -debug typical
##   xsim tb_nand_flash_ctrl -runall
################################################################################

# Compile sources in dependency order
set sim_dir [file dirname [info script]]
set hdl_dir [file normalize "$sim_dir/../hdl"]

xvhdl "$hdl_dir/nand_pkg.vhd"
xvhdl "$hdl_dir/nand_flash_ctrl.vhd"
xvhdl "$sim_dir/tb_nand_flash_ctrl.vhd"

# Elaborate
xelab tb_nand_flash_ctrl -debug typical -s tb_sim

# Run simulation
xsim tb_sim -runall

puts ""
puts "Simulation complete. Check transcript for test results."
