/*
 * os.c
 *
 * Created: 26/01/2017 19:58:26
 *  Author: Jon
 */ 
#include <sam.h>
#include <stdbool.h>
#include <stdint.h>

#include "os.h"
#include "debug.h"

OS_List_t OS_TaskReadyList;
OS_List_t OS_TaskWaitList;

OS_Task_t *OS_TaskCurrent = 0;
OS_Task_t *OS_TaskDispatch = 0;

OS_Task_t OS_TaskTable[10];	// Task ID 0 is link handler task
OS_TaskId_t OS_MessageInterfaceTable[8];

uint8_t OS_InterruptDisableCount;

static void OS_TaskFinished(void)
{
	OS_SignalWait(0);
}


void OS_Init(void)
{
	OS_ListInit(&OS_TaskReadyList);
	OS_ListInit(&OS_TaskWaitList);
}

void OS_TaskInit(OS_TaskId_t TaskId, void (*Handler)(void *), void *UserData, void *StackVoidPtr, uint32_t StackSizeBytes)
{
	uint32_t StackSize = StackSizeBytes / sizeof(uint32_t);
	uint32_t *StackPtr = (uint32_t *)StackVoidPtr + StackSize;
	
	OS_Task_t *Task = &OS_TaskTable[TaskId];

	/* Initialize the task structure and set SP to the top of the stack
	   minus 16 words (64 bytes) to leave space for storing 16 registers: */
	Task->Handler = Handler;
	Task->Status = OS_TASK_STATUS_READY;

	/* Save special registers which will be restored on exc. return:
	   - XPSR: Default value (0x01000000)
	   - PC: Point to the handler function
	   - LR: Point to a function to be called when the handler returns */
	*(--StackPtr) = 0x01000000;
	*(--StackPtr) = (uint32_t)Handler;
	*(--StackPtr) = (uint32_t)&OS_TaskFinished;

	/* Initialise register values */
	*(--StackPtr) = 12;  /* R12 */
	*(--StackPtr) = 3;   /* R3  */
	*(--StackPtr) = 2;   /* R2  */
	*(--StackPtr) = 1;   /* R1  */
	*(--StackPtr) = (uint32_t)UserData;   /* R0  */
	*(--StackPtr) = 7;   /* R7  */
	*(--StackPtr) = 6;  /* R6  */
	*(--StackPtr) = 5;  /* R5  */
	*(--StackPtr) = 4;  /* R4  */
	*(--StackPtr) = 11; /* R11 */
	*(--StackPtr) = 10; /* R10 */
	*(--StackPtr) = 9;  /* R9  */
	*(--StackPtr) = 8;  /* R8  */

	Task->StackPtr = (uint32_t)(StackPtr);

	/* Initialise message queue */
	OS_ListInit(&Task->MessageQueue);

	OS_InterruptDisable();

	/* Add task to ready list */
	OS_ListAddTail(&OS_TaskReadyList, &Task->Node);

	OS_InterruptEnable();
}

void OS_Start(void)
{
	NVIC_SetPriority(PendSV_IRQn, 0xFF); /* Lowest possible priority */

	/* Start the first task */
	PanicFalse(!OS_ListIsEmpty(&OS_TaskReadyList));
	OS_TaskCurrent = (OS_Task_t *)OS_ListRemoveHead(&OS_TaskReadyList);

	/* Set PSP to the top of task's stack */
	__set_PSP(OS_TaskCurrent->StackPtr + 64); 

	/* Switch to PSP, privileged mode */
	__set_CONTROL(0x02); 

	/* Exec. ISB after changing CONTROL (recommended) */
	__ISB();

	/* Globally enable interrupts */
	__enable_irq();

	uint32_t *StackPtr = (uint32_t *)OS_TaskCurrent->StackPtr;
	OS_TaskCurrent->Handler((void *)(StackPtr[8]));
}


void OS_Switch(void)
{
	/* Update task status */
	OS_TaskCurrent->Status = OS_TASK_STATUS_READY;

	/* Move task to ready list */
	OS_ListAddTail(&OS_TaskReadyList, &OS_TaskCurrent->Node);

	/* Dispatch next task */
	OS_Dispatch();
}


void OS_Wait(void)
{
	/* Update task status */
	OS_TaskCurrent->Status = OS_TASK_STATUS_WAIT;

	/* Move task to wait list */
	OS_ListAddHead(&OS_TaskWaitList, &OS_TaskCurrent->Node);

	/* Dispatch next task */
	OS_Dispatch();
}


/* Dispatch next task */
void OS_Dispatch(void)
{
	/* Disable interrupts before checking ready list */
	OS_InterruptDisable();

	/* Wait if ready list is empty */
	while (OS_ListIsEmpty(&OS_TaskReadyList))
	{
		/* No task ready, re-enable interrupts and wait for an interrupt */
		OS_InterruptEnable();
		__WFI();

		/* We've woken up, so disable interrupts and loop around to check ready list */
		OS_InterruptDisable();
	}

	/* Remove task at head of ready list */
	OS_TaskDispatch = (OS_Task_t *)OS_ListRemoveHead(&OS_TaskReadyList);

	/* Re-enable interrupts now that we're done with manipulating task lists */
	OS_InterruptEnable();

	/* Mark task as active */
	OS_TaskDispatch->Status = OS_TASK_STATUS_ACTIVE;

	/* Trigger PendSV which performs the actual context switch: */
	SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}


OS_SignalSet_t OS_SignalWait(OS_SignalSet_t SignalSet)
{
	OS_SignalSet_t Signal;

	/* Wait until we have at least one signal we're waiting for */
	while (!(Signal = OS_TaskCurrent->SignalReceived & SignalSet))
	{
		/* Set mask of events we're waiting for */
		OS_TaskCurrent->SignalWait = SignalSet;
			
		/* Make task wait */
		OS_Wait();
	}

	/* Disable interrupts before modifying signals */
	OS_InterruptDisable();

	/* Clear signals we were waiting for */
	OS_TaskCurrent->SignalReceived &= ~SignalSet;

	/* Signals modified, so re-enable interrupts */
	OS_InterruptEnable();

	/* Return signal(s) that woke us */
	return Signal;
}

void OS_SignalSend(OS_TaskId_t TaskId, OS_SignalSet_t Signals)
{
	OS_Task_t *Task = &OS_TaskTable[TaskId];
	
	/* Disable interrupts before adding signals and moving task between lists */
	OS_InterruptDisable();

	/* Add in new signals */
	Task->SignalReceived |= Signals;

	/* Check if task is waiting */
	if (Task->Status == OS_TASK_STATUS_WAIT)
	{
		/* Check if task now has a signal is is waiting for */
		if (Task->SignalReceived & Task->SignalWait)
		{
			/* Move task to ready list */
			OS_ListRemove(&Task->Node);
			OS_ListAddHead(&OS_TaskReadyList, &Task->Node);

			/* No need to dispatch, if no task was running and we were called
			   from interrupt handler then OS_Dispatch will exit loop
			   and dispatch this task when interrupt handler finishes */
		}
	}

	/* All done, so re-enable interrupts */
	OS_InterruptEnable();
}


OS_SignalSet_t OS_SignalGet(void)
{
	OS_InterruptDisable();
	OS_SignalSet_t Signals = OS_TaskCurrent->SignalReceived;
	OS_TaskCurrent->SignalReceived = 0;
	OS_InterruptEnable();
	return Signals;
}

OS_Message_t *OS_MessageGet(void)
{
	if (OS_ListIsEmpty(&OS_TaskCurrent->MessageQueue))
		return NULL;
	else
		return (OS_Message_t *)OS_ListRemoveHead(&OS_TaskCurrent->MessageQueue);
}

OS_Message_t *OS_MessageWait(void)
{
	while (OS_ListIsEmpty(&OS_TaskCurrent->MessageQueue))
		OS_SignalWait(OS_SIGNAL_MESSAGE);

	return OS_MessageGet();
}

void OS_MessageSendWithSender(OS_Message_t *Message, OS_TaskId_t Destination, OS_TaskId_t Sender)
{
	OS_Task_t *Task;
	OS_TaskId_t TaskId;

	if (OS_TaskIdIsLocal(Destination))
	{
		TaskId = Destination & 0x1F;
		Task = &OS_TaskTable[TaskId];
	}
	else
	{
		/* Remote task, send to registered interface task for forwarding */
		TaskId = OS_MessageInterfaceTable[Destination >> 5];
		Task = &OS_TaskTable[TaskId];
	}

	/* Store sender and destination in message */
	Message->Sender = Sender;
	Message->Destination = Destination;

	/* Add message to queue of destination task */
	OS_ListAddTail(&Task->MessageQueue, &Message->Node);

	/* Send signal to wake up task */
	OS_SignalSend(TaskId, OS_SIGNAL_MESSAGE);
}


void OS_MessageSend(OS_Message_t *Message, OS_TaskId_t Destination)
{
	OS_MessageSendWithSender(Message, Destination, OS_TaskCurrent - OS_TaskTable);
}


OS_TaskId_t OS_MessageInterfaceRegister(OS_TaskId_t Task, OS_CpuId_t Cpu)
{
	OS_TaskId_t TaskOld = OS_MessageInterfaceTable[Cpu];
	OS_MessageInterfaceTable[Cpu] = Task;
	return TaskOld;
}

