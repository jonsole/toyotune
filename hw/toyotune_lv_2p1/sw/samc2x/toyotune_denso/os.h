/*
 * os.h
 *
 * Created: 26/01/2017 19:53:46
 *  Author: Jon
 */ 


#ifndef OS_H_
#define OS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sam.h>

#include "os_task_id.h"

#define OS_TASK_STATUS_READY	0
#define OS_TASK_STATUS_ACTIVE	1
#define OS_TASK_STATUS_WAIT		2

#define OS_TASK_SIGNAL_MSG		(1 << 0)

extern uint8_t OS_InterruptDisableCount;

__inline static bool OS_IsInterruptsDisabled(void)
{
	return OS_InterruptDisableCount > 0;
}

__inline static void OS_InterruptDisable(void)
{
	__disable_irq();
	OS_InterruptDisableCount++;
}


__inline static void OS_InterruptEnable(void)
{
	OS_InterruptDisableCount--;
	if (OS_InterruptDisableCount == 0)
		__enable_irq();
}


typedef struct OS_ListNode
{
	struct OS_ListNode *Succ;
	struct OS_ListNode *Pred;
} OS_ListNode_t;

typedef struct OS_List
{
	OS_ListNode_t *Head;
	OS_ListNode_t *Tail;
	OS_ListNode_t *TailPred;
} OS_List_t;


typedef uint8_t OS_TaskId_t;
typedef uint16_t OS_SignalSet_t;

__inline static void OS_ListInit(OS_List_t *List)
{
	List->Head = (OS_ListNode_t *)&List->Tail;
	List->Tail = 0;
	List->TailPred = (OS_ListNode_t *)&List->Head;
}

__inline static bool OS_ListIsEmpty(OS_List_t *List)
{
	return (List->TailPred == (OS_ListNode_t *)List);
}

__inline static OS_ListNode_t *OS_ListHead(OS_List_t *List)
{
	if (!OS_ListIsEmpty(List))
		return List->Head;
	else
		return NULL;
}

__inline static OS_ListNode_t *OS_ListTail(OS_List_t *List)
{
	if (!OS_ListIsEmpty(List))
		return List->TailPred;
	else
		return NULL;
}

__inline static void OS_ListAddHead(OS_List_t *List, OS_ListNode_t *Node)
{

    /*
	Make the node point to the old first node in the list and to the
	head of the list.
    */
    Node->Succ = List->Head;
    Node->Pred = (OS_ListNode_t *)&List->Head;

    /*
	New we come before the old first node which must now point to us
	and the same applies to the pointer to-the-first-node in the
	head of the list.
    */
    List->Head->Pred = Node;
    List->Head = Node;
}

__inline static void OS_ListAddTail(OS_List_t *List, OS_ListNode_t *Node)
{

    /*
	Make the node point to the head of the list. Our predecessor is the
	previous last node of the list.
    */
    Node->Succ	       = (OS_ListNode_t *)&List->Tail;
    Node->Pred	       = List->TailPred;

    /*
	Now we are the last now. Make the old last node point to us
	and the pointer to the last node, too.
    */
    List->TailPred->Succ = Node;
    List->TailPred	     = Node;
}

__inline static void OS_ListRemove(OS_ListNode_t *Node)
{
    /*
	Just bend the pointers around the node, ie. we make our
	predecessor point to our successor and vice versa
    */
    Node->Pred->Succ = Node->Succ;
    Node->Succ->Pred = Node->Pred;
}

__inline static OS_ListNode_t *OS_ListRemoveHead(OS_List_t *List)
{
    /* Get the first node of the list */
    OS_ListNode_t *Node = OS_ListHead(List);
    if (Node)
	    OS_ListRemove(Node);

    /* Return it's address or NULL if there was no node */
    return Node;
}

__inline static OS_ListNode_t *OS_ListRemoveTail(OS_List_t *List)
{
    /* Get the last node of the list */
	OS_ListNode_t *Node = OS_ListTail(List);
    if (Node)
		OS_ListRemove(Node);

    /* Return it's address or NULL if there was no node */
    return Node;
}

typedef struct OS_Task
{
	OS_ListNode_t Node;

	/* The stack pointer (sp) has to be the first element as it is located
	   at the same address as the structure itself (which makes it possible
	   to locate it safely from assembly implementation of PendSV_Handler).
	   The compiler might add padding between other structure elements. */
	volatile uint32_t StackPtr;
	void (*Handler)(void *);
	uint16_t Status;
	OS_SignalSet_t SignalWait;
	OS_SignalSet_t SignalReceived;

	OS_List_t MessageQueue;

} OS_Task_t;

void OS_Init(void);
void OS_Start(void);

void OS_Switch(void);
void OS_Wait(void);
void OS_Dispatch(void);

void OS_TaskInit(OS_TaskId_t TaskId, void (*Handler)(void *), void *UserData, void *StackVoidPtr, uint32_t StackSizeBytes);

#define OS_SIGNAL_MESSAGE	(1 << 0)	/* Signal sent when message delivered to task */
#define OS_SIGNAL_WAIT		(1 << 1)	/* Signal sent to wake up a waiting task */
#define OS_SIGNAL_USER		(2)

OS_SignalSet_t OS_SignalWait(OS_SignalSet_t SignalMask);
void OS_SignalSend(OS_TaskId_t TaskId, OS_SignalSet_t SignalMask);
OS_SignalSet_t OS_SignalGet(void);

typedef uint8_t OS_CpuId_t;
extern OS_Task_t *OS_TaskCurrent;
extern OS_Task_t OS_TaskTable[];

inline static OS_TaskId_t OS_TaskId(void)
{
	return OS_TaskCurrent - OS_TaskTable;	
}

__inline static bool OS_TaskIdIsLocal(OS_TaskId_t TaskId)
{
	OS_CpuId_t CpuId = TaskId >> 5;
	return (CpuId == 0) || (CpuId == OS_CPU_ID);
}



typedef struct OS_Message
{
	OS_ListNode_t Node;
	uint8_t Sender;
	uint8_t Destination;
	uint16_t Id;
	uint8_t Payload[0];
} OS_Message_t;

OS_Message_t *OS_MessageWait(void);
OS_Message_t *OS_MessageGet(void);
void OS_MessageSend(OS_Message_t *Message, OS_TaskId_t Destination);
void OS_MessageSendWithSender(OS_Message_t *Message, OS_TaskId_t Destination, OS_TaskId_t Sender);

OS_TaskId_t OS_MessageInterfaceRegister(OS_TaskId_t Task, OS_CpuId_t Cpu);

#define OS_AtomicBlock
#define OS_ForbidBlock

#endif /* OS_H_ */