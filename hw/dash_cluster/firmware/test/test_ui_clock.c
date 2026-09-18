/*
 * test_ui_clock.c - host tests for the clock's hands and the time announced
 * on the bus.
 *
 * A clock is the one face where a wrong reading is instantly recognisable to
 * the driver and completely invisible to a test that only checks it drew
 * something - so the hand positions are checked against times whose answers
 * are obvious, and the bus frame is checked to reject everything it should.
 */

#include <stdio.h>
#include <string.h>

#include "clock_link.h"
#include "ui_clock.h"
#include "ui_model.h"

extern int ClockTests_Run(int *Checks, int *Failures);

static int Checks;
static int Failures;

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

/* Within a thousandth of the face - a third of a pixel at the hand's tip. */
#define NEAR(a, b)	(((a) > (b) ? (a) - (b) : (b) - (a)) <= 1)


/***************************************************************************************/
static void TestHands(void)
{
	UiClockHands_t H;

	H = UiClock_Hands(12u, 0u, 0u, 0u);
	CHECK(H.Hour == 0u && H.Minute == 0u && H.Second == 0u,
	      "noon puts every hand at the top: %u %u %u", H.Hour, H.Minute, H.Second);

	H = UiClock_Hands(3u, 0u, 0u, 0u);
	CHECK(NEAR(H.Hour, UI_POSITION_MAX / 4u), "three o'clock is a quarter round: %u",
	      H.Hour);
	CHECK(H.Minute == 0u, "and its minute hand is at the top");

	H = UiClock_Hands(6u, 30u, 0u, 0u);
	CHECK(NEAR(H.Minute, UI_POSITION_MAX / 2u), "half past is half round: %u", H.Minute);
	CHECK(NEAR(H.Hour, 541u), "half past six is past the six: %u", H.Hour);

	H = UiClock_Hands(9u, 45u, 30u, 0u);
	CHECK(NEAR(H.Second, UI_POSITION_MAX / 2u), "thirty seconds is half round: %u",
	      H.Second);
	CHECK(NEAR(H.Minute, 758u), "the minute hand moves with the seconds: %u", H.Minute);
	CHECK(NEAR(H.Hour, 814u), "and the hour hand with the minutes: %u", H.Hour);

	/* A twelve-hour face: the afternoon looks like the morning. */
	CHECK(UiClock_Hands(13u, 20u, 0u, 0u).Hour == UiClock_Hands(1u, 20u, 0u, 0u).Hour,
	      "13:20 and 1:20 put the hour hand in the same place");
	CHECK(UiClock_Hands(0u, 0u, 0u, 0u).Hour == 0u, "midnight is the top too");

	/* The fraction of a second is what makes the sweep smooth. */
	H = UiClock_Hands(0u, 0u, 30u, UI_CLOCK_FRAC_ONE / 2u);
	CHECK(NEAR(H.Second, 508u), "half a second past thirty: %u", H.Second);
	CHECK(UiClock_Hands(0u, 0u, 59u, UI_CLOCK_FRAC_ONE - 1u).Second < UI_POSITION_MAX,
	      "the last instant of the minute has not wrapped yet");
}


/***************************************************************************************/
static void TestFormat(void)
{
	char Text[8];

	UiClock_Format(true, 21u, 47u, Text, sizeof(Text));
	CHECK(strcmp(Text, "21:47") == 0, "21:47, got %s", Text);

	UiClock_Format(true, 9u, 5u, Text, sizeof(Text));
	CHECK(strcmp(Text, "09:05") == 0, "leading zeros, got %s", Text);

	/* No time must never read as a time - a dash clock showing 00:00 would be
	   taken for midnight. */
	UiClock_Format(false, 21u, 47u, Text, sizeof(Text));
	CHECK(strcmp(Text, "--:--") == 0, "no trustworthy time reads --:--, got %s", Text);

	UiClock_Format(true, 24u, 0u, Text, sizeof(Text));
	CHECK(strcmp(Text, "--:--") == 0, "an impossible hour is not printed, got %s", Text);
}


/***************************************************************************************/
static void TestBusTime(void)
{
	uint8_t Frame[8] = { CLOCK_LINK_FLAG_VALID, 21u, 47u, 9u, 17u, 9u, 0x07u, 0xEAu };
	RtcTime_t T;
	uint32_t Rejected;

	ClockLink_Init();
	CHECK(!ClockLink_Take(&T), "nothing has been announced yet");

	CHECK(!ClockLink_Handle(0x400u, Frame, 8u), "the telemetry block is not the time");
	CHECK(ClockLink_Handle(CLOCK_LINK_TIME_ID, Frame, 8u), "the time frame is ours");
	CHECK(ClockLink_Accepted() == 1u, "and it was accepted");
	CHECK(ClockLink_Take(&T), "so there is a time waiting");
	CHECK(T.Hours == 21u && T.Minutes == 47u && T.Seconds == 9u,
	      "and it is the one sent: %02u:%02u:%02u", T.Hours, T.Minutes, T.Seconds);
	CHECK(!ClockLink_Take(&T), "taken once, not twice");

	/* Four bytes are enough: the date is optional. */
	CHECK(ClockLink_Handle(CLOCK_LINK_TIME_ID, Frame, 4u), "four bytes is a time");
	CHECK(ClockLink_Take(&T), "and it is taken");

	/* Everything that is not a time is counted and dropped. An announcer that
	   is nearly right must not be able to put a wrong time on a dash. */
	Rejected = ClockLink_Rejected();
	Frame[0] = 0u;
	CHECK(ClockLink_Handle(CLOCK_LINK_TIME_ID, Frame, 8u), "still our identifier");
	CHECK(!ClockLink_Take(&T), "a sender that says its own time is bad is ignored");

	Frame[0] = CLOCK_LINK_FLAG_VALID;
	Frame[1] = 24u;
	CHECK(ClockLink_Handle(CLOCK_LINK_TIME_ID, Frame, 8u) && !ClockLink_Take(&T),
	      "hour 24 is refused");
	Frame[1] = 21u;
	Frame[2] = 60u;
	CHECK(ClockLink_Handle(CLOCK_LINK_TIME_ID, Frame, 8u) && !ClockLink_Take(&T),
	      "minute 60 is refused");
	Frame[2] = 47u;
	Frame[3] = 60u;
	CHECK(ClockLink_Handle(CLOCK_LINK_TIME_ID, Frame, 8u) && !ClockLink_Take(&T),
	      "second 60 is refused");
	Frame[3] = 9u;
	CHECK(ClockLink_Handle(CLOCK_LINK_TIME_ID, Frame, 3u) && !ClockLink_Take(&T),
	      "three bytes is not a time");
	CHECK(ClockLink_Rejected() == Rejected + 5u,
	      "all five were counted: %lu", (unsigned long)(ClockLink_Rejected() - Rejected));

	/* And a good one still works afterwards. */
	CHECK(ClockLink_Handle(CLOCK_LINK_TIME_ID, Frame, 8u) && ClockLink_Take(&T)
	      && T.Hours == 21u, "a good frame after bad ones is accepted");
}


/***************************************************************************************/
int ClockTests_Run(int *OutChecks, int *OutFailures)
{
	Checks = 0;
	Failures = 0;

	printf("clock - hands, formatting, the time announced on the bus\n");
	TestHands();
	TestFormat();
	TestBusTime();

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
