ROMS = D151803-9651 D151803-9661 D151804-0461 D151804-0471
ROMS_TOYOTUNE = $(ROMS:%=rom_toyotune-%)
ROMS_TECHTOM = $(ROMS:%=rom_techtom-%)
ROMS_CLEAN = $(ROMS:%=clean-%)

rom : $(ROMS)

$(ROMS) :
	$(MAKE) -C $@ rom

rom_toyotune: $(ROMS_TOYOTUNE)

$(ROMS_TOYOTUNE) :
	$(MAKE) -C $(@:rom_toyotune-%=%) rom_toyotune

rom_techtom: $(ROMS_TECHTOM)

$(ROMS_TECHTOM) :
	$(MAKE) -C $(@:rom_techtom-%=%) rom_techtom
	
clean: $(ROMS_CLEAN)
	
$(ROMS_CLEAN):
	$(MAKE) -C $(@:clean-%=%) clean
	
	