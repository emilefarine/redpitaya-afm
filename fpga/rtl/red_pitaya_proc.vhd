library IEEE;
use IEEE.STD_LOGIC_1164.all;
use IEEE.NUMERIC_STD.all;

entity red_pitaya_proc is
generic(
    DW  : integer := 8                                                      -- Digital width (number of GPIO pins)
);
port (
    clk_i                   : in  std_logic;                                -- bus clock
    rstn_i                  : in  std_logic;                                -- bus reset - active low
    dat_a_i, dat_b_i        : in  std_logic_vector(13 downto 0);            -- input
    dat_a_o, dat_b_o        : out std_logic_vector(13 downto 0);            -- output

    led_o                   : out std_logic_vector(7 downto 0);             -- LED output
    gpio_p_i, gpio_n_i      : in  std_logic_vector(DW-1 downto 0);          -- GPIO input data
    gpio_p_o, gpio_n_o      : out std_logic_vector(DW-1 downto 0);          -- GPIO output data
    gpio_p_dir, gpio_n_dir  : out std_logic_vector(DW-1 downto 0);          -- GPIO direction

    sys_addr                : in  std_logic_vector(31 downto 0);            -- bus address
    sys_wdata               : in  std_logic_vector(31 downto 0);            -- bus write data
    sys_wen                 : in  std_logic;                                -- bus write enable
    sys_ren                 : in  std_logic;                                -- bus read enable
    sys_rdata               : out std_logic_vector(31 downto 0);            -- bus read data
    sys_err                 : out std_logic;                                -- bus error indicator
    sys_ack                 : out std_logic                                 -- bus acknowledge signal
);
end red_pitaya_proc;

architecture Behavioral of red_pitaya_proc is

    constant ZERO               : std_logic_vector(32-1 downto 0) := (others => '0');

    signal diop_in, dion_in     : std_logic_vector(DW-1 downto 0);
    signal diop_out, dion_out   : std_logic_vector(DW-1 downto 0) := (others => '0');  -- output 0
    signal diop_dir, dion_dir   : std_logic_vector(DW-1 downto 0) := (others => '0');  -- direction in=0, out=1

    signal led : std_logic_vector(7 downto 0) := (others => '0');


    signal a, b: std_logic_vector(7 downto 0); -- amplitude registers
    signal mul_a, mul_b: signed(22 downto 0);

begin

    -- multiply signed inputs with 8-bit register, register values are unsigned
    mul_a <= signed(dat_a_i) * signed('0' & a);

    -- divide by 16 (multiplication format 4.4), possible output overflow
    dat_a_o <= std_logic_vector(mul_a(17 downto 4));

    pbus: process(clk_i)
    begin
        if rising_edge(clk_i) then
            if rstn_i = '0' then
                diop_dir <= (others => '0');
                dion_dir <= (others => '0');
                diop_out <= (others => '0');
                dion_out <= (others => '0');
                led <= (others => '0');

                a <= x"10";
            else
                sys_ack <= sys_wen or sys_ren;    -- acknowledge transactions

                if sys_wen='1' then                                 -- decode address & write registers
                    if sys_addr(19 downto 0)=X"00010" then
                        diop_dir <= sys_wdata(DW-1 downto 0);       -- Change direction P
                    elsif sys_addr(19 downto 0)=X"00014" then
                        dion_dir <= sys_wdata(DW-1 downto 0);       -- Change direction N
                    elsif sys_addr(19 downto 0)=X"00018" then
                        diop_out <= sys_wdata(DW-1 downto 0);       -- Change output P
                    elsif sys_addr(19 downto 0)=X"0001C" then
                        dion_out <= sys_wdata(DW-1 downto 0);       -- Change output N
                    elsif sys_addr(19 downto 0)=X"00030" then
                        led <= sys_wdata(7 downto 0);               -- Change LEDs
                    elsif sys_addr(19 downto 0)=X"00054" then
                        a <= sys_wdata(7 downto 0);                 -- 8-bit amplitude
                    end if;
            end if;
            end if;
        end if;
    end process pbus;

    -- Handling errors
    sys_err <= '0';

    -- Direct connections
    gpio_p_dir <= diop_dir;
    gpio_n_dir <= dion_dir;
    gpio_p_o <= diop_out;
    gpio_n_o <= dion_out;
    diop_in <= gpio_p_i;
    dion_in <= gpio_n_i;
    led_o <= led;

    -- Decode address & read data
    with sys_addr(19 downto 0) select
        sys_rdata <= X"FEEDBACC"                        when x"00050",      -- ID
                    ZERO(32-1 downto DW) & diop_dir     when x"00010",      -- GPIO P direction
                    ZERO(32-1 downto DW) & dion_dir     when x"00014",      -- GPIO N direction
                    ZERO(32-1 downto DW) & diop_out     when x"00018",      -- GPIO P output
                    ZERO(32-1 downto DW) & diop_out     when x"0001C",      -- GPIO N output
                    ZERO(32-1 downto DW) & diop_in      when x"00020",      -- GPIO P inputs
                    ZERO(32-1 downto DW) & dion_in      when x"00024",      -- GPIO N inputs
                    ZERO(32-1 downto 8) & led           when x"00030",      -- LEDs
                    ZERO(32-1 downto 8) & a             when x"00054",      -- Amplitude
                    ZERO when others;

end Behavioral;