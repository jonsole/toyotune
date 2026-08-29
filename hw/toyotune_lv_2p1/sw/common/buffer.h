#ifndef BUFFER_H_
#define BUFFER_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
//#include <stdio.h>

#define BufferInit(Buf)				do {(Buf).Index = (Buf).Outdex = 0; } while (0)

#define BufferCapacity(Buf)			(sizeof(Buf.Buffer) - 1)
#define BufferSize(Buf)				(sizeof(Buf.Buffer))
#define BufferIndexMask(Buf)		(sizeof(Buf.Buffer) - 1)

#define BufferIndex(Buf)			((Buf.Index) & BufferIndexMask(Buf))
#define BufferOutdex(Buf)			((Buf.Outdex) & BufferIndexMask(Buf))

#define BufferSetIndex(Buf, NewIndex) \
	do { (Buf).Index = (NewIndex); } while (0)
#define	BufferSetIndexFromAddress(Buf, Address) \
	do { (Buf).Index = (uint8_t *)(Address) - (Buf).Buffer; } while (0)
#define BufferSetOutdex(Buf, NewOutdex)	\
	do { (Buf).Outdex = (NewOutdex); } while (0)
#define	BufferSetOutdexFromAddress(Buf, Address) \
	do { (Buf).Outdex = (uint8_t *)(Address) - (Buf).Buffer; } while (0)

#define BufferAddIndex(Buf, AddIndex)	do { (Buf).Index += (AddIndex); } while (0)
#define BufferAddOutdex(Buf, AddOutdex)	do { (Buf).Outdex += (AddOutdex); } while (0)

#define BufferSpace(Buf)		(BufferCapacity(Buf) - BufferAmount(Buf))
#define BufferSpaceToWrap(Buf)	(BufferSize(Buf) - (Buf.Index & BufferIndexMask(Buf)))
#define BufferAmount(Buf)		((Buf.Index - Buf.Outdex) & BufferIndexMask(Buf))
#define BufferAmountToWrap(Buf)	(BufferSize(Buf) - (Buf.Outdex & BufferIndexMask(Buf)))
#define BufferIsEmpty(Buf)		(!BufferAmount(Buf))

#define BufferWrite(Buf, Data) \
	do {Buf.Buffer[BufferIndex(Buf)] = Data; Buf.Index++;} while (0)

#define BufferUnWrite(Buf) \
	do {Buf.Index--;} while (0)
		
#define BufferWriteWithIndex(Buf, Data, Index) \
	do {Buf.Buffer[Index & BufferIndexMask(Buf)] = Data; Index++;} while (0)

#define BufferRead(Buf) \
	(Buf.Buffer[(Buf.Outdex++) & BufferIndexMask(Buf)])

#endif
