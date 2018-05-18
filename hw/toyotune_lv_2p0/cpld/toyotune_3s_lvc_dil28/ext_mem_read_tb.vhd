--------------------------------------------------------------------------------
-- Company: 
-- Engineer:
--
-- Create Date:   13:54:45 01/12/2011
-- Design Name:   
-- Module Name:   H:/toyotune/toyotune-hw/toyotune_lv_1p0/cpld/mem_read_tb.vhd
-- Project Name:  io_expander
-- Target Device:  
-- Tool versions:  
-- Description:   
-- 
-- VHDL Test Bench Created by ISE for module: io_expander
-- 
-- Dependencies:
-- 
-- Revision:
-- Revision 0.01 - File Created
-- Additional Comments:
--
-- Notes: 
-- This testbench has been automatically generated using types std_logic and
-- std_logic_vector for the ports of the unit under test.  Xilinx recommends
-- that these types always be used for the top-level I/O of a design in order
-- to guarantee that the testbench will bind correctly to the post-implementation 
-- simulation model.
--------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.std_logic_unsigned.all;
use ieee.numeric_std.all;
 
entity ext_mem_read_tb is
end ext_mem_read_tb;
 
architecture behavior of ext_mem_read_tb is 
 
   -- Component Declaration for the Unit Under Test (UUT) 
   component io_expander
   port
	(
		d_a 			: in std_logic_vector(15 downto 8);
      d_da 			: inout std_logic_vector(7 downto 0);
      c_pa 			: inout std_logic_vector(7 downto 0);
      c_pb 			: inout std_logic_vector(7 downto 0);
      c_dra 		: out std_logic_vector(7 downto 0);
      c_drb 		: out std_logic_vector(7 downto 0);
      d_al 			: out std_logic_vector(7 downto 0);
      m_a 			: out std_logic_vector(17 downto 14);
      m_ram_n_cs 	: out std_logic;
      m_rom_n_cs 	: out std_logic;
		m_n_rd		: out std_logic;
		m_n_wr		: out std_logic;
      d_n_rd 		: in  std_logic;
      d_n_wr 		: in  std_logic;
      d_adr 		: in  std_logic;
		d_cclk 		: in std_logic;
      d_n_irp 		: in  std_logic;
      d_n_init		: out  std_logic;
      e_n_is 		: in  std_logic;
      e_n_os 		: out  std_logic;
      e_n_init		: in  std_logic;
			
		x_n_init		: in std_logic;
		x_n_bus_en	: in std_logic;
		x_adr			: inout std_logic;
		x_n_rd		: inout std_logic;
		x_n_wr		: inout std_logic
   );
   end component;
    
	component rom
	port
	(
		ce_n	: in std_logic;
		oe_n	: in std_logic;
		a		: in std_logic_vector(13 downto 0);
		d		: out std_logic_vector(7 downto 0)
	);
	end component;
	
   --Inputs
   signal d_a 			: std_logic_vector(15 downto 8) := (others => '0');
   signal d_n_rd 		: std_logic := '1';
   signal d_n_wr 		: std_logic := '1';
   signal d_adr 		: std_logic := '0';
   signal d_n_irp		: std_logic := '1';
   signal d_cclk		: std_logic := '1';
   signal e_n_is		: std_logic := '1';
   signal e_n_init	: std_logic := '1';
	signal x_n_init	: std_logic := '1';
	signal x_n_bus_en	: std_logic := '1';

	--BiDirs
   signal d_da 		: std_logic_vector(7 downto 0) := (others => 'Z');
   signal c_pa 		: std_logic_vector(7 downto 0);
   signal c_pb 		: std_logic_vector(7 downto 0);
	signal x_adr		: std_logic := 'Z';
	signal x_n_rd		: std_logic := 'Z';
	signal x_n_wr		: std_logic := 'Z';

 	--Outputs
   signal c_dra 		: std_logic_vector(7 downto 0);
   signal c_drb 		: std_logic_vector(7 downto 0);
   signal d_al 		: std_logic_vector(7 downto 0);
   signal m_a 			: std_logic_vector(17 downto 14);
   signal m_ram_n_cs	: std_logic;
   signal m_rom_n_cs	: std_logic;
   signal m_n_rd 		: std_logic;
   signal m_n_wr 		: std_logic;
   signal d_n_init	: std_logic;
   signal e_n_os 		: std_logic;
	signal rom_a		: std_logic_vector(13 downto 0);
    
begin 
	-- Instantiate the Unit Under Test (UUT)
   uut : io_expander port map
	(
		d_a 			=> d_a,
      d_da 			=> d_da,
      c_pa 			=> c_pa,
      c_pb 			=> c_pb,
      c_dra 		=> c_dra,
      c_drb 		=> c_drb,
      d_al 			=> d_al,
      m_a 			=> m_a,
      m_ram_n_cs 	=> m_ram_n_cs,
      m_rom_n_cs 	=> m_rom_n_cs,
		m_n_rd		=> m_n_rd,
		m_n_wr		=> m_n_wr,
      d_n_rd 		=> d_n_rd,
      d_n_wr 		=> d_n_wr,
      d_adr 		=> d_adr,
      d_cclk		=> d_cclk,
      d_n_irp		=> d_n_irp,
      d_n_init		=> d_n_init,
      e_n_is		=> e_n_is,
      e_n_os 		=> e_n_os,
      e_n_init 	=> e_n_init,
		x_n_init		=> x_n_init,
		x_n_bus_en	=> x_n_bus_en,
		x_adr			=> x_adr,
		x_n_rd		=> x_n_rd,
		x_n_wr		=> x_n_wr
	);
 
	rom_t : rom port map
	(
		ce_n	=> m_rom_n_cs,
		oe_n	=> m_n_rd,
		a		=> rom_a,
		d		=> d_da
	);
	
	-- ROM address
	rom_a <= d_a(13 downto 8) & d_al(7 downto 0);
	
   -- No clocks detected in port list. Replace <clock> below with 
   -- appropriate port name 
 
   --constant <clock>_period := 1ns;
 
   --<clock>_process :process
   --begin
	--	<clock> <= '0';
	--	wait for <clock>_period/2;
	--	<clock> <= '1';
	--	wait for <clock>_period/2;
   --end process;
 
   -- Stimulus process
   stim_proc : process
	
	procedure read_byte (address : in std_logic_vector(15 downto 0)) is
   begin
		d_adr <= '1';							-- ADR high
		wait for 75ns;			
		d_da <= address(7 downto 0);		-- Set DA7..DA0 to address(7 downto 0)
		d_a <= address(15 downto 8); 		-- Set A15..A8 to address(15 downto 8)
		wait for 50ns;
		d_adr <= '0';							-- ADR low
		wait for 60ns;
		d_n_rd <= '0';							-- !RD low
		wait for 30ns;
		d_da <= "ZZZZZZZZ";					-- Tristate DA7..DA0
		wait for 160ns;
		d_n_rd <= '1';							-- !RD high
   end procedure read_byte;

	procedure read_byte_x (address : in std_logic_vector(15 downto 0)) is
   begin
		x_adr <= '1';							-- ADR high
		wait for 75ns;			
		d_da <= address(7 downto 0);		-- Set DA7..DA0 to address(7 downto 0)
		d_a <= address(15 downto 8); 		-- Set A15..A8 to address(15 downto 8)
		wait for 50ns;
		x_adr <= '0';							-- ADR low
		wait for 60ns;
		x_n_rd <= '0';							-- !RD low
		wait for 30ns;
		d_da <= "ZZZZZZZZ";					-- Tristate DA7..DA0
		wait for 160ns;
		x_n_rd <= '1';							-- !RD high
   end procedure read_byte_x;

   begin		
		-- enable external bus
		x_n_bus_en <= '0';
		
		-- de-assert reset
		x_n_init <= '1';
	
      -- hold reset state for 50ns.
		e_n_init <= '0';
      wait for 50ns;	
		e_n_init <= '1';		

		-- read reset vector from external memory
		read_byte ("1111111111111110");
		read_byte ("1111111111111111");
		
		-- read internal RAM
		read_byte ("0000000001000000");

		-- disable external bus
		x_n_bus_en <= '1';
		
		x_adr <= '0';
		x_n_rd <= '1';
		x_n_wr <= '1';

		-- assert reset
		x_n_init <= '0';
		

		-- read reset vector
		read_byte_x ("1111111111111110");
		read_byte_x ("1111111111111111");
		
      wait;
   end process;

	assertion_proc : process
	begin
		wait for 50ns;
		wait for 25ns;
		
		wait for 375ns;

		wait;
	end process;
		
END;
