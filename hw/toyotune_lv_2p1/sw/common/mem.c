#include <sam.h>

#include "mem.h"
#include "debug.h"
#include "os.h"

extern uint8_t *_end;

#define NUM_POOLS (7)

typedef struct MEM_FreeBlock
{
	struct MEM_FreeBlock *Next;
} MEM_FreeBlock_t;

typedef struct 
{
	uint16_t Size;
	void *Start;
	MEM_FreeBlock_t *Free;
} MEM_Pool_t;

typedef struct 
{
	uint16_t Size;
	uint16_t NumBlocks;
} MEM_PoolConfig_t;

MEM_Pool_t MEM_Pool[NUM_POOLS];

static const MEM_PoolConfig_t MEM_PoolConfig[NUM_POOLS] =
{
	{8,   40},
	{16,  20},
	{32,  20},
	{48,  20},
	{64,  20},
	{128, 10},
	{256, 5},
};

void MEM_Init(void)
{
	uint8_t *MemPtr = (uint8_t *)&_end;
#ifdef HMCRAMC0_ADDR
	uint8_t *MemEnd = (uint8_t *)HMCRAMC0_ADDR + HMCRAMC0_SIZE;
#else
	uint8_t *MemEnd = (uint8_t *)HSRAM_ADDR + HSRAM_SIZE;
#endif

	for (int Pool = 0; Pool < NUM_POOLS; Pool++)
	{
		/* Initialise pool */
		MEM_Pool[Pool].Free = NULL;
		MEM_Pool[Pool].Start = MemPtr;
		MEM_Pool[Pool].Size = MEM_PoolConfig[Pool].Size;

		/* Add blocks to pool */
		for (int Block = 0; Block < MEM_PoolConfig[Pool].NumBlocks; Block++)
		{
			/* Check enough space left for this block */
			if (MemPtr + MEM_Pool[Pool].Size >= MemEnd)
			{
				Debug("MEM_Init, out of memory to create pool size %u\n", MEM_Pool[Pool].Size);
				break;
			}

			/* Create block and add it to pool's free list */
			MEM_FreeBlock_t *Block = (MEM_FreeBlock_t *)MemPtr;
			Block->Next = MEM_Pool[Pool].Free;
			MEM_Pool[Pool].Free = Block;

			/* Advance memory pointer */
			MemPtr += MEM_Pool[Pool].Size;
		}
	}
}


void *MEM_Alloc(uint16_t Size)
{
	for (int Index = 0; Index < NUM_POOLS; Index++)
	{
		if (MEM_Pool[Index].Size >= Size)
		{
			OS_InterruptDisable();
			MEM_FreeBlock_t *Mem = MEM_Pool[Index].Free;
			if (Mem)
			{
				MEM_Pool[Index].Free = Mem->Next;
				OS_InterruptEnable();
				return Mem;
			}
			OS_InterruptEnable();
		}
	}

	//PanicMessage("Out Of Memory");
	return NULL;
}


void MEM_Free(const void *Mem)
{
	int Index = NUM_POOLS - 1;

	/* Find pool that block belongs to */
	while (Mem < MEM_Pool[Index].Start)
	{
		if (Index == 0)
			return;

		Index--;
	}
	
	/* Handle address that's not at start of block */
	uint16_t Offset = (Mem - MEM_Pool[Index].Start) % MEM_Pool[Index].Size;
	Mem -= Offset;
	
	MEM_FreeBlock_t *MemBlock = (MEM_FreeBlock_t *)Mem;

	//AtomicBlock
	{
#if 0	/* Check block isn't already in free list */
		MEM_FreeBlock_t *Block = MEM_Pool[Index].Free;
		while (Block != NULL)
		{
			if (Block == MemBlock)
				return;
			Block = Block->Next;
		}
#endif

		OS_InterruptDisable();
		
		/* Link block to start of free list */
		MemBlock->Next = MEM_Pool[Index].Free;
		MEM_Pool[Index].Free = MemBlock;
		
		OS_InterruptEnable();
	}
}
