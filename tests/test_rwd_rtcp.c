#include <stdint.h>

#include "greatest.h"
#include "../rwd/rwd_rtcp.h"

TEST rwd_rtcp_accepts_pli_and_fir(void)
{
	uint8_t pli[12] = {0x81, 206};
	uint8_t fir[12] = {0x84, 206};

	ASSERT(rwd_rtcp_requests_keyframe(pli, sizeof(pli)));
	ASSERT(rwd_rtcp_requests_keyframe(fir, sizeof(fir)));
	PASS();
}

TEST rwd_rtcp_rejects_rtp_and_other_rtcp(void)
{
	uint8_t rtp[12] = {0x80, 96};
	uint8_t receiver_report[12] = {0x80, 201};
	uint8_t short_pli[8] = {0x81, 206};

	ASSERT_FALSE(rwd_rtcp_requests_keyframe(rtp, sizeof(rtp)));
	ASSERT_FALSE(rwd_rtcp_requests_keyframe(receiver_report, sizeof(receiver_report)));
	ASSERT_FALSE(rwd_rtcp_requests_keyframe(short_pli, sizeof(short_pli)));
	ASSERT_FALSE(rwd_rtcp_requests_keyframe(NULL, 0));
	PASS();
}

SUITE(rwd_rtcp_suite)
{
	RUN_TEST(rwd_rtcp_accepts_pli_and_fir);
	RUN_TEST(rwd_rtcp_rejects_rtp_and_other_rtcp);
}
