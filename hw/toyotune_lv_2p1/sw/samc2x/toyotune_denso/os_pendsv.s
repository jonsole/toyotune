
/*
 * os_pendsv.s
 *
 * Created: 26/01/2017 20:25:07
 *  Author: Jon
 */ 


.syntax unified
.cpu cortex-m0
.fpu softvfp

.thumb

.global PendSV_Handler
.type PendSV_Handler, %function
PendSV_Handler:
				/* Disable interrupts: */
				cpsid	i

				/*
				Exception frame saved by the NVIC hardware onto stack:
				+------+
				|      | <- SP before interrupt (orig. SP)
				| xPSR |
				|  PC  |
				|  LR  |
				|  R12 |
				|  R3  |
				|  R2  |
				|  R1  |
				|  R0  | <- SP after entering interrupt (orig. SP + 32 bytes)
				+------+

				Registers saved by the software (PendSV_Handler):
				+------+
				|  R7  |
				|  R6  |
				|  R5  |
				|  R4  |
				|  R11 |
				|  R10 |
				|  R9  |
				|  R8  | <- Saved SP (orig. SP + 64 bytes)
				+------+
				*/

				/* Save registers R4-R11 (32 bytes) onto current PSP (process stack
				   pointer) and make the PSP point to the last stacked register (R8):
				   - The MRS/MSR instruction is for loading/saving a special registers.
				   - The STMIA inscruction can only save low registers (R0-R7), it is
					 therefore necesary to copy registers R8-R11 into R4-R7 and call
					 STMIA twice. */
				MRS		R0, PSP
				SUBS	R0, #16
				STMIA	R0!,{R4-R7}
				MOV		R4, R8
				MOV		R5, R9
				MOV		R6, R10
				MOV		R7, R11
				SUBS	R0, #32
				STMIA	R0!,{R4-R7}
				SUBS	R0, #16

				/* Save current task's SP: */
				LDR		R2,=OS_TaskCurrent
				LDR		R1,[R2]
				STR		R0,[R1,8]

				/* TODO: Write 1 to PENDSVCLR */

				/* Load next task's SP: */
				LDR		R3,=OS_TaskDispatch
				LDR		R1,[R3]
				STR		R1,[R2]
				LDR		R0,[R1,8]

				/* Load registers R4-R11 (32 bytes) from the new PSP and make the PSP
				   point to the end of the exception stack frame. The NVIC hardware
				   will restore remaining registers after returning from exception): */
				LDMIA	R0!,{R4-R7}
				MOV		R8,R4
				MOV		R9,R5
				MOV		R10,R6
				MOV		R11,R7
				LDMIA	R0!,{R4-R7}
				MSR		PSP,R0

				/* EXC_RETURN - Thread mode with PSP: */
				LDR		R0,=0xFFFFFFFD

				/* Enable interrupts: */
				CPSIE	I
				BX		R0

