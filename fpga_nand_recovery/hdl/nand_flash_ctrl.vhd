--------------------------------------------------------------------------------
-- nand_flash_ctrl.vhd
-- ONFI NAND Flash Controller — Low-level bus protocol engine
--
-- Implements READ PAGE, READ ID, READ STATUS, READ PARAMETER PAGE, and RESET
-- operations with proper ONFI async Mode 0 timing.
--
-- Architecture: Two-level FSM
--   seq_state : Operation sequencer (which phase of the operation)
--   bus_state : Bus cycle engine (signal timing within a single byte transfer)
--
-- The controller drives the 8-bit NAND data bus (directly to I/O pads via
-- separate I/O/T signals for IOBUF inference), plus CLE, ALE, CE#, WE#, RE#,
-- WP# outputs and R/B# input.
--
-- Data bytes read from the NAND are presented one at a time on rd_data with
-- rd_valid asserted for one clock cycle per byte. The parent module must
-- capture them (e.g., into a BRAM page buffer).
--------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

use work.nand_pkg.all;

entity nand_flash_ctrl is
    port (
        clk           : in  std_logic;
        rst           : in  std_logic;  -- synchronous active-high

        -- Command interface (active-high pulse on cmd_start for one cycle)
        cmd_start     : in  std_logic;
        cmd_op        : in  nand_op_t;
        cmd_done      : out std_logic;  -- pulses high for one cycle on completion
        cmd_busy      : out std_logic;  -- high while operation in progress

        -- Address inputs (latched on cmd_start)
        addr_col      : in  std_logic_vector(15 downto 0);  -- column (byte offset)
        addr_row      : in  std_logic_vector(23 downto 0);  -- row (page + block)

        -- Number of bytes to read back (latched on cmd_start)
        rd_byte_cnt   : in  unsigned(15 downto 0);

        -- Read data output
        rd_data       : out std_logic_vector(7 downto 0);
        rd_valid      : out std_logic;

        -- Results (stable after cmd_done)
        id_data       : out std_logic_vector(39 downto 0);
        status_data   : out std_logic_vector(7 downto 0);

        -- NAND physical interface
        nand_io_i     : in  std_logic_vector(7 downto 0);   -- pad -> FPGA
        nand_io_o     : out std_logic_vector(7 downto 0);   -- FPGA -> pad
        nand_io_t     : out std_logic;  -- '1' = tristate (input), '0' = drive
        nand_cle      : out std_logic;
        nand_ale      : out std_logic;
        nand_ce_n     : out std_logic;
        nand_we_n     : out std_logic;
        nand_re_n     : out std_logic;
        nand_wp_n     : out std_logic;
        nand_rb_n     : in  std_logic   -- active low: '0'=busy, '1'=ready
    );
end entity nand_flash_ctrl;

architecture rtl of nand_flash_ctrl is

    ---------------------------------------------------------------------------
    -- Operation sequencer states
    ---------------------------------------------------------------------------
    type seq_state_t is (
        SEQ_IDLE,
        SEQ_CE_ON,         -- Assert CE#, brief setup
        SEQ_CMD1,          -- Send first command byte (CLE cycle)
        SEQ_ADDR,          -- Send address bytes (ALE cycles, loop)
        SEQ_CMD2,          -- Send second command byte (READ PAGE confirm)
        SEQ_WAIT_RB,       -- Wait for R/B# to go high
        SEQ_POST_WAIT,     -- tRR / tWHR delay after ready
        SEQ_READ,          -- Read data bytes (RE# cycles, loop)
        SEQ_CE_OFF,        -- Deassert CE#
        SEQ_DONE
    );

    ---------------------------------------------------------------------------
    -- Bus cycle engine states (executes one byte write or read)
    ---------------------------------------------------------------------------
    type bus_state_t is (
        BUS_IDLE,
        BUS_WR_SETUP,     -- CLE/ALE + data driven; wait T_SETUP
        BUS_WR_WE_LO,     -- WE# asserted low; wait T_WP
        BUS_WR_WE_HI,     -- WE# released high; wait T_WH
        BUS_RD_RE_LO,     -- RE# asserted low; wait T_RP
        BUS_RD_CAPTURE     -- RE# released high; data captured; wait T_REH
    );

    -- FSM registers
    signal seq         : seq_state_t := SEQ_IDLE;
    signal bus         : bus_state_t := BUS_IDLE;

    -- Timing counter (shared by bus engine)
    signal timer       : unsigned(17 downto 0) := (others => '0');

    -- Latched operation parameters
    signal cur_op      : nand_op_t := OP_NOP;
    signal lat_col     : std_logic_vector(15 downto 0);
    signal lat_row     : std_logic_vector(23 downto 0);
    signal lat_rd_cnt  : unsigned(15 downto 0);

    -- Address byte tracking
    signal addr_packed : std_logic_vector(39 downto 0);  -- 5 bytes
    signal addr_idx    : unsigned(2 downto 0);  -- current byte index
    signal addr_total  : unsigned(2 downto 0);  -- total address bytes for op

    -- Read byte tracking
    signal rd_idx      : unsigned(15 downto 0);
    signal rd_total    : unsigned(15 downto 0);

    -- Bus cycle control
    signal bus_go_wr   : std_logic;  -- start a write cycle
    signal bus_go_rd   : std_logic;  -- start a read cycle
    signal bus_wr_byte : std_logic_vector(7 downto 0);  -- data to write
    signal bus_cle     : std_logic;  -- CLE value during write
    signal bus_ale     : std_logic;  -- ALE value during write
    signal bus_done    : std_logic;  -- bus cycle complete (one cycle pulse)
    signal bus_rd_byte : std_logic_vector(7 downto 0);  -- captured read data

    -- Output registers
    signal r_ce_n      : std_logic := '1';
    signal r_we_n      : std_logic := '1';
    signal r_re_n      : std_logic := '1';
    signal r_cle       : std_logic := '0';
    signal r_ale       : std_logic := '0';
    signal r_io_o      : std_logic_vector(7 downto 0) := (others => '0');
    signal r_io_t      : std_logic := '1';  -- default tristate (input)
    signal r_wp_n      : std_logic := '1';  -- default: not write-protected

    -- Result registers
    signal r_id        : std_logic_vector(39 downto 0) := (others => '0');
    signal r_status    : std_logic_vector(7 downto 0) := (others => '0');

    -- Metastability synchroniser for R/B#
    signal rb_sync     : std_logic_vector(2 downto 0) := (others => '0');
    signal rb_safe     : std_logic;  -- synchronised ready/busy

    -- Sequencer flags for current operation
    signal op_has_addr : std_logic;
    signal op_has_cmd2 : std_logic;
    signal op_has_wait : std_logic;
    signal op_has_read : std_logic;

begin

    ---------------------------------------------------------------------------
    -- Continuous assignments
    ---------------------------------------------------------------------------
    nand_ce_n  <= r_ce_n;
    nand_we_n  <= r_we_n;
    nand_re_n  <= r_re_n;
    nand_cle   <= r_cle;
    nand_ale   <= r_ale;
    nand_io_o  <= r_io_o;
    nand_io_t  <= r_io_t;
    nand_wp_n  <= r_wp_n;

    id_data     <= r_id;
    status_data <= r_status;

    rb_safe <= rb_sync(2);

    ---------------------------------------------------------------------------
    -- R/B# synchroniser (three-stage for metastability protection)
    ---------------------------------------------------------------------------
    process(clk)
    begin
        if rising_edge(clk) then
            rb_sync <= rb_sync(1 downto 0) & nand_rb_n;
        end if;
    end process;

    ---------------------------------------------------------------------------
    -- Operation parameter decode (combinational)
    ---------------------------------------------------------------------------
    process(cur_op)
    begin
        case cur_op is
            when OP_READ_PAGE =>
                op_has_addr <= '1'; op_has_cmd2 <= '1';
                op_has_wait <= '1'; op_has_read <= '1';
            when OP_READ_ID =>
                op_has_addr <= '1'; op_has_cmd2 <= '0';
                op_has_wait <= '0'; op_has_read <= '1';
            when OP_READ_STATUS =>
                op_has_addr <= '0'; op_has_cmd2 <= '0';
                op_has_wait <= '0'; op_has_read <= '1';
            when OP_RESET =>
                op_has_addr <= '0'; op_has_cmd2 <= '0';
                op_has_wait <= '1'; op_has_read <= '0';
            when OP_READ_PARAM =>
                op_has_addr <= '1'; op_has_cmd2 <= '0';
                op_has_wait <= '1'; op_has_read <= '1';
            when others =>
                op_has_addr <= '0'; op_has_cmd2 <= '0';
                op_has_wait <= '0'; op_has_read <= '0';
        end case;
    end process;

    ---------------------------------------------------------------------------
    -- Bus cycle engine
    -- Executes a single write-byte (CLE or ALE) or read-byte (RE#) cycle.
    ---------------------------------------------------------------------------
    process(clk)
    begin
        if rising_edge(clk) then
            if rst = '1' then
                bus      <= BUS_IDLE;
                timer    <= (others => '0');
                bus_done <= '0';
                r_we_n   <= '1';
                r_re_n   <= '1';
                r_cle    <= '0';
                r_ale    <= '0';
                r_io_t   <= '1';
                r_io_o   <= (others => '0');
            else
                bus_done <= '0';

                case bus is
                    --------------------------------------------------------
                    when BUS_IDLE =>
                        if bus_go_wr = '1' then
                            -- Begin write cycle: drive CLE/ALE + data
                            r_cle   <= bus_cle;
                            r_ale   <= bus_ale;
                            r_io_o  <= bus_wr_byte;
                            r_io_t  <= '0';       -- drive bus
                            timer   <= to_unsigned(T_SETUP, timer'length);
                            bus     <= BUS_WR_SETUP;
                        elsif bus_go_rd = '1' then
                            -- Begin read cycle: tristate bus, assert RE#
                            r_io_t  <= '1';       -- tristate
                            r_cle   <= '0';
                            r_ale   <= '0';
                            timer   <= to_unsigned(T_RP, timer'length);
                            r_re_n  <= '0';
                            bus     <= BUS_RD_RE_LO;
                        end if;

                    --------------------------------------------------------
                    -- Write cycle
                    --------------------------------------------------------
                    when BUS_WR_SETUP =>
                        if timer = 0 then
                            r_we_n <= '0';  -- assert WE#
                            timer  <= to_unsigned(T_WP, timer'length);
                            bus    <= BUS_WR_WE_LO;
                        else
                            timer <= timer - 1;
                        end if;

                    when BUS_WR_WE_LO =>
                        if timer = 0 then
                            r_we_n <= '1';  -- release WE# (data latched on rising edge)
                            timer  <= to_unsigned(T_WH, timer'length);
                            bus    <= BUS_WR_WE_HI;
                        else
                            timer <= timer - 1;
                        end if;

                    when BUS_WR_WE_HI =>
                        if timer = 0 then
                            r_cle    <= '0';
                            r_ale    <= '0';
                            r_io_t   <= '1';  -- tristate bus
                            bus_done <= '1';
                            bus      <= BUS_IDLE;
                        else
                            timer <= timer - 1;
                        end if;

                    --------------------------------------------------------
                    -- Read cycle
                    --------------------------------------------------------
                    when BUS_RD_RE_LO =>
                        if timer = 0 then
                            bus_rd_byte <= nand_io_i;  -- capture data
                            r_re_n      <= '1';        -- release RE#
                            timer       <= to_unsigned(T_REH, timer'length);
                            bus         <= BUS_RD_CAPTURE;
                        else
                            timer <= timer - 1;
                        end if;

                    when BUS_RD_CAPTURE =>
                        if timer = 0 then
                            bus_done <= '1';
                            bus      <= BUS_IDLE;
                        else
                            timer <= timer - 1;
                        end if;

                end case;
            end if;
        end if;
    end process;

    ---------------------------------------------------------------------------
    -- Operation sequencer
    -- Steps through the phases of each NAND command using the bus engine.
    ---------------------------------------------------------------------------
    process(clk)

        -- Helper: select address byte by index
        function get_addr_byte(packed : std_logic_vector(39 downto 0);
                               idx    : unsigned(2 downto 0))
            return std_logic_vector is
        begin
            case to_integer(idx) is
                when 0 => return packed( 7 downto  0);
                when 1 => return packed(15 downto  8);
                when 2 => return packed(23 downto 16);
                when 3 => return packed(31 downto 24);
                when 4 => return packed(39 downto 32);
                when others => return x"00";
            end case;
        end function;

        -- Helper: get number of read bytes for the operation
        function get_rd_count(op  : nand_op_t;
                              cnt : unsigned(15 downto 0))
            return unsigned is
        begin
            case op is
                when OP_READ_PAGE  => return cnt;  -- user-specified
                when OP_READ_ID    => return to_unsigned(5, 16);
                when OP_READ_STATUS => return to_unsigned(1, 16);
                when OP_READ_PARAM => return cnt;   -- typically 256
                when others        => return to_unsigned(0, 16);
            end case;
        end function;

        -- Helper: number of address bytes for the operation
        function get_addr_count(op : nand_op_t) return unsigned is
        begin
            case op is
                when OP_READ_PAGE  => return to_unsigned(5, 3);  -- 2 col + 3 row
                when OP_READ_ID    => return to_unsigned(1, 3);  -- 1 byte (0x00)
                when OP_READ_PARAM => return to_unsigned(1, 3);  -- 1 byte (0x00)
                when others        => return to_unsigned(0, 3);
            end case;
        end function;

        -- Helper: command byte 1
        function get_cmd1(op : nand_op_t)
            return std_logic_vector is
        begin
            case op is
                when OP_READ_PAGE   => return CMD_PAGE_READ_1;
                when OP_READ_ID     => return CMD_READ_ID;
                when OP_READ_STATUS => return CMD_READ_STATUS;
                when OP_RESET       => return CMD_RESET;
                when OP_READ_PARAM  => return CMD_READ_PARAM;
                when others         => return x"00";
            end case;
        end function;

    begin
        if rising_edge(clk) then
            if rst = '1' then
                seq        <= SEQ_IDLE;
                cur_op     <= OP_NOP;
                r_ce_n     <= '1';
                bus_go_wr  <= '0';
                bus_go_rd  <= '0';
                cmd_done   <= '0';
                cmd_busy   <= '0';
                rd_valid   <= '0';
                rd_data    <= (others => '0');
                r_id       <= (others => '0');
                r_status   <= (others => '0');
            else
                -- Default: de-assert one-cycle pulses
                bus_go_wr <= '0';
                bus_go_rd <= '0';
                cmd_done  <= '0';
                rd_valid  <= '0';

                case seq is
                    ----------------------------------------------------
                    when SEQ_IDLE =>
                        cmd_busy <= '0';
                        if cmd_start = '1' and cmd_op /= OP_NOP then
                            cur_op    <= cmd_op;
                            lat_col   <= addr_col;
                            lat_row   <= addr_row;
                            lat_rd_cnt <= rd_byte_cnt;
                            cmd_busy  <= '1';
                            timer     <= to_unsigned(T_CE_SETUP, timer'length);
                            r_ce_n    <= '0';  -- assert CE#
                            seq       <= SEQ_CE_ON;
                        end if;

                    ----------------------------------------------------
                    when SEQ_CE_ON =>
                        -- Wait a brief setup time after CE# assertion
                        if timer = 0 then
                            -- Pack address bytes: col_lo, col_hi, row0, row1, row2
                            addr_packed <= lat_row & lat_col;
                            addr_idx    <= (others => '0');
                            addr_total  <= get_addr_count(cur_op);
                            rd_total    <= get_rd_count(cur_op, lat_rd_cnt);
                            rd_idx      <= (others => '0');

                            -- Start CMD1 write cycle
                            bus_wr_byte <= get_cmd1(cur_op);
                            bus_cle     <= '1';
                            bus_ale     <= '0';
                            bus_go_wr   <= '1';
                            seq         <= SEQ_CMD1;
                        else
                            timer <= timer - 1;
                        end if;

                    ----------------------------------------------------
                    when SEQ_CMD1 =>
                        if bus_done = '1' then
                            if op_has_addr = '1' and addr_total > 0 then
                                -- Start first address byte
                                bus_wr_byte <= get_addr_byte(addr_packed, addr_idx);
                                bus_cle     <= '0';
                                bus_ale     <= '1';
                                bus_go_wr   <= '1';
                                seq         <= SEQ_ADDR;
                            elsif op_has_cmd2 = '1' then
                                bus_wr_byte <= CMD_PAGE_READ_2;
                                bus_cle     <= '1';
                                bus_ale     <= '0';
                                bus_go_wr   <= '1';
                                seq         <= SEQ_CMD2;
                            elsif op_has_wait = '1' then
                                seq <= SEQ_WAIT_RB;
                            elsif op_has_read = '1' then
                                timer <= to_unsigned(T_WHR, timer'length);
                                seq   <= SEQ_POST_WAIT;
                            else
                                seq <= SEQ_CE_OFF;
                            end if;
                        end if;

                    ----------------------------------------------------
                    when SEQ_ADDR =>
                        if bus_done = '1' then
                            if addr_idx + 1 < addr_total then
                                addr_idx    <= addr_idx + 1;
                                bus_wr_byte <= get_addr_byte(addr_packed, addr_idx + 1);
                                bus_cle     <= '0';
                                bus_ale     <= '1';
                                bus_go_wr   <= '1';
                            elsif op_has_cmd2 = '1' then
                                bus_wr_byte <= CMD_PAGE_READ_2;
                                bus_cle     <= '1';
                                bus_ale     <= '0';
                                bus_go_wr   <= '1';
                                seq         <= SEQ_CMD2;
                            elsif op_has_wait = '1' then
                                seq <= SEQ_WAIT_RB;
                            elsif op_has_read = '1' then
                                timer <= to_unsigned(T_WHR, timer'length);
                                seq   <= SEQ_POST_WAIT;
                            else
                                seq <= SEQ_CE_OFF;
                            end if;
                        end if;

                    ----------------------------------------------------
                    when SEQ_CMD2 =>
                        if bus_done = '1' then
                            if op_has_wait = '1' then
                                seq <= SEQ_WAIT_RB;
                            elsif op_has_read = '1' then
                                timer <= to_unsigned(T_WHR, timer'length);
                                seq   <= SEQ_POST_WAIT;
                            else
                                seq <= SEQ_CE_OFF;
                            end if;
                        end if;

                    ----------------------------------------------------
                    when SEQ_WAIT_RB =>
                        -- Wait for R/B# to go high (ready)
                        if rb_safe = '1' then
                            if op_has_read = '1' then
                                timer <= to_unsigned(T_RR, timer'length);
                                seq   <= SEQ_POST_WAIT;
                            else
                                seq <= SEQ_CE_OFF;
                            end if;
                        end if;
                        -- Watchdog: timer counts up while waiting
                        -- (could add timeout here for error detection)

                    ----------------------------------------------------
                    when SEQ_POST_WAIT =>
                        -- tRR or tWHR delay before first read
                        if timer = 0 then
                            if rd_total > 0 then
                                bus_go_rd <= '1';
                                seq       <= SEQ_READ;
                            else
                                seq <= SEQ_CE_OFF;
                            end if;
                        else
                            timer <= timer - 1;
                        end if;

                    ----------------------------------------------------
                    when SEQ_READ =>
                        if bus_done = '1' then
                            -- Output captured byte
                            rd_data  <= bus_rd_byte;
                            rd_valid <= '1';

                            -- Store into ID / status registers as appropriate
                            case cur_op is
                                when OP_READ_ID =>
                                    case to_integer(rd_idx) is
                                        when 0 => r_id( 7 downto  0) <= bus_rd_byte;
                                        when 1 => r_id(15 downto  8) <= bus_rd_byte;
                                        when 2 => r_id(23 downto 16) <= bus_rd_byte;
                                        when 3 => r_id(31 downto 24) <= bus_rd_byte;
                                        when 4 => r_id(39 downto 32) <= bus_rd_byte;
                                        when others => null;
                                    end case;
                                when OP_READ_STATUS =>
                                    r_status <= bus_rd_byte;
                                when others =>
                                    null;
                            end case;

                            rd_idx <= rd_idx + 1;
                            if rd_idx + 1 < rd_total then
                                bus_go_rd <= '1';
                            else
                                seq <= SEQ_CE_OFF;
                            end if;
                        end if;

                    ----------------------------------------------------
                    when SEQ_CE_OFF =>
                        r_ce_n <= '1';  -- deassert CE#
                        seq    <= SEQ_DONE;

                    ----------------------------------------------------
                    when SEQ_DONE =>
                        cmd_done <= '1';
                        cmd_busy <= '0';
                        seq      <= SEQ_IDLE;

                end case;
            end if;
        end if;
    end process;

end architecture rtl;
