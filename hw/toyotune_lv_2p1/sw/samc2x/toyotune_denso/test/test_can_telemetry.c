/*
 * test_can_telemetry.c
 *
 * Host tests for the telemetry frame packing.
 *
 * The packing is where the two most dangerous mistakes in this change live:
 * a signal at the wrong offset, and a 16-bit value written in the wrong byte
 * order.  Neither crashes.  Both produce a frame that decodes to plausible
 * numbers, which is exactly the failure that survives a bench test and then
 * misleads someone reading a gauge.
 *
 * So this builds can_telemetry.c on the host - included directly, so the
 * static tables and CanTelemetry_SendFrame() are reachable - feeds it a DMA
 * snapshot with known values, and asserts the bytes that would go on the wire.
 *
 * Build and run:  python test/run_tests.py
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- standing in for the target's headers --------------------------------
 *
 * can_telemetry.c pulls in os.h and can.h.  Neither is under test here, and
 * os.h in particular reaches for <sam.h> and the Cortex-M intrinsics, which
 * do not exist on a host compiler.
 *
 * Rather than stub those headers on the include path - which cannot work,
 * since a quoted #include resolves against the including file's own directory
 * first - their include guards are defined here before can_telemetry.c is
 * pulled in.  The real headers then compile to nothing and the handful of
 * declarations that matter are given below, matching the real signatures
 * exactly so a change to either one breaks this build rather than drifting.
 *
 * CAN_TxStandard is the interesting stub: it captures the frame instead of
 * transmitting it, which is how the assertions reach the payload.
 */

#define CAN_H_
#define OS_H_

#include "os_task_id.h"

typedef uint8_t OS_TaskId_t;
typedef uint16_t OS_SignalSet_t;

#define OS_SIGNAL_USER		(2)

uint32_t CAN_TxDropped = 0;
uint32_t CAN_BusOffRecoveries = 0;

static uint16_t SentId;
static uint8_t SentData[8];
static uint32_t SentLength;
static int SentCount;

static bool CAN_TxStandard(uint16_t Id, const void *Data, uint32_t DataSize)
{
	SentId = Id;
	SentLength = DataSize;
	memset(SentData, 0, sizeof(SentData));
	memcpy(SentData, Data, DataSize < sizeof(SentData) ? DataSize : sizeof(SentData));
	SentCount++;
	return true;
}

static void CAN_Poll(void) { }

static void OS_SignalSend(OS_TaskId_t TaskId, OS_SignalSet_t SignalMask)
{
	(void)TaskId; (void)SignalMask;
}

static OS_SignalSet_t OS_SignalWait(OS_SignalSet_t SignalMask)
{
	return SignalMask;
}

static void OS_TaskInit(OS_TaskId_t TaskId, void (*Handler)(void *), void *UserData,
                        void *StackVoidPtr, uint32_t StackSizeBytes)
{
	(void)TaskId; (void)Handler; (void)UserData;
	(void)StackVoidPtr; (void)StackSizeBytes;
}

#include "../can_telemetry.c"

/* Only the task loop calls this, and the tests drive CanTelemetry_SendFrame()
   directly rather than running the task - so it just has to link.  Defined
   after the include because its argument types come from ecu.h. */
bool ECU_GetDmaSnapshot(ECU_DmaData1_t *Cpu1ToCpu2, ECU_DmaData2_t *Cpu2ToCpu1)
{
	(void)Cpu1ToCpu2; (void)Cpu2ToCpu1;
	return false;
}


/***************************************************************************************/
static int Failures = 0;
static int Checks = 0;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		Checks++;                                                             \
		if (!(cond)) {                                                        \
			Failures++;                                                       \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
			printf(__VA_ARGS__);                                              \
			printf("\n");                                                     \
		}                                                                     \
	} while (0)

#define CHECK_NEAR(got, want, tol, ...)                                       \
	do {                                                                      \
		long g_ = (long)(got), w_ = (long)(want), t_ = (long)(tol);           \
		long d_ = g_ > w_ ? g_ - w_ : w_ - g_;                                \
		Checks++;                                                             \
		if (d_ > t_) {                                                        \
			Failures++;                                                       \
			printf("  FAIL %s:%d: got %ld, want %ld +/- %ld  (", __FILE__,    \
			       __LINE__, g_, w_, t_);                                     \
			printf(__VA_ARGS__);                                              \
			printf(")\n");                                                    \
		}                                                                     \
	} while (0)


/* The DMA blocks hold the ECU's own byte order - big-endian - so the test
   fixtures are built the same way, by byte.  Writing them through the struct
   fields instead would quietly hide the very byte-order bug being tested. */
static void SetBe16(void *Block, size_t Offset, uint16_t Value)
{
	uint8_t *b = (uint8_t *)Block;
	b[Offset]     = (uint8_t)(Value >> 8);
	b[Offset + 1] = (uint8_t)(Value & 0xFF);
}

static void SetU8(void *Block, size_t Offset, uint8_t Value)
{
	((uint8_t *)Block)[Offset] = Value;
}

static uint16_t GetBe16(const uint8_t *Payload, size_t Offset)
{
	return (uint16_t)(((uint16_t)Payload[Offset] << 8) | Payload[Offset + 1]);
}

static int16_t GetBe16Signed(const uint8_t *Payload, size_t Offset)
{
	return (int16_t)GetBe16(Payload, Offset);
}


static ECU_DmaData1_t Cpu1;
static ECU_DmaData2_t Cpu2;

/* A plausible running engine: 3000 rpm, atmospheric manifold pressure, 4ms
   of injector, 82 C coolant, 14.6 V, 10 degrees of knock retard. */
static void BuildSnapshot(void)
{
	memset(&Cpu1, 0, sizeof(Cpu1));
	memset(&Cpu2, 0, sizeof(Cpu2));

	SetBe16(&Cpu2, offsetof(ECU_DmaData2_t, RpmX5p12), 15360);  /* 3000 rpm */
	SetBe16(&Cpu1, offsetof(ECU_DmaData1_t, Tps), 0x1234);      /* raw */
	SetBe16(&Cpu1, offsetof(ECU_DmaData1_t, Pim2), 0x6443);     /* atmospheric */
	SetBe16(&Cpu1, offsetof(ECU_DmaData1_t, InjPwInj1), 1000);  /* 4000 us */

	SetBe16(&Cpu1, offsetof(ECU_DmaData1_t, Ect), 0xE400);      /* ~82 C */
	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, Tha), 134);           /* ~20.7 C */
	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, Tham), 134);
	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, Battery), 188);       /* ~14.6 V */

	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, KnockRetard), 20);    /* 10.00 deg */
	SetU8(&Cpu2, offsetof(ECU_DmaData2_t, IgnTiming), 0x5A);    /* raw */
	SetU8(&Cpu2, offsetof(ECU_DmaData2_t, IscvDuty), 0x40);     /* raw */
	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, AdcLambda), 0x77);    /* raw */
	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, PwLoopMode), 0xC8);   /* closed loop */

	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, KnockRetardInfo) + 0, 2);   /* 1.00 */
	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, KnockRetardInfo) + 1, 4);   /* 2.00 */
	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, KnockRetardInfo) + 2, 6);   /* 3.00 */
	SetU8(&Cpu2, offsetof(ECU_DmaData2_t, LambdaTrim), 0x33);
	SetU8(&Cpu2, offsetof(ECU_DmaData2_t, MaxRetard), 0x44);

	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, ErrorFlags1), 0xA5);
	SetU8(&Cpu1, offsetof(ECU_DmaData1_t, LimiterFlags), 0x5A);
}


/***************************************************************************************/
static void TestFastFrame(void)
{
	printf("FAST frame\n");
	CanTelemetry_SendFrame(CAN_FRAME_FAST, &Cpu1, &Cpu2);

	CHECK(SentId == TOYOTUNE_CAN_ID_FAST, "identifier should be 0x%03X, got 0x%03X",
	      TOYOTUNE_CAN_ID_FAST, SentId);
	CHECK(SentLength == 8, "length should be 8, got %u", (unsigned)SentLength);

	CHECK(GetBe16(SentData, 0) == 3000, "Rpm should be 3000, got %u",
	      GetBe16(SentData, 0));
	CHECK(GetBe16(SentData, 2) == 0x1234, "Tps passes through raw, got 0x%04X",
	      GetBe16(SentData, 2));
	CHECK_NEAR(GetBe16Signed(SentData, 4), 1013, 5,
	           "Pim2 0x6443 is atmospheric, ~101.3 kPa");
	CHECK(GetBe16(SentData, 6) == 4000, "InjPw should be 4000 us, got %u",
	      GetBe16(SentData, 6));

	/* Byte order, stated explicitly rather than inferred from the values
	   above: 3000 is 0x0BB8, so the high byte must come first. */
	CHECK(SentData[0] == 0x0B && SentData[1] == 0xB8,
	      "Rpm must be big-endian: expected 0B B8, got %02X %02X",
	      SentData[0], SentData[1]);
}


static void TestMedium1Frame(void)
{
	printf("MEDIUM1 frame - temperatures and supply\n");
	CanTelemetry_SendFrame(CAN_FRAME_MEDIUM1, &Cpu1, &Cpu2);

	CHECK(SentId == TOYOTUNE_CAN_ID_MEDIUM1, "identifier");

	CHECK_NEAR(GetBe16Signed(SentData, 0), 8179, 5, "Ect 0xE400 is ~81.8 C");
	CHECK_NEAR(GetBe16Signed(SentData, 2), 2070, 5, "Tha 134 is ~20.7 C");
	CHECK_NEAR(GetBe16Signed(SentData, 4), 2070, 5, "Tham 134 is ~20.7 C");
	CHECK_NEAR(GetBe16(SentData, 6), 1461, 3, "Battery 188 is ~14.6 V");

	/* ECT must be passed whole, not truncated to its high byte - the low
	   byte carries two real bits of ADC resolution.  0xE480 is a quarter of
	   a count hotter than 0xE400 and must decode differently. */
	SetBe16(&Cpu1, offsetof(ECU_DmaData1_t, Ect), 0xE480);
	CanTelemetry_SendFrame(CAN_FRAME_MEDIUM1, &Cpu1, &Cpu2);
	CHECK(GetBe16Signed(SentData, 0) != 8179,
	      "ECT's low byte must affect the result, or resolution is being lost");
	SetBe16(&Cpu1, offsetof(ECU_DmaData1_t, Ect), 0xE400);
}


static void TestMedium2Frame(void)
{
	printf("MEDIUM2 frame - fuelling, and the derived duty\n");
	CanTelemetry_SendFrame(CAN_FRAME_MEDIUM2, &Cpu1, &Cpu2);

	CHECK(SentId == TOYOTUNE_CAN_ID_MEDIUM2, "identifier");

	/* 4000us at 3000 rpm: the cycle is 40ms, so this is 10.00%. */
	CHECK_NEAR(GetBe16(SentData, 0), 1000, 2,
	           "derived InjDuty: 4ms at 3000 rpm is 10%%");

	CHECK(GetBe16Signed(SentData, 2) == 1000, "KnockRetard 20 counts is 10.00 deg");
	CHECK(SentData[4] == 0x5A, "IgnTiming passes through raw");
	CHECK(SentData[5] == 0x40, "IscvDuty passes through raw");
	CHECK(SentData[6] == 0x77, "AdcLambda passes through raw");
	CHECK(SentData[7] == 0xC8, "PwLoopMode passes through raw");
}


static void TestMedium3Frame(void)
{
	printf("MEDIUM3 frame - per-cylinder knock\n");
	CanTelemetry_SendFrame(CAN_FRAME_MEDIUM3, &Cpu1, &Cpu2);

	CHECK(SentId == TOYOTUNE_CAN_ID_MEDIUM3, "identifier should be base+5");

	CHECK(GetBe16Signed(SentData, 0) == 100, "cylinder 1: 2 counts is 1.00 deg");
	CHECK(GetBe16Signed(SentData, 2) == 200, "cylinder 2: 4 counts is 2.00 deg");
	CHECK(GetBe16Signed(SentData, 4) == 300, "cylinder 3: 6 counts is 3.00 deg");
	CHECK(SentData[6] == 0x33, "LambdaTrim raw");
	CHECK(SentData[7] == 0x44, "MaxRetard raw");
}


static void TestSlowFrame(void)
{
	printf("SLOW frame - trims and flags\n");
	CanTelemetry_SendFrame(CAN_FRAME_SLOW, &Cpu1, &Cpu2);

	CHECK(SentId == TOYOTUNE_CAN_ID_SLOW, "identifier");
	CHECK(SentData[3] == 0xA5, "ErrorFlags1 must pass through bit-exact");
	CHECK(SentData[7] == 0x5A, "LimiterFlags must pass through bit-exact");
}


static void TestInfoFrame(void)
{
	printf("INFO frame - version and error counters\n");

	CAN_TxDropped = 7;
	CAN_BusOffRecoveries = 3;
	CanTelemetry_SendInfo();

	CHECK(SentId == TOYOTUNE_CAN_ID_INFO, "identifier should be base+6");
	CHECK(SentData[0] == TOYOTUNE_TELEMETRY_PROTOCOL_VERSION,
	      "protocol version byte");
	CHECK(SentData[2] == 1, "CPU index should be 1 for a CPU1 build");
	CHECK(GetBe16(SentData, 4) == 7, "TxDropped");
	CHECK(GetBe16(SentData, 6) == 3, "BusOffRecoveries");

	/* Saturation: a wrapped counter would read as recovery, which is the
	   opposite of the truth. */
	CAN_TxDropped = 100000;
	CanTelemetry_SendInfo();
	CHECK(GetBe16(SentData, 4) == 0xFFFF, "TxDropped must saturate, not wrap");

	CAN_TxDropped = 0;
	CAN_BusOffRecoveries = 0;
}


/* Every row must sit inside its frame.  This walks the table rather than
   trusting the layout comments, so adding a row that overruns fails here
   instead of silently overwriting its neighbour on the bus. */
static void TestTableFitsFrames(void)
{
	uint32_t i;

	printf("signal table - every row fits its frame\n");

	for (i = 0; i < CAN_TELEMETRY_SIGNAL_COUNT; i++)
	{
		const CanTelemetry_Signal_t *S = &CanTelemetry_Signals[i];
		const CanTelemetry_XformInfo_t *X = &CanTelemetry_XformInfo[S->Xform];
		uint32_t End = (uint32_t)S->Offset + X->DstSize;

		CHECK(S->Frame < CAN_FRAME_COUNT, "row %u names frame %u", i, S->Frame);
		CHECK(S->Xform < CAN_XFORM_COUNT, "row %u names transform %u", i, S->Xform);
		CHECK(End <= CanTelemetry_Frames[S->Frame].Length,
		      "row %u ends at byte %u of a %u-byte frame",
		      i, End, CanTelemetry_Frames[S->Frame].Length);
	}
}


/* Two rows writing the same byte would mean one silently wins.  Checked per
   frame across the whole table, including the derived duty's two bytes. */
static void TestNoOverlappingSignals(void)
{
	uint8_t Used[CAN_FRAME_COUNT][8];
	uint32_t i;
	uint8_t b;

	printf("signal table - no two rows share a byte\n");
	memset(Used, 0, sizeof(Used));

	/* The derived injector duty claims its bytes without a table row. */
	Used[CAN_FRAME_MEDIUM2][CAN_MEDIUM2_INJDUTY_OFFSET] = 1;
	Used[CAN_FRAME_MEDIUM2][CAN_MEDIUM2_INJDUTY_OFFSET + 1] = 1;

	for (i = 0; i < CAN_TELEMETRY_SIGNAL_COUNT; i++)
	{
		const CanTelemetry_Signal_t *S = &CanTelemetry_Signals[i];
		const CanTelemetry_XformInfo_t *X = &CanTelemetry_XformInfo[S->Xform];

		for (b = 0; b < X->DstSize; b++)
		{
			uint8_t Byte = (uint8_t)(S->Offset + b);

			if (Byte >= 8)
				continue;	/* already reported by TestTableFitsFrames */

			CHECK(Used[S->Frame][Byte] == 0,
			      "frame %u byte %u claimed twice (row %u)", S->Frame, Byte, i);
			Used[S->Frame][Byte] = 1;
		}
	}
}


/* The periods must divide the tick, or a frame silently transmits at the
   wrong rate.  The build-time check in can_telemetry.c covers this too; this
   catches it with a readable message rather than a negative array size. */
static void TestFramePeriods(void)
{
	uint32_t i;

	printf("frame periods\n");

	for (i = 0; i < CAN_FRAME_COUNT; i++)
	{
		CHECK(CanTelemetry_Frames[i].PeriodMs % CAN_TELEMETRY_TICK_MS == 0,
		      "frame %u period %u is not a whole number of %u ms ticks",
		      i, CanTelemetry_Frames[i].PeriodMs, CAN_TELEMETRY_TICK_MS);
		CHECK(CanTelemetry_Frames[i].Length <= 8,
		      "frame %u length %u exceeds a classic CAN frame",
		      i, CanTelemetry_Frames[i].Length);
		CHECK(CanTelemetry_Frames[i].Id != 0, "frame %u has no identifier", i);
	}
}


/* Identifiers must be distinct, and must not collide with the diagnostic
   pair.  Two frames sharing an identifier cannot be separated by arbitration
   and would corrupt each other on the bus. */
static void TestIdentifiersDistinct(void)
{
	uint32_t i, j;

	printf("frame identifiers are distinct\n");

	for (i = 0; i < CAN_FRAME_COUNT; i++)
	{
		CHECK(CanTelemetry_Frames[i].Id != TOYOTUNE_CAN_ID_DIAG_CMD &&
		      CanTelemetry_Frames[i].Id != TOYOTUNE_CAN_ID_DIAG_RSP,
		      "frame %u collides with the diagnostic identifiers", i);

		for (j = i + 1; j < CAN_FRAME_COUNT; j++)
			CHECK(CanTelemetry_Frames[i].Id != CanTelemetry_Frames[j].Id,
			      "frames %u and %u share identifier 0x%03X",
			      i, j, CanTelemetry_Frames[i].Id);
	}
}


/* Print each frame as hex, for test_dbc_roundtrip.py to decode through
   toyotune.dbc.  This is the only check that closes the loop between the
   firmware's packing and the DBC every consumer decodes with: the two are
   edited in different files by different hands, and nothing else would notice
   them disagreeing until a gauge read wrong. */
static void DumpFrames(void)
{
	static const uint8_t Frames[] = {
		CAN_FRAME_FAST, CAN_FRAME_MEDIUM1, CAN_FRAME_MEDIUM2,
		CAN_FRAME_MEDIUM3, CAN_FRAME_SLOW
	};
	uint32_t f, b;

	for (f = 0; f < sizeof(Frames); f++)
	{
		CanTelemetry_SendFrame(Frames[f], &Cpu1, &Cpu2);
		printf("FRAME %u", SentId);
		for (b = 0; b < SentLength; b++)
			printf(" %02X", SentData[b]);
		printf("\n");
	}

	CAN_TxDropped = 7;
	CAN_BusOffRecoveries = 3;
	CanTelemetry_SendInfo();
	printf("FRAME %u", SentId);
	for (b = 0; b < SentLength; b++)
		printf(" %02X", SentData[b]);
	printf("\n");
}


/***************************************************************************************/
int main(int argc, char **argv)
{
	if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'd')
	{
		BuildSnapshot();
		DumpFrames();
		return EXIT_SUCCESS;
	}

	printf("can_telemetry packing tests\n");
	printf("---------------------------\n");

	BuildSnapshot();

	TestFramePeriods();
	TestIdentifiersDistinct();
	TestTableFitsFrames();
	TestNoOverlappingSignals();
	TestFastFrame();
	TestMedium1Frame();
	TestMedium2Frame();
	TestMedium3Frame();
	TestSlowFrame();
	TestInfoFrame();

	printf("---------------------------\n");
	printf("%d checks, %d failures\n", Checks, Failures);
	return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
