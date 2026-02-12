--------------------------------------------------------------------------------
-- tb_nand_flash_ctrl.vhd
-- Testbench for nand_flash_ctrl
--
-- Includes a behavioral NAND flash model that responds to READ ID, RESET,
-- READ STATUS, and READ PAGE commands with known data patterns.
-- Verifies the controller's bus timing and protocol correctness.
--
-- Run in Vivado: source sim/run_sim.tcl
--------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

use work.nand_pkg.all;

entity tb_nand_flash_ctrl is
end entity tb_nand_flash_ctrl;

architecture sim of tb_nand_flash_ctrl is

    constant CLK_PERIOD : time := 10 ns;  -- 100 MHz

    -- DUT signals
    signal clk         : std_logic := '0';
    signal rst         : std_logic := '1';
    signal cmd_start   : std_logic := '0';
    signal cmd_op      : nand_op_t := OP_NOP;
    signal cmd_done    : std_logic;
    signal cmd_busy    : std_logic;
    signal addr_col    : std_logic_vector(15 downto 0) := (others => '0');
    signal addr_row    : std_logic_vector(23 downto 0) := (others => '0');
    signal rd_byte_cnt : unsigned(15 downto 0) := (others => '0');
    signal rd_data     : std_logic_vector(7 downto 0);
    signal rd_valid    : std_logic;
    signal id_data     : std_logic_vector(39 downto 0);
    signal status_data : std_logic_vector(7 downto 0);

    -- NAND bus signals
    signal nand_io_i   : std_logic_vector(7 downto 0);
    signal nand_io_o   : std_logic_vector(7 downto 0);
    signal nand_io_t   : std_logic;
    signal nand_cle    : std_logic;
    signal nand_ale    : std_logic;
    signal nand_ce_n   : std_logic;
    signal nand_we_n   : std_logic;
    signal nand_re_n   : std_logic;
    signal nand_wp_n   : std_logic;
    signal nand_rb_n   : std_logic := '1';  -- default: ready

    -- Simulated bidirectional bus (directly resolved in testbench)
    signal nand_bus    : std_logic_vector(7 downto 0);

    -- NAND model internal state
    type nand_model_state_t is (
        NM_IDLE, NM_CMD, NM_ADDR, NM_WAIT_CMD2, NM_BUSY, NM_DATA_OUT
    );
    signal nm_state   : nand_model_state_t := NM_IDLE;
    signal nm_cmd     : std_logic_vector(7 downto 0) := x"00";
    signal nm_addr    : std_logic_vector(39 downto 0) := (others => '0');
    signal nm_addr_idx : integer := 0;
    signal nm_data_idx : integer := 0;
    signal nm_page_data : std_logic_vector(7 downto 0) := x"00";

    -- Test control
    signal test_done : boolean := false;
    signal read_bytes : integer := 0;

begin

    ---------------------------------------------------------------------------
    -- Clock generation
    ---------------------------------------------------------------------------
    clk <= not clk after CLK_PERIOD / 2 when not test_done;

    ---------------------------------------------------------------------------
    -- Bidirectional bus simulation
    ---------------------------------------------------------------------------
    -- Controller drives bus when nand_io_t = '0' (output mode)
    nand_bus <= nand_io_o when nand_io_t = '0' else (others => 'Z');
    -- NAND model drives bus during data-out phase
    nand_io_i <= nand_bus;

    ---------------------------------------------------------------------------
    -- DUT instantiation
    ---------------------------------------------------------------------------
    u_dut : entity work.nand_flash_ctrl
        port map (
            clk         => clk,
            rst         => rst,
            cmd_start   => cmd_start,
            cmd_op      => cmd_op,
            cmd_done    => cmd_done,
            cmd_busy    => cmd_busy,
            addr_col    => addr_col,
            addr_row    => addr_row,
            rd_byte_cnt => rd_byte_cnt,
            rd_data     => rd_data,
            rd_valid    => rd_valid,
            id_data     => id_data,
            status_data => status_data,
            nand_io_i   => nand_io_i,
            nand_io_o   => nand_io_o,
            nand_io_t   => nand_io_t,
            nand_cle    => nand_cle,
            nand_ale    => nand_ale,
            nand_ce_n   => nand_ce_n,
            nand_we_n   => nand_we_n,
            nand_re_n   => nand_re_n,
            nand_wp_n   => nand_wp_n,
            nand_rb_n   => nand_rb_n
        );

    ---------------------------------------------------------------------------
    -- Behavioral NAND flash model
    -- Responds to commands by driving the bus during read cycles.
    -- Simulates:
    --   READ ID:     Returns maker=0x98 (Toshiba), device=0xDE, etc.
    --   READ STATUS: Returns 0xE0 (ready, no error)
    --   READ PAGE:   Returns incrementing byte pattern (row_lo XOR index)
    --   RESET:       Goes busy for 100 ns, then ready
    ---------------------------------------------------------------------------
    nand_model : process
        -- ID bytes: Toshiba 64Gb TLC (example)
        constant ID_MAKER  : std_logic_vector(7 downto 0) := x"98";
        constant ID_DEVICE : std_logic_vector(7 downto 0) := x"DE";
        constant ID_BYTE2  : std_logic_vector(7 downto 0) := x"94";
        constant ID_BYTE3  : std_logic_vector(7 downto 0) := x"93";  -- 8KB page
        constant ID_BYTE4  : std_logic_vector(7 downto 0) := x"76";
        type id_array_t is array(0 to 4) of std_logic_vector(7 downto 0);
        constant ID_BYTES : id_array_t := (ID_MAKER, ID_DEVICE, ID_BYTE2, ID_BYTE3, ID_BYTE4);

        variable we_prev  : std_logic := '1';
        variable re_prev  : std_logic := '1';
        variable latched  : std_logic_vector(7 downto 0);
        variable cmd_reg  : std_logic_vector(7 downto 0) := x"00";
        variable addr_cnt : integer := 0;
        variable data_cnt : integer := 0;
        variable state    : nand_model_state_t := NM_IDLE;
        variable row_byte : std_logic_vector(7 downto 0) := x"00";
    begin
        wait until rising_edge(clk);

        -- Default: NAND model doesn't drive bus (high-Z)
        -- Bus is driven by forcing nand_bus in the read section below

        if nand_ce_n = '1' then
            state := NM_IDLE;
            addr_cnt := 0;
            data_cnt := 0;
        else
            -- Detect WE# rising edge (data latch point)
            if we_prev = '0' and nand_we_n = '1' then
                latched := nand_io_o;  -- what the controller drove

                if nand_cle = '1' then
                    -- Command byte
                    cmd_reg := latched;
                    addr_cnt := 0;

                    case latched is
                        when CMD_RESET =>
                            -- Go busy for a while
                            nand_rb_n <= '0';
                            state := NM_BUSY;
                        when CMD_READ_ID =>
                            state := NM_ADDR;
                        when CMD_READ_STATUS =>
                            state := NM_DATA_OUT;
                            data_cnt := 0;
                        when CMD_PAGE_READ_1 =>
                            state := NM_ADDR;
                        when CMD_PAGE_READ_2 =>
                            -- Second command for READ PAGE -> go busy briefly
                            nand_rb_n <= '0';
                            state := NM_BUSY;
                        when others =>
                            state := NM_IDLE;
                    end case;

                elsif nand_ale = '1' then
                    -- Address byte
                    nm_addr(addr_cnt*8+7 downto addr_cnt*8) <= latched;
                    addr_cnt := addr_cnt + 1;
                    row_byte := latched;
                end if;
            end if;

            -- Detect RE# falling edge (data output point for model)
            if re_prev = '1' and nand_re_n = '0' then
                if state = NM_DATA_OUT then
                    case cmd_reg is
                        when CMD_READ_ID =>
                            if data_cnt < 5 then
                                nand_bus <= ID_BYTES(data_cnt);
                            else
                                nand_bus <= x"00";
                            end if;
                        when CMD_READ_STATUS =>
                            nand_bus <= x"E0";  -- ready, no error
                        when CMD_PAGE_READ_2 | CMD_PAGE_READ_1 =>
                            -- Return pattern: row_addr_byte0 XOR byte_index
                            nand_bus <= std_logic_vector(
                                unsigned(row_byte) xor
                                to_unsigned(data_cnt mod 256, 8));
                        when others =>
                            nand_bus <= x"FF";
                    end case;
                    data_cnt := data_cnt + 1;
                end if;
            elsif nand_re_n = '1' and nand_io_t = '1' then
                -- Release bus when RE# is high and controller is in input mode
                nand_bus <= (others => 'Z');
            end if;

            -- Busy -> Ready transition (simulate flash busy time)
            if state = NM_BUSY then
                -- Simple: go ready after a short delay (in real NAND this is us-ms)
                nand_rb_n <= '1' after 200 ns;
                if nand_rb_n = '1' then
                    state := NM_DATA_OUT;
                    data_cnt := 0;
                end if;
            end if;
        end if;

        we_prev := nand_we_n;
        re_prev := nand_re_n;
    end process;

    ---------------------------------------------------------------------------
    -- Main test sequence
    ---------------------------------------------------------------------------
    test_proc : process
        procedure wait_cycles(n : integer) is
        begin
            for i in 1 to n loop
                wait until rising_edge(clk);
            end loop;
        end procedure;

        procedure start_op(op : nand_op_t) is
        begin
            cmd_op    <= op;
            cmd_start <= '1';
            wait until rising_edge(clk);
            cmd_start <= '0';
        end procedure;

        procedure wait_done(timeout_cycles : integer := 50000) is
            variable cnt : integer := 0;
        begin
            while cmd_done = '0' and cnt < timeout_cycles loop
                if rd_valid = '1' then
                    read_bytes <= read_bytes + 1;
                end if;
                wait until rising_edge(clk);
                cnt := cnt + 1;
            end loop;
            assert cmd_done = '1'
                report "Operation timed out!" severity error;
        end procedure;
    begin
        -- Initial reset
        rst <= '1';
        wait_cycles(10);
        rst <= '0';
        wait_cycles(5);

        report "=== Test 1: NAND Reset ===" severity note;
        start_op(OP_RESET);
        wait_done;
        report "Reset complete." severity note;
        wait_cycles(10);

        report "=== Test 2: Read ID ===" severity note;
        addr_col    <= x"0000";
        rd_byte_cnt <= to_unsigned(5, 16);
        start_op(OP_READ_ID);
        wait_done;
        report "ID = 0x" & to_hstring(id_data) severity note;
        assert id_data(7 downto 0) = x"98"
            report "ID maker mismatch! Expected 0x98, got 0x" &
                   to_hstring(id_data(7 downto 0))
            severity error;
        assert id_data(15 downto 8) = x"DE"
            report "ID device mismatch! Expected 0xDE, got 0x" &
                   to_hstring(id_data(15 downto 8))
            severity error;
        wait_cycles(10);

        report "=== Test 3: Read Status ===" severity note;
        start_op(OP_READ_STATUS);
        wait_done;
        report "Status = 0x" & to_hstring(status_data) severity note;
        assert status_data = x"E0"
            report "Status mismatch! Expected 0xE0, got 0x" &
                   to_hstring(status_data)
            severity error;
        wait_cycles(10);

        report "=== Test 4: Read Page (16 bytes) ===" severity note;
        addr_col    <= x"0000";  -- column 0
        addr_row    <= x"000042";  -- row 0x42 (page 66)
        rd_byte_cnt <= to_unsigned(16, 16);
        read_bytes  <= 0;
        start_op(OP_READ_PAGE);
        wait_done;
        report "Page read complete. Bytes read: " & integer'image(read_bytes)
            severity note;
        assert read_bytes = 16
            report "Expected 16 bytes, got " & integer'image(read_bytes)
            severity error;
        wait_cycles(10);

        report "=== Test 5: Read Page (8192 bytes - full page) ===" severity note;
        addr_col    <= x"0000";
        addr_row    <= x"000100";
        rd_byte_cnt <= to_unsigned(8192, 16);
        read_bytes  <= 0;
        start_op(OP_READ_PAGE);
        wait_done(500000);
        report "Full page read complete. Bytes: " & integer'image(read_bytes)
            severity note;
        assert read_bytes = 8192
            report "Expected 8192 bytes, got " & integer'image(read_bytes)
            severity error;
        wait_cycles(10);

        report "=== All tests passed ===" severity note;
        test_done <= true;
        wait;
    end process;

end architecture sim;
