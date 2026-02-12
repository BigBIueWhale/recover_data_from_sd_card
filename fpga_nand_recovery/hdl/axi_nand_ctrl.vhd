--------------------------------------------------------------------------------
-- axi_nand_ctrl.vhd
-- AXI4-Lite slave wrapper around the NAND flash controller
--
-- Provides a register interface for the Zynq PS (ARM Cortex-A9) to control
-- the NAND dump operation. Includes a dual-port BRAM page buffer that the
-- NAND controller writes into (byte-at-a-time from rd_valid) and the PS
-- reads from (word-at-a-time via AXI).
--
-- Register Map (active address bits [14:0], byte-addressed):
--   0x0000  CTRL      [W]  bit 0: start, bits [3:1]: op_type (nand_op_t)
--   0x0004  STATUS    [R]  bit 0: busy, bit 1: done (sticky, W1C via CTRL[4]),
--                          bit 2: rb_n state
--   0x0008  ADDR_COL  [RW] Column address [15:0]
--   0x000C  ADDR_ROW  [RW] Row address [23:0]
--   0x0010  RD_COUNT  [RW] Bytes to read [15:0]
--   0x0014  ID_LO     [R]  NAND ID bytes 0-3
--   0x0018  ID_HI     [R]  NAND ID byte 4 [7:0]
--   0x001C  NAND_STAT [R]  NAND status register byte
--   0x0020  PAGE_IDX  [R]  Number of bytes written into page buffer (last op)
--   0x0024  VERSION   [R]  Design version (0x4E414E44 = "NAND")
--
--   0x4000 - 0xBFFF  PAGE_BUF [R] Page buffer (up to 32 KB, 32-bit aligned)
--     Read word at 0x4000 + 4*N to get page bytes [4N+3 : 4N]
--------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

use work.nand_pkg.all;

entity axi_nand_ctrl is
    generic (
        C_S_AXI_DATA_WIDTH : integer := 32;
        C_S_AXI_ADDR_WIDTH : integer := 16;
        PAGE_BUF_DEPTH     : integer := 18432  -- bytes
    );
    port (
        -----------------------------------------------------------------
        -- AXI4-Lite slave interface
        -----------------------------------------------------------------
        s_axi_aclk    : in  std_logic;
        s_axi_aresetn : in  std_logic;

        s_axi_awaddr  : in  std_logic_vector(C_S_AXI_ADDR_WIDTH-1 downto 0);
        s_axi_awvalid : in  std_logic;
        s_axi_awready : out std_logic;

        s_axi_wdata   : in  std_logic_vector(C_S_AXI_DATA_WIDTH-1 downto 0);
        s_axi_wstrb   : in  std_logic_vector(C_S_AXI_DATA_WIDTH/8-1 downto 0);
        s_axi_wvalid  : in  std_logic;
        s_axi_wready  : out std_logic;

        s_axi_bresp   : out std_logic_vector(1 downto 0);
        s_axi_bvalid  : out std_logic;
        s_axi_bready  : in  std_logic;

        s_axi_araddr  : in  std_logic_vector(C_S_AXI_ADDR_WIDTH-1 downto 0);
        s_axi_arvalid : in  std_logic;
        s_axi_arready : out std_logic;

        s_axi_rdata   : out std_logic_vector(C_S_AXI_DATA_WIDTH-1 downto 0);
        s_axi_rresp   : out std_logic_vector(1 downto 0);
        s_axi_rvalid  : out std_logic;
        s_axi_rready  : in  std_logic;

        -----------------------------------------------------------------
        -- NAND flash physical interface (directly to I/O pads)
        -----------------------------------------------------------------
        nand_io_i     : in  std_logic_vector(7 downto 0);
        nand_io_o     : out std_logic_vector(7 downto 0);
        nand_io_t     : out std_logic;
        nand_cle      : out std_logic;
        nand_ale      : out std_logic;
        nand_ce_n     : out std_logic;
        nand_we_n     : out std_logic;
        nand_re_n     : out std_logic;
        nand_wp_n     : out std_logic;
        nand_rb_n     : in  std_logic
    );
end entity axi_nand_ctrl;

architecture rtl of axi_nand_ctrl is

    -- Page buffer: byte-addressable BRAM
    constant BUF_WORDS : integer := (PAGE_BUF_DEPTH + 3) / 4;
    type buf_array_t is array (0 to BUF_WORDS-1)
        of std_logic_vector(31 downto 0);
    signal page_buf    : buf_array_t := (others => (others => '0'));

    -- NAND controller signals
    signal ctrl_start     : std_logic := '0';
    signal ctrl_op        : nand_op_t := OP_NOP;
    signal ctrl_done      : std_logic;
    signal ctrl_busy      : std_logic;
    signal ctrl_addr_col  : std_logic_vector(15 downto 0) := (others => '0');
    signal ctrl_addr_row  : std_logic_vector(23 downto 0) := (others => '0');
    signal ctrl_rd_cnt    : unsigned(15 downto 0) := (others => '0');
    signal ctrl_rd_data   : std_logic_vector(7 downto 0);
    signal ctrl_rd_valid  : std_logic;
    signal ctrl_id        : std_logic_vector(39 downto 0);
    signal ctrl_status    : std_logic_vector(7 downto 0);

    -- Page buffer write pointer (byte index)
    signal buf_wr_idx     : unsigned(15 downto 0) := (others => '0');

    -- Status register
    signal reg_busy       : std_logic := '0';
    signal reg_done       : std_logic := '0';

    -- AXI handshake
    signal axi_awready_r  : std_logic := '0';
    signal axi_wready_r   : std_logic := '0';
    signal axi_bvalid_r   : std_logic := '0';
    signal axi_arready_r  : std_logic := '0';
    signal axi_rvalid_r   : std_logic := '0';
    signal axi_rdata_r    : std_logic_vector(31 downto 0) := (others => '0');
    signal aw_latched     : std_logic_vector(C_S_AXI_ADDR_WIDTH-1 downto 0)
                            := (others => '0');
    signal ar_latched     : std_logic_vector(C_S_AXI_ADDR_WIDTH-1 downto 0)
                            := (others => '0');

    -- Internal reset
    signal rst : std_logic;

begin

    rst <= not s_axi_aresetn;

    ---------------------------------------------------------------------------
    -- NAND controller instantiation
    ---------------------------------------------------------------------------
    u_nand_ctrl : entity work.nand_flash_ctrl
        port map (
            clk         => s_axi_aclk,
            rst         => rst,
            cmd_start   => ctrl_start,
            cmd_op      => ctrl_op,
            cmd_done    => ctrl_done,
            cmd_busy    => ctrl_busy,
            addr_col    => ctrl_addr_col,
            addr_row    => ctrl_addr_row,
            rd_byte_cnt => ctrl_rd_cnt,
            rd_data     => ctrl_rd_data,
            rd_valid    => ctrl_rd_valid,
            id_data     => ctrl_id,
            status_data => ctrl_status,
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
    -- Page buffer write logic: capture bytes from NAND into BRAM
    -- Bytes are packed into 32-bit words, little-endian.
    ---------------------------------------------------------------------------
    process(s_axi_aclk)
        variable word_idx : integer;
        variable byte_pos : integer;  -- 0..3 within word
        variable cur_word : std_logic_vector(31 downto 0);
    begin
        if rising_edge(s_axi_aclk) then
            if rst = '1' then
                buf_wr_idx <= (others => '0');
            else
                -- Reset write pointer on new operation start
                if ctrl_start = '1' then
                    buf_wr_idx <= (others => '0');
                end if;

                -- Store incoming bytes
                if ctrl_rd_valid = '1' then
                    word_idx := to_integer(buf_wr_idx(15 downto 2));
                    byte_pos := to_integer(buf_wr_idx(1 downto 0));

                    if word_idx < BUF_WORDS then
                        cur_word := page_buf(word_idx);
                        case byte_pos is
                            when 0 => cur_word( 7 downto  0) := ctrl_rd_data;
                            when 1 => cur_word(15 downto  8) := ctrl_rd_data;
                            when 2 => cur_word(23 downto 16) := ctrl_rd_data;
                            when 3 => cur_word(31 downto 24) := ctrl_rd_data;
                            when others => null;
                        end case;
                        page_buf(word_idx) <= cur_word;
                    end if;

                    buf_wr_idx <= buf_wr_idx + 1;
                end if;
            end if;
        end if;
    end process;

    ---------------------------------------------------------------------------
    -- Status tracking
    ---------------------------------------------------------------------------
    process(s_axi_aclk)
    begin
        if rising_edge(s_axi_aclk) then
            if rst = '1' then
                reg_busy <= '0';
                reg_done <= '0';
            else
                reg_busy <= ctrl_busy;
                if ctrl_done = '1' then
                    reg_done <= '1';
                end if;
                -- W1C: clear done when CTRL[4] written (handled in write logic)
            end if;
        end if;
    end process;

    ---------------------------------------------------------------------------
    -- AXI4-Lite write channel
    ---------------------------------------------------------------------------
    s_axi_awready <= axi_awready_r;
    s_axi_wready  <= axi_wready_r;
    s_axi_bvalid  <= axi_bvalid_r;
    s_axi_bresp   <= "00";  -- OKAY

    process(s_axi_aclk)
    begin
        if rising_edge(s_axi_aclk) then
            if rst = '1' then
                axi_awready_r <= '0';
                axi_wready_r  <= '0';
                axi_bvalid_r  <= '0';
                ctrl_start    <= '0';
            else
                ctrl_start <= '0';  -- one-cycle pulse

                -- Accept write address
                if s_axi_awvalid = '1' and axi_awready_r = '0'
                   and s_axi_wvalid = '1' then
                    axi_awready_r <= '1';
                    aw_latched    <= s_axi_awaddr;
                else
                    axi_awready_r <= '0';
                end if;

                -- Accept write data (same cycle as address for simplicity)
                if s_axi_wvalid = '1' and axi_wready_r = '0'
                   and s_axi_awvalid = '1' then
                    axi_wready_r <= '1';
                else
                    axi_wready_r <= '0';
                end if;

                -- Process the write
                if axi_awready_r = '1' and axi_wready_r = '1' then
                    axi_bvalid_r <= '1';

                    case to_integer(unsigned(aw_latched(5 downto 0))) is
                        when 16#00# =>  -- CTRL register
                            if s_axi_wdata(0) = '1' and ctrl_busy = '0' then
                                ctrl_op    <= op_decode(s_axi_wdata(3 downto 1));
                                ctrl_start <= '1';
                            end if;
                            if s_axi_wdata(4) = '1' then
                                reg_done <= '0';  -- W1C done flag
                            end if;

                        when 16#08# =>  -- ADDR_COL
                            ctrl_addr_col <= s_axi_wdata(15 downto 0);

                        when 16#0C# =>  -- ADDR_ROW
                            ctrl_addr_row <= s_axi_wdata(23 downto 0);

                        when 16#10# =>  -- RD_COUNT
                            ctrl_rd_cnt <= unsigned(s_axi_wdata(15 downto 0));

                        when others =>
                            null;  -- ignore writes to read-only / reserved
                    end case;
                end if;

                -- Write response handshake
                if axi_bvalid_r = '1' and s_axi_bready = '1' then
                    axi_bvalid_r <= '0';
                end if;
            end if;
        end if;
    end process;

    ---------------------------------------------------------------------------
    -- AXI4-Lite read channel
    ---------------------------------------------------------------------------
    s_axi_arready <= axi_arready_r;
    s_axi_rvalid  <= axi_rvalid_r;
    s_axi_rdata   <= axi_rdata_r;
    s_axi_rresp   <= "00";  -- OKAY

    process(s_axi_aclk)
        variable buf_word_addr : integer;
    begin
        if rising_edge(s_axi_aclk) then
            if rst = '1' then
                axi_arready_r <= '0';
                axi_rvalid_r  <= '0';
                axi_rdata_r   <= (others => '0');
            else
                -- Accept read address
                if s_axi_arvalid = '1' and axi_arready_r = '0' then
                    axi_arready_r <= '1';
                    ar_latched    <= s_axi_araddr;
                else
                    axi_arready_r <= '0';
                end if;

                -- Provide read data one cycle after accepting address
                if axi_arready_r = '1' then
                    axi_rvalid_r <= '1';

                    if unsigned(ar_latched) >= x"4000" then
                        -- Page buffer read
                        buf_word_addr := to_integer(
                            unsigned(ar_latched(14 downto 2)) - 16#1000#);
                        if buf_word_addr >= 0 and buf_word_addr < BUF_WORDS then
                            axi_rdata_r <= page_buf(buf_word_addr);
                        else
                            axi_rdata_r <= x"DEADBEEF";
                        end if;
                    else
                        -- Control/status register read
                        case to_integer(unsigned(ar_latched(5 downto 0))) is
                            when 16#00# =>  -- CTRL (read back op type)
                                axi_rdata_r <= (others => '0');
                                axi_rdata_r(3 downto 1) <=
                                    op_encode(ctrl_op);

                            when 16#04# =>  -- STATUS
                                axi_rdata_r <= (others => '0');
                                axi_rdata_r(0) <= reg_busy;
                                axi_rdata_r(1) <= reg_done;
                                axi_rdata_r(2) <= nand_rb_n;

                            when 16#08# =>  -- ADDR_COL
                                axi_rdata_r <= x"0000" & ctrl_addr_col;

                            when 16#0C# =>  -- ADDR_ROW
                                axi_rdata_r <= x"00" & ctrl_addr_row;

                            when 16#10# =>  -- RD_COUNT
                                axi_rdata_r <= x"0000" &
                                    std_logic_vector(ctrl_rd_cnt);

                            when 16#14# =>  -- ID_LO
                                axi_rdata_r <= ctrl_id(31 downto 0);

                            when 16#18# =>  -- ID_HI
                                axi_rdata_r <= x"000000" &
                                    ctrl_id(39 downto 32);

                            when 16#1C# =>  -- NAND_STATUS
                                axi_rdata_r <= x"000000" & ctrl_status;

                            when 16#20# =>  -- PAGE_IDX
                                axi_rdata_r <= x"0000" &
                                    std_logic_vector(buf_wr_idx);

                            when 16#24# =>  -- VERSION
                                axi_rdata_r <= x"4E414E44";  -- "NAND"

                            when others =>
                                axi_rdata_r <= (others => '0');
                        end case;
                    end if;
                end if;

                -- Read response handshake
                if axi_rvalid_r = '1' and s_axi_rready = '1' then
                    axi_rvalid_r <= '0';
                end if;
            end if;
        end if;
    end process;

end architecture rtl;
