#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <iterator>
#include <vector>
#include <iostream>

using namespace std;
#define DEBUG

/* Command line to fix checksum
    
   checksum input output
   
   image        - filename of ROM image
*/

int main(int argc, const char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Not enough command line arguments:  input output\n");
        exit(1);
    }
    
    const char *filename_rom = argv[1];    

    FILE *file_rom;
	fopen_s(&file_rom, filename_rom, "rb");
    if (!file_rom)
    {
        fprintf(stderr, "Unable to open file to read %s", filename_rom);
        exit(1);
    }        

    unsigned char *rom = (unsigned char *)malloc(65536);
    if (!rom)
    {
        fprintf(stderr, "Unable to allocate ROM buffer");
        exit(1);
    }    
    
	fseek(file_rom, 0, SEEK_END); // seek to end of file
	size_t file_size = ftell(file_rom); // get current file pointer
	fseek(file_rom, 0, SEEK_SET); // seek back to beginning of file

	/* Read ROM data */
    if (fread(&rom[65536 - file_size], 1, file_size, file_rom) != file_size)
    {
        fprintf(stderr, "Unable to read file");
        exit(1);
    }

	/* Close file */
	fclose(file_rom);

    /* Fix ld #02h,OMODE instruction */
    long vector = (rom[0xFFFE] << 8) | rom[0xFFFF];
    if (rom[vector + 0] == 0x33 && rom[vector + 1] == 0x07)
	{
	    fprintf(stdout, "Fixing OMODE register write at %04x\n", vector);
		rom[vector + 1] = 0x02;
	}

	/* Calculate new checksum */
	unsigned old_adjustment = (rom[0xFFDA] << 8) | rom[0xFFDB];
	rom[0xFFDA] = 0;
	rom[0xFFDB] = 0;
	unsigned checksum = 0;
    for (unsigned address = 65536 - file_size; address < 65536; address += 2)
    {
        unsigned word = (rom[address + 0] << 8) + (rom[address + 1] << 0);
        checksum = (checksum + word) & 0xFFFF;
    }

    /* Calculate adjustment word */
    unsigned adjustment = (0xAA55 - checksum) & 0xFFFF;

    /* Store checksum adjustment word */
    rom[0xFFDA] = (adjustment >> 8) & 0xFF;
    rom[0xFFDB] = (adjustment >> 0) & 0xFF;
	if (adjustment != old_adjustment)
		fprintf(stdout, "New checksum adjustment word is %04x\n", adjustment);

    filename_rom = argv[2];    
    fopen_s(&file_rom, filename_rom, "wb");
    if (!file_rom)
    {
        fprintf(stderr, "Unable to open file to write  %s", filename_rom);
        exit(1);
    }        

    if (fwrite(&rom[65536 - file_size], 1, file_size, file_rom) != file_size)
    {
       fprintf(stderr, "Unable to write file");
        exit(1);
    }

	/* Close file */
	fclose(file_rom);

}