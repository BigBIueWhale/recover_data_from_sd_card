################################################################################
## arty_z7_pmod_nand.xdc
## Pin constraints for Digilent Arty Z7-20 (Zynq XC7Z020-1CLG400C)
##
## IMPORTANT: Verify these pin assignments against the Digilent Arty Z7
## schematic and master XDC for your specific board revision before building.
## Download from: https://digilent.com/reference/programmable-logic/arty-z7/start
##
## Pmod JA -> NAND data bus I/O[7:0] (directly bidirectional)
## Pmod JB -> NAND control signals (CLE, ALE, CE#, WE#, RE#, WP#, R/B#)
################################################################################

## --------------------------------------------------------------------------
## Pmod JA: NAND data bus (directly bidirectional I/O)
## --------------------------------------------------------------------------
## JA pin 1 -> NAND I/O[0]
set_property -dict { PACKAGE_PIN Y18  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JA[0] }]
## JA pin 2 -> NAND I/O[1]
set_property -dict { PACKAGE_PIN Y19  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JA[1] }]
## JA pin 3 -> NAND I/O[2]
set_property -dict { PACKAGE_PIN Y16  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JA[2] }]
## JA pin 4 -> NAND I/O[3]
set_property -dict { PACKAGE_PIN Y17  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JA[3] }]
## JA pin 7 -> NAND I/O[4]
set_property -dict { PACKAGE_PIN U18  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JA[4] }]
## JA pin 8 -> NAND I/O[5]
set_property -dict { PACKAGE_PIN U19  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JA[5] }]
## JA pin 9 -> NAND I/O[6]
set_property -dict { PACKAGE_PIN W18  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JA[6] }]
## JA pin 10 -> NAND I/O[7]
set_property -dict { PACKAGE_PIN W19  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JA[7] }]

## --------------------------------------------------------------------------
## Pmod JB: NAND control signals
## --------------------------------------------------------------------------
## JB pin 1 -> NAND CLE (Command Latch Enable) - output
set_property -dict { PACKAGE_PIN W14  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JB[0] }]
## JB pin 2 -> NAND ALE (Address Latch Enable) - output
set_property -dict { PACKAGE_PIN Y14  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JB[1] }]
## JB pin 3 -> NAND CE# (Chip Enable, active low) - output
set_property -dict { PACKAGE_PIN T11  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JB[2] }]
## JB pin 4 -> NAND WE# (Write Enable, active low) - output
set_property -dict { PACKAGE_PIN T10  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JB[3] }]
## JB pin 7 -> NAND RE# (Read Enable, active low) - output
set_property -dict { PACKAGE_PIN V16  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JB[4] }]
## JB pin 8 -> NAND WP# (Write Protect, active low) - output
set_property -dict { PACKAGE_PIN W16  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JB[5] }]
## JB pin 9 -> NAND R/B# (Ready/Busy, active low) - input with pull-up
set_property -dict { PACKAGE_PIN V12  IOSTANDARD LVCMOS33  PULLUP TRUE } [get_ports { JB[6] }]
## JB pin 10 -> Debug output (active-low mirror of CE#)
set_property -dict { PACKAGE_PIN W13  IOSTANDARD LVCMOS33  SLEW SLOW  DRIVE 12 } [get_ports { JB[7] }]

## --------------------------------------------------------------------------
## Timing constraints
## --------------------------------------------------------------------------
## The PL clock comes from the Zynq PS FCLK_CLK0 (configured to 100 MHz).
## Vivado creates this clock automatically from the PS block design.
## We only need I/O timing constraints relative to that clock.

## NAND bus is asynchronous — all timing is handled in the FSM state machine.
## Set false paths on all NAND I/O to suppress timing analysis on these pins.
set_false_path -to   [get_ports { JA[*] }]
set_false_path -from [get_ports { JA[*] }]
set_false_path -to   [get_ports { JB[*] }]
set_false_path -from [get_ports { JB[*] }]
