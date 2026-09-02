/*
 * cli.c
 *
 * Created: 29/06/2021 13:11:48
 *  Author: jonso
 */ 
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "buffer.h"
#include "debug.h"
#include "dcc.h"

typedef struct
{
	uint8_t Index;
	uint8_t Outdex;
	char Buffer[64];	
}  CLI_Buffer_t;	

CLI_Buffer_t CLI_Buffer;

typedef struct 
{
	uint8_t Loco;
	uint32_t Functions;
} CLI_LocoFunctions_t;

CLI_LocoFunctions_t CLI_LocoFunctions[32];

uint32_t CLI_GetLocoFunctions(uint8_t Loco)
{
	for (int Index = 0; Index < 32; Index++)
	{
		
		if (CLI_LocoFunctions[Index].Loco == Loco)
			return CLI_LocoFunctions[Index].Functions;
	}
	return 0;
}

void CLI_SetLocoFunctions(uint8_t Loco, uint32_t Functions)
{
	for (int Index = 0; Index < 32; Index++)
	{		
		if (CLI_LocoFunctions[Index].Loco == Loco)
		{
			CLI_LocoFunctions[Index].Functions = Functions;
			return;
		}
	}
	
	for (int Index = 0; Index < 32; Index++)
	{		
		if (CLI_LocoFunctions[Index].Loco == 0)
		{
			CLI_LocoFunctions[Index].Loco = Loco;			
			CLI_LocoFunctions[Index].Functions = Functions;
			return;
		}
	}
	
}


typedef struct 
{
	int (*command)(int arvc, const char *argv[]);
	const char *name;
	const char *args;
	const char *help;
} CLI_Command_t;


static bool CLI_ArgToInt(const char *Arg, int *Value)
{
	char *end;
	long l = strtol(Arg, &end, 0);		
	*Value = l;
	return 1;
}

int CLI_CommandCv(int argc, const char *argv[])
{
	if (argc != 2)
		return -1;
		
	int CvId, CvValue;
	if (CLI_ArgToInt(argv[1], &CvId) && CLI_ArgToInt(argv[2], &CvValue))
	{
		Debug("Setting CV %u to %u\n", CvId, CvValue);
		DCC_CvWrite(CvId, CvValue);
		return 0;
	}	
	else
		return -1;
}


int CLI_CommandSpeed(int argc, const char *argv[])
{
	if (argc != 2)
		return -1;
		
	int Loco, Speed;
	if (CLI_ArgToInt(argv[1], &Loco) && CLI_ArgToInt(argv[2], &Speed))
	{
		if (Speed < 0)
			DCC_SetLocomotiveSpeed(Loco, -Speed, 0);
		else
			DCC_SetLocomotiveSpeed(Loco, Speed, 1);
		return 0;
	}	
	else
		return -1;
}

int CLI_CommandFunction(int argc, const char *argv[])
{
	if (argc != 2)
		return -1;
		
	int Loco, Function;
	if (CLI_ArgToInt(argv[1], &Loco) && CLI_ArgToInt(argv[2], &Function))
	{
		uint32_t FunctionMap = CLI_GetLocoFunctions(Loco);
		FunctionMap ^= (1UL << Function);
		CLI_SetLocoFunctions(Loco, FunctionMap);
		Debug("Local %u functions %08x\n", Loco, FunctionMap);

		if (Function <= 4)
			DCC_SetLocomotiveFunctions(Loco, ((FunctionMap >> 1) & 0b01111) | ((FunctionMap & 0b00001) << 4), 0); /* F1 - F4 */
		else if (Function <= 8)
			DCC_SetLocomotiveFunctions(Loco, ((FunctionMap >> 5) & 0b01111), 5); /* F5 - F8 */
		else if (Function <= 12)
			DCC_SetLocomotiveFunctions(Loco, ((FunctionMap >> 9) & 0b01111), 9); /* F9 - F12 */
		else if (Function <= 20)
			DCC_SetLocomotiveFunctions(Loco, ((FunctionMap >> 13) & 0b1111111), 13); /* F13 - F20 */
		else if (Function <= 28)
			DCC_SetLocomotiveFunctions(Loco, ((FunctionMap >> 21) & 0b1111111), 21); /* F21 - F28 */
		else if (Function <= 36)
			DCC_SetLocomotiveFunctions(Loco, ((FunctionMap >> 29) & 0b1111111), 29); /* F29 - F36 */

		return 0;		
	}	
	else
		return -1;
}



const CLI_Command_t CLI_CommandTable[] = 
{
	{ CLI_CommandCv, "CV", "ID/N VALUE/N", "Write CV value"	},
	{ CLI_CommandSpeed, "SP", "LOCO/N SPEED/N", "Set locomotive speed" },
	{ CLI_CommandFunction,  "FN", "LOCO/N FUNCTION/B", "Toggle function on or off" },
	{ 0, 0, 0, 0 }
};



static void CLI_ParseLine(void)
{
	const char *argv[10] = { NULL };
	int8_t argc = -1;
	
	char *str = CLI_Buffer.Buffer;
	char *token = strtok_r(str, " ", &str);
	while (token)
	{
		if (token[0] == '#')
			break;
		argv[++argc] = token;
		token = strtok_r(str, " ", &str);
	}
	
	if (argc >= 0)
	{
		const CLI_Command_t *Command = CLI_CommandTable;
		while (Command->command)
		{
			if (strcasecmp(Command->name, argv[0]) == 0)
				break;
			Command += 1;
		}
		
		if (Command->command)
		{
			int Result = Command->command(argc, argv);
			if (Result)
				Debug_PrintF("'%s' returned %d\n", argv[0], Result);
		}
		else
			Debug_PrintF("Unknown command '%s'\n", argv[0]);
	}
	
	BufferInit(CLI_Buffer);
}
	

void CLI_InputChar(uint8_t Char)
{
	switch (Char)
	{
		case '\r':
		{
			BufferWrite(CLI_Buffer, 0);		
			Debug_PutChar('\n');
			CLI_ParseLine();
		}
		break;
		
		case 127:
		{
			if (BufferAmount(CLI_Buffer) > 0)
			{
				BufferUnWrite(CLI_Buffer);
				Debug_PutChar(127);
			}
			else
				Debug_PutChar(7);			
		}
		break;
		
		default:
		{
			if (isprint(Char) && BufferSpace(CLI_Buffer) >= 1)
			{
				Debug_PutChar(Char);
				BufferWrite(CLI_Buffer, Char);
			}		
			else
				Debug_PutChar(7);
		}
		break;	
	}
}

void CLI_Task(void *Instance)
{
	for (;;)
	{
		OS_SignalSet_t Sig = OS_SignalWait(0xFFFFUL);
		uint8_t Char = Debug_GetChar();
		while (Char)
		{
			CLI_InputChar(Char);
			Char = Debug_GetChar();
		}
	}
}

void CLI_Init(void)
{
	static uint32_t CLI_TaskStack[256];
	OS_TaskInit(CLI_TASK_ID, CLI_Task, NULL, CLI_TaskStack, sizeof(CLI_TaskStack));

	BufferInit(CLI_Buffer);
}
