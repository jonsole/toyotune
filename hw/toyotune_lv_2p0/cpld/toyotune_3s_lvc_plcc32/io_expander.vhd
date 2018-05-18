-- Denso 8X MCU IO Expander 
-- Copyright (C) 2009, Jon Sole
--
-- This program is free software: you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation, either version 3 of the License, or
-- (at your option) any later version.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program.  If not, see <http://www.gnu.org/licenses/>.

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.STD_LOGIC_ARITH.ALL;
use IEEE.STD_LOGIC_UNSIGNED.ALL;

entity io_expander is
	port
	(
	   -- Multiplexed address and data bus from Denso MCU
		d_a			: in	  std_logic_vector(15 downto 8);
		d_da	  		: inout std_logic_vector(7 downto 0);

		-- PORTA & PORTB
		c_pa        : inout std_logic_vector(7 downto 0);
		c_pb        : inout std_logic_vector(7 downto 0);
		
		-- DDRA & DDRB
		c_dra			: out std_logic_vector(7 downto 0);
		c_drb			: out std_logic_vector(7 downto 0);
		
		-- Lower memory address bus
		d_al			: out std_logic_vector(7 downto 0);
		
		-- Upper memory address bus
		m_a			: out std_logic_vector(17 downto 15);
		
		-- Memory selection
		msel			: in std_logic_vector(2 downto 0);

		-- Flash !CS, !RD and !WR signals
		m_rom_n_cs	: out	std_logic;
		m_ram_n_cs	: out	std_logic;
		m_n_rd		: out std_logic;
		m_n_wr		: out std_logic;
		
		-- Denso MCU !RD, !WR, ADR and !IRP signals
		d_n_rd    	: in std_logic;
		d_n_wr    	: in std_logic;
		d_adr			: in std_logic;
		d_n_irp 		: in std_logic;

		-- Denso 1MHz clock
		d_cclk 		: in std_logic;

		-- Denso !INIT signal
		d_n_init		: out std_logic;
		
		-- ECU !IS, !OS signals
		e_n_is		: in std_logic;
		e_n_os		: out std_logic;

		-- ECU !INIT signal
		e_n_init 	: in std_logic
	);	
end io_expander;

architecture io_expander_arch of io_expander is

	constant version : integer := 4;
	
	-- Internal registers
	signal reg_ddra			: std_logic_vector(7 downto 0);
	signal reg_porta			: std_logic_vector(7 downto 0);
	signal reg_portal			: std_logic_vector(7 downto 4);
	signal reg_portal_buf	: std_logic_vector(7 downto 4);
	signal reg_ddrb			: std_logic_vector(7 downto 0);
	signal reg_portb  		: std_logic_vector(7 downto 0);
	signal reg_memc_page		: std_logic_vector(2 downto 0) := (others => '0');
	signal reg_memc_sram_rw	: std_logic := '0';
	signal reg_memc_sram_wt : std_logic := '0';
	
	-- Internal register enables
	signal reg_en				: std_logic;
	signal reg_ddra_en   	: std_logic;
	signal reg_ddrb_en   	: std_logic;
	signal reg_porta_en  	: std_logic;
	signal reg_portal_en 	: std_logic;
	signal reg_portb_en  	: std_logic;
	signal reg_pbcs_en   	: std_logic;
	signal reg_memc_en		: std_logic;
	signal reg_ver_en			: std_logic;
	
	-- Denso MCU low address latch
	signal d_a_latch 			: std_logic_vector(7 downto 0);
	
	-- Internal !WR, !RD & ADR signals, generated from a combination of Denso MCU
	-- and External Bus signals
	signal n_wr 				: std_logic;
	signal n_rd 				: std_logic;
	signal adr  				: std_logic;
	
	-- Internal memory chip select signal for memory accesses in the range 0x8000..0xFFFF
	signal m_n_cs			: std_logic;
	
begin
	-- Internal register chip select
	reg_en <= '1' when d_a(15 downto 8) & d_a_latch(7 downto 6) = "0000000000" else '0';

	-- Internal nRD & nWR from External Bus and Denso MCU nRD & nWR signals
	-- Note: Denso MCU signals held high when MCU is held in reset
	n_rd <= d_n_rd;
	n_wr <= d_n_wr;
	
	-- Generate internal ADR signal from External Bus and Denso MCU ADR signals
	-- Note: Denso MCU ADR signal is active even when MCU is held in reset
	adr <= d_adr;

	-- Output data direction registers
	c_dra <= reg_ddra;
	c_drb <= reg_ddrb;

	-- High memory select when address is in the range 0x8000..0xFFFF
	m_n_cs <= (n_rd and n_wr) when d_a(15) = '1' else '1';
	
	-- Select ROM when MEMC_SRAM_WR = 0
	--														n_wr	not n_wr		reg_memc_sram_rt
	--														  0		1				0		=  0
	--														  1		0				0		=	0
	--														  0		1				1		=	1 
	--														  1		0				1		=	0
	m_rom_n_cs <= m_n_cs or (reg_memc_sram_rw or (not n_wr and reg_memc_sram_wt));
	
	-- Select RAM when MEMC_SRAM_WR = 1 or when MEMC_SRAM_WT = 1 and !WR = 0
	--																n_wr	not reg_memc_sram_rt
	--																  0			1		=  1
	--																  1			1		=	1
	--																  0			0		=	0 
	--																  1			0		=	1	
	m_ram_n_cs <= m_n_cs or ((not reg_memc_sram_rw) and (n_wr or not reg_memc_sram_wt));

	-- Multiplexed nRD & nWR signals
	m_n_rd <= n_rd;
	m_n_wr <= n_wr;
	
	-- Output high memory address (a17:a15) from MEMC register
	m_a <= reg_memc_page;
	
	-- Output latched low memory address (a7:a0)
	d_al <= d_a_latch;
	
	-- Denso MCU !INIT from ECU !INIT
	d_n_init <= e_n_init;

	-- ECU !OS signal follows Denso MCU !WR signal when accessing PORTB register
	e_n_os <= d_n_wr when reg_portb_en = '1' else '1';
	
-- Buffer low byte of address bus on falling edge of ADRs
p_denso_addr_buffer : process(e_n_init, adr)
begin
	if adr'event and adr = '0' then
		d_a_latch <= d_da;
	end if;
end process;

-- Register enables
p_reg : process(reg_en, d_a_latch)
begin
	case reg_en & d_a_latch(5 downto 0) is
		when "1000000" => -- DDRA
			reg_ddra_en   <= '1';
			reg_ddrb_en   <= '0';
			reg_porta_en  <= '0';
			reg_portal_en <= '0';
			reg_portb_en  <= '0';
			reg_pbcs_en   <= '0';
			reg_memc_en   <= '0';
			reg_ver_en    <= '0';

		when "1000001" => -- DDRB
			reg_ddra_en   <= '0';
			reg_ddrb_en   <= '1';
			reg_porta_en  <= '0';
			reg_portal_en <= '0';
			reg_portb_en  <= '0';
			reg_pbcs_en   <= '0';
			reg_memc_en   <= '0';
			reg_ver_en    <= '0';
			
		when "1100000" => -- PORTA
			reg_ddra_en   <= '0';
			reg_ddrb_en   <= '0';
			reg_porta_en  <= '1';
			reg_portal_en <= '0';
			reg_portb_en  <= '0';
			reg_pbcs_en   <= '0';
			reg_memc_en   <= '0';
			reg_ver_en    <= '0';
			
		when "1100001" => -- PORTAL
			reg_ddra_en   <= '0';
			reg_ddrb_en   <= '0';
			reg_porta_en  <= '0';
			reg_portal_en <= '1';
			reg_portb_en  <= '0';
			reg_pbcs_en   <= '0';
			reg_memc_en   <= '0';
			reg_ver_en    <= '0';
		
		when "1100010" => -- PORTB
			reg_ddra_en   <= '0';
			reg_ddrb_en   <= '0';
			reg_porta_en  <= '0';
			reg_portal_en <= '0';
			reg_portb_en  <= '1';
			reg_pbcs_en   <= '0';
			reg_memc_en   <= '0';
			reg_ver_en    <= '0';
			
		when "1100011" => -- PBCS
			reg_ddra_en   <= '0';
			reg_ddrb_en   <= '0';
			reg_porta_en  <= '0';
			reg_portal_en <= '0';
			reg_portb_en  <= '0';
			reg_pbcs_en   <= '1';
			reg_memc_en   <= '0';
			reg_ver_en    <= '0';

		when "1011110" => -- MEMC
			reg_ddra_en   <= '0';
			reg_ddrb_en   <= '0';
			reg_porta_en  <= '0';
			reg_portal_en <= '0';
			reg_portb_en  <= '0';
			reg_pbcs_en   <= '0';
			reg_memc_en   <= '1';
			reg_ver_en    <= '0';

		when "1011111" => -- VER
			reg_ddra_en   <= '0';
			reg_ddrb_en   <= '0';
			reg_porta_en  <= '0';
			reg_portal_en <= '0';
			reg_portb_en  <= '0';
			reg_pbcs_en   <= '0';
			reg_memc_en   <= '0';
			reg_ver_en    <= '1';
			
		when others =>
			reg_ddra_en   <= '0';
			reg_ddrb_en   <= '0';
			reg_porta_en  <= '0';
			reg_portal_en <= '0';
			reg_portb_en  <= '0';
			reg_pbcs_en   <= '0';
			reg_memc_en   <= '0';
			reg_ver_en    <= '0';
	end case;
end process;

-- Register read
p_read : process(reg_ddra, reg_ddrb_en, reg_porta_en, reg_portal_en, reg_portb_en, reg_memc_en, reg_pbcs_en, reg_ver_en,
					            reg_ddrb,    reg_porta,	   reg_portal,    reg_portb,
									reg_memc_page, reg_memc_sram_rw, reg_memc_sram_wt,
					  n_rd, d_n_irp,
					  c_pa, c_pb,
					  e_n_is, e_n_init)
begin
	if n_rd = '0' then
		if reg_ddrb_en = '1'then
			d_da <= reg_ddrb;
		elsif reg_porta_en = '1' then
			d_da <= (reg_porta and reg_ddra) or (c_pa and (not reg_ddra));
		elsif reg_portal_en = '1' then
			d_da <= reg_portal & "1111";
		elsif reg_portb_en = '1' then
			d_da <= (reg_portb and reg_ddrb) or (c_pb and (not reg_ddrb));
		elsif reg_pbcs_en = '1' then
			d_da <= "11" & d_n_irp & e_n_is & "0000";
		elsif reg_memc_en = '1' then
			d_da <= e_n_init & "0" & reg_memc_sram_wt & reg_memc_sram_rw & "0" & reg_memc_page;
		elsif reg_ver_en = '1' then
			d_da <= conv_std_logic_vector(version, 8);
		else
			d_da <= (others => 'Z');
		end if;
	else
		d_da <= (others => 'Z');
	end if;
end process;

-- Register DDRA write
p_reg_ddra : process(e_n_init, n_wr, reg_ddra_en)						 
begin
	if e_n_init = '0' then
		reg_ddra <= (others => '0');
	elsif n_wr'event and n_wr = '1' then
		if reg_ddra_en = '1' then
			reg_ddra <= d_da;
		end if;
	end if;
end process;

-- Register DDRB write
p_reg_ddrb : process(e_n_init, n_wr, reg_ddrb_en)						 
begin
	if e_n_init = '0' then
		reg_ddrb <= (others => '0');
	elsif n_wr'event and n_wr = '1' then
		if reg_ddrb_en = '1' then
			reg_ddrb <= d_da;
		end if;
	end if;
end process;

-- Register PORTA write
p_reg_porta : process(e_n_init, n_wr, reg_porta_en)						 
begin
	if e_n_init = '0' then
		reg_porta <= (others => '0');
	elsif n_wr'event and n_wr = '1' then
		if reg_porta_en = '1' then
			reg_porta <= d_da;
		end if;
	end if;
end process;

-- Register PORTAL write
gen_reg_portal_45 : for i in 4 to 5 generate
	-- PORTAL(4..5), latch PA(4..5) on rising edge
	p_reg_portal_45 : process(e_n_init, d_cclk, n_wr, d_da, reg_portal_en, c_pa(i))
	begin
		if e_n_init = '0' then
			reg_portal(i) <= '0';
		elsif reg_portal_en = '1' and n_wr = '0' then
			reg_portal(i) <= d_da(i);
		elsif d_cclk'event and d_cclk	= '0' then
         reg_portal(i) <= reg_portal(i) or (c_pa(i) and (not reg_portal_buf(i)));
         reg_portal_buf(i) <= c_pa(i);
		end if;
	end process;
end generate;

gen_reg_portal_67 : for i in 6 to 7 generate
	-- PORTAL(6..7), latch PA(6..7) on falling edge
	p_reg_portal_67 : process(e_n_init, d_cclk, n_wr, d_da, reg_portal_en, c_pa(i))
	begin
		if e_n_init = '0' then
			reg_portal(i) <= '0';
		elsif reg_portal_en = '1' and n_wr = '0' then
			reg_portal(i) <= d_da(i);
		elsif d_cclk'event and d_cclk = '0' then
         reg_portal(i) <= reg_portal(i) or ((not c_pa(i)) and reg_portal_buf(i));
         reg_portal_buf(i) <= c_pa(i);
		end if;
	end process;
end generate;

-- Register PORTB write
p_reg_portb : process(e_n_init, n_wr, reg_portb_en)						 
begin
	if e_n_init = '0' then
		reg_portb <= (others => '0');
	elsif n_wr'event and n_wr = '1' then
		if reg_portb_en = '1' then
			reg_portb <= d_da;
		end if;
	end if;
end process;

-- Register MEMC write
p_reg_memc : process(e_n_init, n_wr, reg_memc_en, msel)						 
begin
	if e_n_init = '0' then
		reg_memc_page <= not msel(2 downto 0);
	elsif n_wr'event and n_wr = '1' then
		if reg_memc_en = '1' then
			reg_memc_page <= d_da(2 downto 0);
			reg_memc_sram_rw <= d_da(4);
			reg_memc_sram_wt <= d_da(5);
		end if;
	end if;
end process;

-- Drive PA0..7
p_pa : process(reg_ddra, reg_porta)
	variable i : integer;
begin
	for i in 0 to 7 loop
		if reg_ddra(i) = '1' then
			c_pa(i) <= reg_porta(i);
		else
			c_pa(i) <= 'Z';
		end if;
	end loop;
end process;

-- Drive PB0..7
p_pb : process(reg_ddrb, reg_portb)
	variable i : integer;
begin
	for i in 0 to 7 loop
		if reg_ddrb(i) = '1' then
			c_pb(i) <= reg_portb(i);
		else
			c_pb(i) <= 'Z';
		end if;
	end loop;
end process;

end io_expander_arch;

