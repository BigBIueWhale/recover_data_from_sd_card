--------------------------------------------------------------------------------
-- nand_pkg.vhd
-- Package: ONFI NAND flash constants, timing parameters, and types
--
-- Target: Digilent Arty Z7 (Zynq XC7Z020) with PL clock at 100 MHz
-- Purpose: DIY chip-off recovery — read raw NAND pages from a desoldered
--          flash die, bypassing the SD card controller's FTL/TRIM logic.
--------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package nand_pkg is

    ---------------------------------------------------------------------------
    -- ONFI standard command opcodes
    ---------------------------------------------------------------------------
    constant CMD_PAGE_READ_1   : std_logic_vector(7 downto 0) := x"00";
    constant CMD_PAGE_READ_2   : std_logic_vector(7 downto 0) := x"30";
    constant CMD_READ_ID       : std_logic_vector(7 downto 0) := x"90";
    constant CMD_READ_STATUS   : std_logic_vector(7 downto 0) := x"70";
    constant CMD_READ_PARAM    : std_logic_vector(7 downto 0) := x"EC";
    constant CMD_RESET         : std_logic_vector(7 downto 0) := x"FF";

    ---------------------------------------------------------------------------
    -- Timing constants (clock cycles at 100 MHz, 10 ns per cycle)
    -- Conservative values that exceed ONFI async Mode 0 minimums.
    ---------------------------------------------------------------------------
    constant T_SETUP : natural := 2;   -- CLE/ALE/data setup (20 ns >= 12 ns)
    constant T_WP    : natural := 3;   -- WE# pulse low      (30 ns >= 12 ns)
    constant T_WH    : natural := 2;   -- WE# hold high      (20 ns >= 10 ns)
    constant T_RP    : natural := 3;   -- RE# pulse low       (30 ns >= 12 ns)
    constant T_REH   : natural := 2;   -- RE# hold high      (20 ns >= 10 ns)
    constant T_WHR   : natural := 8;   -- WE# high to RE# low (80 ns >= 60 ns)
    constant T_RR    : natural := 3;   -- R/B# rise to RE# low (30 ns >= 20 ns)
    constant T_RST   : natural := 100000; -- Reset recovery (1 ms, worst case)
    constant T_CE_SETUP : natural := 2;  -- CE# assert to first bus op (20 ns)

    ---------------------------------------------------------------------------
    -- Maximum NAND page size including spare/OOB area (bytes).
    -- Covers 16 KB data + 2 KB spare. Adjustable via generic on controller.
    ---------------------------------------------------------------------------
    constant MAX_PAGE_BYTES : natural := 18432;

    ---------------------------------------------------------------------------
    -- Operation type enumeration
    ---------------------------------------------------------------------------
    type nand_op_t is (
        OP_NOP,
        OP_RESET,
        OP_READ_ID,
        OP_READ_STATUS,
        OP_READ_PAGE,
        OP_READ_PARAM
    );

    -- Encode op type to 3-bit value for AXI register interface
    function op_encode(op : nand_op_t) return std_logic_vector;
    function op_decode(v  : std_logic_vector(2 downto 0)) return nand_op_t;

end package nand_pkg;

package body nand_pkg is

    function op_encode(op : nand_op_t) return std_logic_vector is
    begin
        case op is
            when OP_NOP         => return "000";
            when OP_RESET       => return "001";
            when OP_READ_ID     => return "010";
            when OP_READ_STATUS => return "011";
            when OP_READ_PAGE   => return "100";
            when OP_READ_PARAM  => return "101";
        end case;
    end function;

    function op_decode(v : std_logic_vector(2 downto 0)) return nand_op_t is
    begin
        case v is
            when "001"  => return OP_RESET;
            when "010"  => return OP_READ_ID;
            when "011"  => return OP_READ_STATUS;
            when "100"  => return OP_READ_PAGE;
            when "101"  => return OP_READ_PARAM;
            when others => return OP_NOP;
        end case;
    end function;

end package body nand_pkg;
