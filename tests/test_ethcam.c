#include "greatest.h"
#include "rcd_ethcam.h"
#include "rcd_tsmap.h"

#include <string.h>

static const uint8_t SPS[] = {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x33, 0xac, 0x15};
static const uint8_t PPS[] = {0, 0, 0, 1, 0x68, 0xee, 0x3c, 0xb0};

/* One record as the module sends it; body = start code + nal byte + filler */
static size_t make_record(uint8_t *dst, uint64_t ts, uint32_t body_len, int gop_idx, int nal_byte,
			  bool prefix)
{
	size_t o = 0;
	if (prefix) {
		dst[o++] = sizeof(SPS) & 0xff;
		dst[o++] = 0;
		memcpy(dst + o, SPS, sizeof(SPS));
		o += sizeof(SPS);
		dst[o++] = sizeof(PPS) & 0xff;
		dst[o++] = 0;
		memcpy(dst + o, PPS, sizeof(PPS));
		o += sizeof(PPS);
	}
	uint8_t *hd = dst + o, *body = hd + 16;
	body[0] = body[1] = body[2] = 0;
	body[3] = 1;
	body[4] = (uint8_t)nal_byte;
	for (uint32_t i = 5; i < body_len; i++)
		body[i] = (uint8_t)(i * 13);
	for (int i = 0; i < 8; i++)
		hd[i] = (uint8_t)(ts >> (56 - 8 * i));
	hd[8] = body_len & 0xff;
	hd[9] = (body_len >> 8) & 0xff;
	hd[10] = (body_len >> 16) & 0x0f;
	hd[11] = (uint8_t)gop_idx;
	uint8_t ca = 0, cb = 0;
	if (body_len > 64) {
		for (uint32_t i = 0; i < 64; i++) {
			ca = (uint8_t)(ca + body[i]);
			cb = (uint8_t)(cb + body[body_len - 64 + i]);
		}
	}
	hd[12] = ca;
	hd[13] = cb;
	hd[14] = 0;
	hd[15] = 2;
	return o + 16 + body_len;
}

static uint8_t buf[1 << 20];

TEST ethcam_cmd_has_nul(void)
{
	char b[96];
	int n = ethcam_build_cmd(b, sizeof(b), "192.168.0.12", 8887, 9008, 0);
	ASSERT_EQ((int)strlen("192.168.0.12:8887/?custom=1&cmd=9008&par=0") + 1, n);
	ASSERT_STR_EQ("192.168.0.12:8887/?custom=1&cmd=9008&par=0", b);
	ASSERT_EQ('\0', b[n - 1]);
	ASSERT_EQ(-1, ethcam_build_cmd(b, 10, "192.168.0.12", 8887, 9008, 0));
	PASS();
}

TEST ethcam_reply_function_and_list(void)
{
	const char *two = "192.168.0.12:8899<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
			  "<Function>\n<Cmd>9025</Cmd>\n<Status>0</Status>\n"
			  "<String>2019-12-12-Ver1.00</String>\n</Function>"
			  "192.168.0.12:8887<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
			  "<LIST>\n<Cmd>9012</Cmd>\n<ITEM>2560</ITEM>\n<ITEM>1440</ITEM>\n"
			  "<ITEM>30</ITEM>\n</LIST>\n";
	ethcam_reply_t r;
	size_t len = strlen(two);

	/* Every cut short of the first reply's end asks for more data */
	const char *first_end = strstr(two, "</Function>") + strlen("</Function>");
	for (size_t cut = 0; cut < (size_t)(first_end - two); cut++)
		ASSERT_EQ(0, ethcam_parse_reply(two, cut, &r));

	int n = ethcam_parse_reply(two, len, &r);
	ASSERT_EQ((int)(first_end - two), n);
	ASSERT_EQ(9025, r.cmd);
	ASSERT_EQ(0, r.status);
	ASSERT_STR_EQ("2019-12-12-Ver1.00", r.string);
	ASSERT_EQ(0, r.item_count);

	int m = ethcam_parse_reply(two + n, len - (size_t)n, &r);
	ASSERT_EQ((int)(len - (size_t)n), m);
	ASSERT_EQ(9012, r.cmd);
	ASSERT_EQ(INT32_MIN, r.status);
	ASSERT_EQ(3, r.item_count);
	ASSERT_EQ(2560, r.items[0]);
	ASSERT_EQ(30, r.items[2]);
	PASS();
}

TEST ethcam_frame_h264_i_and_p(void)
{
	/* Header values from the first capture of the bench module */
	size_t a = make_record(buf, 26087836, 305178, 1, 0x65, true);
	size_t b = make_record(buf + a, 26121172, 41145, 2, 0x41, false);
	ethcam_frame_t f;

	int n = ethcam_parse_frame(buf, a + b, false, &f);
	ASSERT_EQ((int)a, n);
	ASSERT_EQ(sizeof(SPS), f.sps_len);
	ASSERT_EQ(0, memcmp(f.sps, SPS, sizeof(SPS)));
	ASSERT_EQ(sizeof(PPS), f.pps_len);
	ASSERT_EQ(305178u, f.body_len);
	ASSERT_EQ(26087836u, f.ts_us);
	ASSERT(f.key);
	ASSERT(f.chk_ok);
	ASSERT_EQ(5, f.nal);

	n = ethcam_parse_frame(buf + a, b, false, &f);
	ASSERT_EQ((int)b, n);
	ASSERT_EQ(NULL, f.sps);
	ASSERT_FALSE(f.key);
	ASSERT_EQ(1, f.nal);
	ASSERT_EQ(33336u, f.ts_us - 26087836u);
	PASS();
}

TEST ethcam_frame_partial_needs_more(void)
{
	size_t a = make_record(buf, 1000, 300, 1, 0x65, true);
	ethcam_frame_t f;
	for (size_t cut = 0; cut < a; cut++)
		ASSERT_EQ_FMT(0, ethcam_parse_frame(buf, cut, false, &f), "%d");
	ASSERT_EQ((int)a, ethcam_parse_frame(buf, a, false, &f));
	PASS();
}

TEST ethcam_frame_h265_inline_params(void)
{
	/* H.265 I: no prefix; VPS (type 32) leads the body */
	size_t a = make_record(buf, 5000, 2000, 1, 32 << 1, false);
	ethcam_frame_t f;
	ASSERT_EQ((int)a, ethcam_parse_frame(buf, a, true, &f));
	ASSERT(f.key);
	ASSERT_EQ(32, f.nal);
	ASSERT_EQ(NULL, f.sps);
	PASS();
}

TEST ethcam_frame_desync_and_checksum(void)
{
	size_t a = make_record(buf, 7, 500, 2, 0x41, false);
	ethcam_frame_t f;

	buf[15] = 0; /* header type must be 2 */
	ASSERT_EQ(-1, ethcam_parse_frame(buf, a, false, &f));
	buf[15] = 2;
	buf[16] = 5; /* body must start with a start code */
	ASSERT_EQ(-1, ethcam_parse_frame(buf, a, false, &f));
	buf[16] = 0;
	buf[16 + 499] ^= 0xff; /* last 64 bytes no longer match checksum B */
	ASSERT_EQ((int)a, ethcam_parse_frame(buf, a, false, &f));
	ASSERT_FALSE(f.chk_ok);
	PASS();
}

TEST rcd_tsmap_tracks_lowest_delay(void)
{
	rcd_tsmap_t m = {0};
	/* module clock starts at 1 s; local clock at 100 s; 5 ms transit */
	int64_t out = rcd_tsmap(&m, 1000000, 100005000);
	ASSERT_EQ(100005000, out);
	/* a frame that arrives 3 ms faster pulls the mapping down at once */
	out = rcd_tsmap(&m, 1033333, 100035333);
	ASSERT_EQ(1033333 + 99002000, out);
	/* a slow frame does not move it */
	out = rcd_tsmap(&m, 1066666, 100090000);
	ASSERT_EQ(1066666 + 99002000, out);
	PASS();
}

TEST ethcam_tsmap_slews_and_stays_monotonic(void)
{
	rcd_tsmap_t m = {0};
	int64_t prev = 0;
	/* Module clock runs 1000 ppm slow: transit grows 1 ms per second.
	 * The mapping may only creep up 2 ms per 10 s window, output never repeats. */
	for (int i = 0; i < 30 * 30; i++) {
		uint64_t mod = 1000000 + (uint64_t)i * 33333;
		int64_t local = 50000000 + (int64_t)i * 33333 + (int64_t)i * 33333 / 1000;
		int64_t out = rcd_tsmap(&m, mod, local);
		ASSERT(out > prev);
		ASSERT(out <= local);
		prev = out;
	}
	/* window 1 still holds the start-up delay; window 2 slews one step */
	ASSERT_EQ(49000000 + 2000, m.offset);
	ASSERT_EQ(0, m.resets);

	/* module reboot: its clock restarts; counted, output keeps going forward */
	int64_t out = rcd_tsmap(&m, 500000, prev + 1000);
	ASSERT_EQ(1, m.resets);
	ASSERT(out > prev);
	PASS();
}

TEST rcd_seq_stale_startup_stamps(void)
{
	/* An EthCam module after stream start: three frames carry the same
	 * stamp, the next jumps 100 ms, then a steady 33.33 ms. No frame is
	 * missing, so the sequence has no gap and never goes backward. */
	rcd_seq_t q = {0};
	const uint64_t ts[] = {27177600, 27177600, 27177600, 27277600, 27310933, 27344266};
	uint64_t eff[6];
	for (int i = 0; i < 6; i++) {
		uint32_t seq = rcd_seq(&q, ts[i], 33333, &eff[i]);
		ASSERT_EQ_FMT((uint32_t)i, seq, "%u");
		if (i)
			ASSERT(eff[i] > eff[i - 1]);
	}
	ASSERT_EQ(0u, q.resets);
	PASS();
}

TEST rcd_seq_gaps_and_reboots(void)
{
	rcd_seq_t q = {0};
	uint64_t eff;
	ASSERT_EQ(0u, rcd_seq(&q, 1000000, 33333, &eff));
	ASSERT_EQ(1u, rcd_seq(&q, 1033333, 33333, &eff));
	/* two frames never arrived (a reconnect): the gap stays visible */
	ASSERT_EQ(4u, rcd_seq(&q, 1133333, 33333, &eff));
	ASSERT_EQ(0u, q.resets);
	/* the camera rebooted: its clock restarts; counted, sequence moves on */
	uint32_t s = rcd_seq(&q, 2000, 33333, &eff);
	ASSERT_EQ(1u, q.resets);
	ASSERT(s > 4);
	ASSERT_EQ(2000u, eff);
	PASS();
}

SUITE(ethcam_suite)
{
	RUN_TEST(ethcam_cmd_has_nul);
	RUN_TEST(ethcam_reply_function_and_list);
	RUN_TEST(ethcam_frame_h264_i_and_p);
	RUN_TEST(ethcam_frame_partial_needs_more);
	RUN_TEST(ethcam_frame_h265_inline_params);
	RUN_TEST(ethcam_frame_desync_and_checksum);
	RUN_TEST(rcd_tsmap_tracks_lowest_delay);
	RUN_TEST(ethcam_tsmap_slews_and_stays_monotonic);
	RUN_TEST(rcd_seq_stale_startup_stamps);
	RUN_TEST(rcd_seq_gaps_and_reboots);
}
