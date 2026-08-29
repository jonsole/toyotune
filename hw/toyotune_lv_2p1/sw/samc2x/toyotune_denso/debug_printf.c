/*
 * debug_printf.h
 *
 * Created: 18/10/2016 20:52:50
 *  Author: Jon
 */ 


#ifndef DEBUG_PRINTF_H_
#define DEBUG_PRINTF_H_

/*------------------------------------------------------------------------/
/  Universal string handler for user console interface
/-------------------------------------------------------------------------/
/
/  Copyright (C) 2011, ChaN, all right reserved.
/
/ * This software is a free software and there is NO WARRANTY.
/ * No restriction on use. You can use, modify and redistribute it for
/   personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
/ * Redistributions of source code must retain the above copyright notice.
/
/-------------------------------------------------------------------------*/
#include <stdarg.h>
#include "debug.h"

#if 0

/*----------------------------------------------*/
/* Formatted string output                      */
/*----------------------------------------------*/
/*  xprintf("%d", 1234);			"1234"
    xprintf("%6d,%3d%%", -200, 5);	"  -200,  5%"
    xprintf("%-6u", 100);			"100   "
    xprintf("%ld", 12345678L);		"12345678"
    xprintf("%04x", 0xA3);			"00a3"
    xprintf("%08LX", 0x123ABC);		"00123ABC"
    xprintf("%016b", 0x550F);		"0101010100001111"
    xprintf("%s", "String");		"String"
    xprintf("%-4s", "abc");			"abc "
    xprintf("%4s", "abc");			" abc"
    xprintf("%c", 'a');				"a"
    xprintf("%f", 10.0);            <xprintf lacks floating point support>
*/

void xvprintf(const char *Format, va_list ArgList, void (*PutCharFunc)(char, void *), void *PutCharData)
{
	unsigned int Radix, Index, StringLen, MinimumWidth, Flags;
	unsigned long Value;
	char StringBuffer[16], Char, Digit, *StringPtr;

	for (;;)
	{
		Char = *Format++;
		if (!Char)
			break;

		/* Pass through it if not a % sequence */
		if (Char != '%')
		{
			PutCharFunc(Char, PutCharData);
			continue;
		}

		Flags = 0;
		Char = *Format++;

		/* Flag: '0' padded */
		if (Char == '0')
		{	
			Flags = 1;
			Char = *Format++;
		}
		else
		{
			/* Flag: left justified */
			if (Char == '-')
			{	
				Flags = 2;
				Char = *Format++;
			}
		}

		/* Minimum width */
		for (MinimumWidth = 0; Char >= '0' && Char <= '9'; Char = *Format++)
			MinimumWidth = MinimumWidth * 10 + Char - '0';

		/* Prefix: Size is long int */
		if (Char == 'l' || Char == 'L')
		{
			Flags |= 4;
			Char = *Format++;
		}

		/* End of format? */
		if (!Char)
			break;

		Digit = Char;
		if (Digit >= 'a')
			Digit -= 0x20;

		switch (Digit)
		{
			/* String */
			case 'S':
				StringPtr = va_arg(ArgList, char*);
				for (StringLen = 0; StringPtr[StringLen]; StringLen++);
				while (!(Flags & 2) && StringLen++ < MinimumWidth)
					PutCharFunc(' ', PutCharData);
				while (*StringPtr)
					PutCharFunc(*StringPtr++, PutCharData);
				while (StringLen++ < MinimumWidth)
					PutCharFunc(' ', PutCharData);
				continue;
		
			/* String */
			case 'C':
				PutCharFunc((char)va_arg(ArgList, int), PutCharData);
				continue;
		
			/* Binary */
			case 'B':
				Radix = 2;
				break;

			/* Octal */
			case 'O':
				Radix = 8;
				break;

			/* Signed decimal */
			case 'D':
			/* Unsigned decimal */
				case 'U':
				Radix = 10;
				break;

			/* Hexadecimal */
			case 'X':
				Radix = 16;
				break;

			/* Unknown type (pass through) */
			default:
				PutCharFunc(Char, PutCharData);
				continue;
		}

		/* Get an argument and put it in numeral */
		Value = (Flags & 4) ? va_arg(ArgList, long) : ((Digit == 'D') ? (long)va_arg(ArgList, int) : (long)va_arg(ArgList, unsigned int));
		if (Digit == 'D' && (Value & 0x80000000))
		{
			Value = 0 - Value;
			Flags |= 8;
		}

		Index = 0;
		do
		{
			Digit = (char)(Value % Radix);
			Value /= Radix;

			if (Digit > 9)
				Digit += (Char == 'x') ? 0x27 : 0x07;

			StringBuffer[Index++] = Digit + '0';
		} while (Value && Index < sizeof(StringBuffer));
		
		if (Flags & 8)
			StringBuffer[Index++] = '-';

		StringLen = Index;
		Digit = (Flags & 1) ? '0' : ' ';

		while (!(Flags & 2) && StringLen++ < MinimumWidth)
			PutCharFunc(Digit, PutCharData);

		do
		{
			PutCharFunc(StringBuffer[--Index], PutCharData);
		}
		while(Index);

		while (StringLen++ < MinimumWidth)
			PutCharFunc(' ', PutCharData);
	}
}


void Debug_PrintF(const char *Format, ...)
{
	va_list ArgList;
	va_start(ArgList, Format);
	xvprintf(Format, ArgList, Debug_PutChar, NULL);
	va_end(ArgList);
}

#endif

#endif /* DEBUG_PRINTF_H_ */