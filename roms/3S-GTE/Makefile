ROMS = D151803-9651 D151803-9661 D151804-0461 D151804-0471 D151804-0481 Jon_ST205_ECU
ROMS_TOYOTUNE = $(ROMS:%=rom_toyotune-%)
ROMS_TECHTOM = $(ROMS:%=rom_techtom-%)
ROMS_CLEAN = $(ROMS:%=clean-%)

rom : $(ROMS)

$(ROMS) ::
	@echo Building $@
	@$(MAKE) -s -C $@

rom_toyotune: $(ROMS_TOYOTUNE)

$(ROMS_TOYOTUNE) :
	@echo Building $(@:rom_toyotune-%=%) for Toyotune
	@$(MAKE) -s -C $(@:rom_toyotune-%=%) rom_toyotune

rom_techtom: $(ROMS_TECHTOM)

$(ROMS_TECHTOM) :
	@echo Building $(@:rom_techtom-%=%) for Techtom
	@$(MAKE) -s -C $(@:rom_techtom-%=%) rom_techtom
	
clean: $(ROMS_CLEAN)
	
$(ROMS_CLEAN):
	@echo Cleaning $(@:clean-%=%)
	@$(MAKE) -s -C $(@:clean-%=%) clean
	
	