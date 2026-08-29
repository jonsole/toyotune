#ifndef __MEM_H
#define __MEM_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void MEM_Init(void);

extern void *MEM_Alloc(uint16_t);

extern void MEM_Free(const void *);

#define MEM_Create(x)  MEM_Alloc(sizeof(x))

#ifdef __cplusplus
}
#endif

#endif 