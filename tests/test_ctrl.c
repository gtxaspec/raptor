/*
 * test_ctrl.c -- Unit tests for rvd_ctrl_handler
 *
 * Stateful mock HAL records what the ctrl handler passes to HAL ops
 * and can be configured to return failure, so we can verify:
 *  - enc_cfg updated only on HAL success, not on failure
 *  - config written only on HAL success
 *  - correct HW channel (streams[ch].chn) passed, not logical index
 *  - multi-stream isolation
 *  - table-driven enc-set/get type dispatch (int/u32/bool roundtrip)
 *  - WB partial merge logic
 *  - privacy toggle semantics
 *  - IVS gating (commands rejected when inactive)
 */

#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include "greatest.h"
#include "../rvd/rvd.h"

/* ── Recording HAL layer ──
 *
 * Wraps specific HAL ops to record what the ctrl handler passed
 * and to return a configurable error code.
 */

static struct {
	int last_chn;
	int call_count;
	int return_val;

	/* Last values received by setters */
	uint32_t set_bitrate;
	uint32_t set_gop;
	uint32_t set_fps_num;
	uint32_t set_fps_den;
	int set_min_qp;
	int set_max_qp;
	rss_rc_mode_t set_rc_mode;
	uint32_t set_rc_bitrate;

	/* Table-driven: last enc-set values by type */
	int set_int_val;
	uint32_t set_u32_val;
	bool set_bool_val;

	/* Stored values for enc-get roundtrip */
	int stored_gop_mode;
	uint32_t stored_rc_options;
	bool stored_color2grey;

	/* ISP */
	int set_brightness;
	int brightness_stored;
} rec;

static void rec_reset(void)
{
	memset(&rec, 0, sizeof(rec));
}

static int rec_enc_set_bitrate(void *ctx, int chn, uint32_t bitrate)
{
	(void)ctx;
	rec.last_chn = chn;
	rec.set_bitrate = bitrate;
	rec.call_count++;
	return rec.return_val;
}

static int rec_enc_set_gop(void *ctx, int chn, uint32_t gop)
{
	(void)ctx;
	rec.last_chn = chn;
	rec.set_gop = gop;
	rec.call_count++;
	return rec.return_val;
}

static int rec_enc_set_fps(void *ctx, int chn, uint32_t num, uint32_t den)
{
	(void)ctx;
	rec.last_chn = chn;
	rec.set_fps_num = num;
	rec.set_fps_den = den;
	rec.call_count++;
	return rec.return_val;
}

static int rec_enc_set_qp_bounds(void *ctx, int chn, int min_qp, int max_qp)
{
	(void)ctx;
	rec.last_chn = chn;
	rec.set_min_qp = min_qp;
	rec.set_max_qp = max_qp;
	rec.call_count++;
	return rec.return_val;
}

static int rec_enc_set_rc_mode(void *ctx, int chn, rss_rc_mode_t mode, uint32_t bitrate)
{
	(void)ctx;
	rec.last_chn = chn;
	rec.set_rc_mode = mode;
	rec.set_rc_bitrate = bitrate;
	rec.call_count++;
	return rec.return_val;
}

/* Table-driven type dispatch recorders */
static int rec_enc_set_gop_mode(void *ctx, int chn, int val)
{
	(void)ctx;
	(void)chn;
	rec.set_int_val = val;
	rec.stored_gop_mode = val;
	rec.call_count++;
	return rec.return_val;
}

static int rec_enc_get_gop_mode(void *ctx, int chn, int *out)
{
	(void)ctx;
	(void)chn;
	*out = rec.stored_gop_mode;
	return 0;
}

static int rec_enc_set_rc_options(void *ctx, int chn, uint32_t val)
{
	(void)ctx;
	(void)chn;
	rec.set_u32_val = val;
	rec.stored_rc_options = val;
	rec.call_count++;
	return rec.return_val;
}

static int rec_enc_get_rc_options(void *ctx, int chn, uint32_t *out)
{
	(void)ctx;
	(void)chn;
	*out = rec.stored_rc_options;
	return 0;
}

static int rec_enc_set_color2grey(void *ctx, int chn, bool val)
{
	(void)ctx;
	(void)chn;
	rec.set_bool_val = val;
	rec.stored_color2grey = val;
	rec.call_count++;
	return rec.return_val;
}

static int rec_enc_get_color2grey(void *ctx, int chn, bool *out)
{
	(void)ctx;
	(void)chn;
	*out = rec.stored_color2grey;
	return 0;
}

static int rec_isp_set_brightness(void *ctx, int val)
{
	(void)ctx;
	rec.set_brightness = val;
	rec.brightness_stored = val;
	rec.call_count++;
	return rec.return_val;
}

static int rec_isp_get_brightness(void *ctx, uint8_t *val)
{
	(void)ctx;
	*val = (uint8_t)rec.brightness_stored;
	return 0;
}

/* WB mock that preserves state for merge testing */
static rss_wb_config_t wb_state;

static int rec_isp_get_wb(void *ctx, rss_wb_config_t *wb)
{
	(void)ctx;
	*wb = wb_state;
	return 0;
}

static int rec_isp_set_wb(void *ctx, rss_wb_config_t *wb)
{
	(void)ctx;
	wb_state = *wb;
	rec.call_count++;
	return rec.return_val;
}

/* ── Stubs for pipeline/OSD/IVS functions ── */

int rvd_stream_init(rvd_state_t *st, int idx)
{
	(void)st;
	(void)idx;
	return RSS_OK;
}

void rvd_stream_deinit(rvd_state_t *st, int idx)
{
	(void)st;
	(void)idx;
}

int rvd_stream_start(rvd_state_t *st, int idx)
{
	(void)st;
	(void)idx;
	return RSS_OK;
}

void rvd_stream_stop(rvd_state_t *st, int idx)
{
	(void)st;
	(void)idx;
}

void rvd_osd_set_privacy(rvd_state_t *st, bool enable, int stream)
{
	if (stream >= 0 && stream < st->stream_count) {
		st->privacy[stream] = enable;
	} else {
		for (int i = 0; i < st->stream_count; i++)
			st->privacy[i] = enable;
	}
}

void rvd_osd_calc_position(int sw, int sh, int rw, int rh, const char *p, int *x, int *y)
{
	(void)sw;
	(void)sh;
	(void)rw;
	(void)rh;
	(void)p;
	*x = 0;
	*y = 0;
}

rvd_osd_region_t *rvd_osd_find_region(rvd_state_t *st, int s, const char *n)
{
	(void)st;
	(void)s;
	(void)n;
	return NULL;
}

void rvd_ivs_pause(rvd_state_t *st)
{
	(void)st;
}

void rvd_ivs_resume(rvd_state_t *st)
{
	(void)st;
}

void *rvd_ivs_thread(void *arg)
{
	(void)arg;
	return NULL;
}

/* ── Test fixture ── */

static rvd_state_t st;
static rss_hal_ctx_t *test_hal;
static rss_config_t *test_cfg;
static rss_hal_ops_t ops; /* mutable copy — tests patch individual fn ptrs */
static char resp[4096];

static void setup(void)
{
	memset(&st, 0, sizeof(st));
	memset(resp, 0, sizeof(resp));
	rec_reset();

	test_hal = rss_hal_create();
	st.hal_ctx = test_hal;

	/* Mutable copy of mock ops — tests can patch individual entries */
	ops = *rss_hal_get_ops(test_hal);

	/* Install recording ops for the functions we actually test */
	ops.enc_set_bitrate = (void *)rec_enc_set_bitrate;
	ops.enc_set_gop = (void *)rec_enc_set_gop;
	ops.enc_set_fps = (void *)rec_enc_set_fps;
	ops.enc_set_qp_bounds = (void *)rec_enc_set_qp_bounds;
	ops.enc_set_rc_mode = (void *)rec_enc_set_rc_mode;
	ops.enc_set_gop_mode = (void *)rec_enc_set_gop_mode;
	ops.enc_get_gop_mode = (void *)rec_enc_get_gop_mode;
	ops.enc_set_rc_options = (void *)rec_enc_set_rc_options;
	ops.enc_get_rc_options = (void *)rec_enc_get_rc_options;
	ops.enc_set_color2grey = (void *)rec_enc_set_color2grey;
	ops.enc_get_color2grey = (void *)rec_enc_get_color2grey;
	ops.isp_set_brightness = (void *)rec_isp_set_brightness;
	ops.isp_get_brightness = (void *)rec_isp_get_brightness;
	ops.isp_get_wb = rec_isp_get_wb;
	ops.isp_set_wb = (void *)rec_isp_set_wb;

	st.ops = &ops;
	st.config_path = "/dev/null";
	test_cfg = rss_config_load("/dev/null");
	st.cfg = test_cfg;

	pthread_mutex_init(&st.osd_lock, NULL);
	pthread_mutex_init(&st.ivs_det_lock, NULL);

	/* Stream 0: main, hw channel 0 */
	st.stream_count = 3;
	st.streams[0].chn = 0;
	st.streams[0].fs_chn = 0;
	st.streams[0].enc_cfg.codec = RSS_CODEC_H264;
	st.streams[0].enc_cfg.width = 1920;
	st.streams[0].enc_cfg.height = 1080;
	st.streams[0].enc_cfg.bitrate = 2000000;
	st.streams[0].enc_cfg.gop_length = 50;
	st.streams[0].enc_cfg.fps_num = 25;
	st.streams[0].enc_cfg.min_qp = 15;
	st.streams[0].enc_cfg.max_qp = 45;
	st.streams[0].enc_cfg.rc_mode = RSS_RC_CBR;
	st.streams[0].is_jpeg = false;
	snprintf(st.streams[0].cfg_sect, sizeof(st.streams[0].cfg_sect), "stream0");

	/* Stream 1: sub, hw channel 3 (different from logical index!) */
	st.streams[1].chn = 3;
	st.streams[1].fs_chn = 1;
	st.streams[1].enc_cfg.codec = RSS_CODEC_H264;
	st.streams[1].enc_cfg.width = 640;
	st.streams[1].enc_cfg.height = 360;
	st.streams[1].enc_cfg.bitrate = 500000;
	st.streams[1].enc_cfg.gop_length = 30;
	st.streams[1].enc_cfg.fps_num = 15;
	st.streams[1].enc_cfg.min_qp = 20;
	st.streams[1].enc_cfg.max_qp = 51;
	st.streams[1].enc_cfg.rc_mode = RSS_RC_VBR;
	st.streams[1].is_jpeg = false;
	snprintf(st.streams[1].cfg_sect, sizeof(st.streams[1].cfg_sect), "stream1");

	/* Stream 2: JPEG snapshot channel */
	st.streams[2].chn = 2;
	st.streams[2].fs_chn = 0;
	st.streams[2].is_jpeg = true;
	snprintf(st.streams[2].cfg_sect, sizeof(st.streams[2].cfg_sect), "jpeg0");

	/* WB initial state */
	wb_state = (rss_wb_config_t){
		.mode = RSS_WB_AUTO, .r_gain = 256, .g_gain = 256, .b_gain = 256};
	rec.brightness_stored = 128;
}

static void teardown(void)
{
	rss_config_free(test_cfg);
	rss_hal_destroy(test_hal);
	pthread_mutex_destroy(&st.osd_lock);
	pthread_mutex_destroy(&st.ivs_det_lock);
}

static int call(const char *json)
{
	memset(resp, 0, sizeof(resp));
	return rvd_ctrl_handler(json, resp, sizeof(resp), &st);
}

/* ══════════════════════════════════════════════════════════════════
 *  State mutation: enc_cfg updated only on HAL success
 * ══════════════════════════════════════════════════════════════════ */

TEST set_bitrate_updates_state_on_success(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":3000000}");
	ASSERT_EQ(3000000, (int)st.streams[0].enc_cfg.bitrate);
	ASSERT_EQ(3000000, rss_config_get_int(st.cfg, "stream0", "bitrate", 0));
	teardown();
	PASS();
}

TEST set_bitrate_no_state_change_on_hal_failure(void)
{
	setup();
	rec.return_val = -1;
	call("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":9999}");
	ASSERT_EQ(2000000, (int)st.streams[0].enc_cfg.bitrate);
	ASSERT_EQ(0, rss_config_get_int(st.cfg, "stream0", "bitrate", 0));
	ASSERT(strstr(resp, "\"error\"") != NULL);
	teardown();
	PASS();
}

TEST set_gop_no_state_change_on_hal_failure(void)
{
	setup();
	rec.return_val = -1;
	call("{\"cmd\":\"set-gop\",\"channel\":0,\"value\":999}");
	ASSERT_EQ(50, (int)st.streams[0].enc_cfg.gop_length);
	teardown();
	PASS();
}

TEST set_fps_updates_state_on_success(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-fps\",\"channel\":0,\"value\":30}");
	ASSERT_EQ(30, (int)st.streams[0].enc_cfg.fps_num);
	ASSERT_EQ(30, rss_config_get_int(st.cfg, "stream0", "fps", 0));
	teardown();
	PASS();
}

TEST set_qp_bounds_atomic_update(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-qp-bounds\",\"channel\":0,\"min\":5,\"max\":48}");
	ASSERT_EQ(5, st.streams[0].enc_cfg.min_qp);
	ASSERT_EQ(48, st.streams[0].enc_cfg.max_qp);
	teardown();
	PASS();
}

TEST set_qp_bounds_neither_updated_on_failure(void)
{
	setup();
	rec.return_val = -1;
	call("{\"cmd\":\"set-qp-bounds\",\"channel\":0,\"min\":1,\"max\":51}");
	/* Both must remain at original values */
	ASSERT_EQ(15, st.streams[0].enc_cfg.min_qp);
	ASSERT_EQ(45, st.streams[0].enc_cfg.max_qp);
	teardown();
	PASS();
}

TEST set_rc_mode_stores_enum_not_string(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-rc-mode\",\"channel\":0,\"mode\":\"smart\"}");
	ASSERT_EQ(RSS_RC_SMART, st.streams[0].enc_cfg.rc_mode);
	teardown();
	PASS();
}

TEST set_rc_mode_persists_string(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-rc-mode\",\"channel\":0,\"mode\":\"capped_vbr\"}");
	char buf[32] = "";
	const char *v = rss_config_get_str(st.cfg, "stream0", "rc_mode", "");
	ASSERT_STR_EQ("capped_vbr", v);
	(void)buf;
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  HW channel mapping: streams[ch].chn passed, not logical index
 * ══════════════════════════════════════════════════════════════════ */

TEST set_bitrate_passes_hw_channel(void)
{
	setup();
	rec.return_val = 0;
	/* Stream 1 has chn=3, not 1 */
	call("{\"cmd\":\"set-bitrate\",\"channel\":1,\"value\":1000000}");
	ASSERT_EQ(3, rec.last_chn);
	teardown();
	PASS();
}

TEST set_gop_passes_hw_channel(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-gop\",\"channel\":1,\"value\":60}");
	ASSERT_EQ(3, rec.last_chn);
	teardown();
	PASS();
}

TEST set_rc_mode_passes_hw_channel_and_bitrate(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-rc-mode\",\"channel\":1,\"mode\":\"cbr\",\"bitrate\":800000}");
	ASSERT_EQ(3, rec.last_chn);
	ASSERT_EQ(800000, (int)rec.set_rc_bitrate);
	ASSERT_EQ(RSS_RC_CBR, rec.set_rc_mode);
	teardown();
	PASS();
}

TEST set_rc_mode_uses_current_bitrate_when_not_specified(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-rc-mode\",\"channel\":1,\"mode\":\"vbr\"}");
	/* Should use streams[1].enc_cfg.bitrate = 500000 */
	ASSERT_EQ(500000, (int)rec.set_rc_bitrate);
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  Multi-stream isolation
 * ══════════════════════════════════════════════════════════════════ */

TEST set_bitrate_ch0_doesnt_affect_ch1(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-bitrate\",\"channel\":0,\"value\":9000000}");
	ASSERT_EQ(9000000, (int)st.streams[0].enc_cfg.bitrate);
	ASSERT_EQ(500000, (int)st.streams[1].enc_cfg.bitrate);
	teardown();
	PASS();
}

TEST config_written_to_correct_section(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-bitrate\",\"channel\":1,\"value\":777000}");
	/* Must write to "stream1", not "stream0" */
	ASSERT_EQ(0, rss_config_get_int(st.cfg, "stream0", "bitrate", 0));
	ASSERT_EQ(777000, rss_config_get_int(st.cfg, "stream1", "bitrate", 0));
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  Table-driven enc-set/enc-get: type dispatch + roundtrip
 * ══════════════════════════════════════════════════════════════════ */

TEST enc_set_int_value_reaches_hal(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"enc-set\",\"channel\":0,\"param\":\"gop_mode\",\"value\":2}");
	ASSERT_EQ(2, rec.set_int_val);
	ASSERT(strstr(resp, "\"ok\"") != NULL);
	teardown();
	PASS();
}

TEST enc_set_u32_value_reaches_hal(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"enc-set\",\"channel\":0,\"param\":\"rc_options\",\"value\":42}");
	ASSERT_EQ(42, (int)rec.set_u32_val);
	teardown();
	PASS();
}

TEST enc_set_bool_value_reaches_hal(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"enc-set\",\"channel\":0,\"param\":\"color2grey\",\"value\":1}");
	ASSERT_EQ(true, rec.set_bool_val);
	teardown();
	PASS();
}

TEST enc_get_int_roundtrip(void)
{
	setup();
	rec.return_val = 0;
	rec.stored_gop_mode = 0;
	call("{\"cmd\":\"enc-set\",\"channel\":0,\"param\":\"gop_mode\",\"value\":2}");
	call("{\"cmd\":\"enc-get\",\"channel\":0,\"param\":\"gop_mode\"}");
	ASSERT(strstr(resp, "\"value\":2") != NULL);
	teardown();
	PASS();
}

TEST enc_get_bool_roundtrip(void)
{
	setup();
	rec.return_val = 0;
	rec.stored_color2grey = false;
	call("{\"cmd\":\"enc-set\",\"channel\":0,\"param\":\"color2grey\",\"value\":1}");
	call("{\"cmd\":\"enc-get\",\"channel\":0,\"param\":\"color2grey\"}");
	ASSERT(strstr(resp, "\"value\":true") != NULL);
	teardown();
	PASS();
}

TEST enc_set_null_fn_ptr_returns_not_supported(void)
{
	setup();
	ops.enc_set_gop_mode = NULL;
	call("{\"cmd\":\"enc-set\",\"channel\":0,\"param\":\"gop_mode\",\"value\":1}");
	ASSERT(strstr(resp, "not supported") != NULL);
	ASSERT_EQ(0, rec.call_count);
	teardown();
	PASS();
}

TEST enc_get_set_only_param_returns_no_getter(void)
{
	setup();
	call("{\"cmd\":\"enc-get\",\"channel\":0,\"param\":\"qp\"}");
	ASSERT(strstr(resp, "no getter") != NULL);
	teardown();
	PASS();
}

TEST enc_set_unknown_param(void)
{
	setup();
	call("{\"cmd\":\"enc-set\",\"channel\":0,\"param\":\"bogus\",\"value\":1}");
	ASSERT(strstr(resp, "unknown param") != NULL);
	teardown();
	PASS();
}

TEST enc_list_has_all_params_with_types(void)
{
	setup();
	call("{\"cmd\":\"enc-list\"}");
	/* Verify the 3 type strings appear (proves type field is populated) */
	ASSERT(strstr(resp, "\"type\":\"int\"") != NULL);
	ASSERT(strstr(resp, "\"type\":\"uint\"") != NULL);
	ASSERT(strstr(resp, "\"type\":\"bool\"") != NULL);
	/* Verify set/get booleans present */
	ASSERT(strstr(resp, "\"set\":true") != NULL);
	ASSERT(strstr(resp, "\"get\":true") != NULL);
	ASSERT(strstr(resp, "\"get\":false") != NULL); /* set-only params */
	teardown();
	PASS();
}

TEST enc_list_with_channel_includes_values(void)
{
	setup();
	rec.stored_gop_mode = 7;
	rec.stored_color2grey = true;
	call("{\"cmd\":\"enc-list\",\"channel\":0}");
	/* Params with getters should have "value" populated from HAL */
	ASSERT(strstr(resp, "\"value\":7") != NULL);
	ASSERT(strstr(resp, "\"value\":true") != NULL);
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  WB partial merge: set-wb reads current, merges, writes back
 * ══════════════════════════════════════════════════════════════════ */

TEST set_wb_mode_only_preserves_gains(void)
{
	setup();
	wb_state.r_gain = 300;
	wb_state.b_gain = 200;
	rec.return_val = 0;
	call("{\"cmd\":\"set-wb\",\"mode\":\"daylight\"}");
	ASSERT_EQ(RSS_WB_DAYLIGHT, wb_state.mode);
	ASSERT_EQ(300, wb_state.r_gain);
	ASSERT_EQ(200, wb_state.b_gain);
	teardown();
	PASS();
}

TEST set_wb_gain_only_preserves_mode(void)
{
	setup();
	wb_state.mode = RSS_WB_MANUAL;
	rec.return_val = 0;
	call("{\"cmd\":\"set-wb\",\"r_gain\":400}");
	ASSERT_EQ(RSS_WB_MANUAL, wb_state.mode);
	ASSERT_EQ(400, wb_state.r_gain);
	ASSERT_EQ(256, wb_state.b_gain); /* unchanged */
	teardown();
	PASS();
}

TEST get_wb_returns_current_state(void)
{
	setup();
	wb_state.mode = RSS_WB_CLOUDY;
	wb_state.r_gain = 111;
	wb_state.g_gain = 222;
	wb_state.b_gain = 333;
	call("{\"cmd\":\"get-wb\"}");
	ASSERT(strstr(resp, "\"mode\":\"cloudy\"") != NULL);
	ASSERT(strstr(resp, "\"r_gain\":111") != NULL);
	ASSERT(strstr(resp, "\"g_gain\":222") != NULL);
	ASSERT(strstr(resp, "\"b_gain\":333") != NULL);
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ISP set→get roundtrip through ctrl handler
 * ══════════════════════════════════════════════════════════════════ */

TEST isp_brightness_roundtrip(void)
{
	setup();
	rec.return_val = 0;
	call("{\"cmd\":\"set-brightness\",\"value\":200}");
	ASSERT(strstr(resp, "\"ok\"") != NULL);
	ASSERT_EQ(200, rec.set_brightness);

	call("{\"cmd\":\"get-isp\"}");
	ASSERT(strstr(resp, "\"brightness\":200") != NULL);
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  Channel validation
 * ══════════════════════════════════════════════════════════════════ */

TEST reject_negative_channel(void)
{
	setup();
	int len = call("{\"cmd\":\"set-bitrate\",\"channel\":-1,\"value\":1000}");
	ASSERT(len > 0);
	ASSERT(strstr(resp, "\"error\"") != NULL);
	ASSERT_EQ(0, rec.call_count);
	teardown();
	PASS();
}

TEST reject_out_of_range_channel(void)
{
	setup();
	int len = call("{\"cmd\":\"set-bitrate\",\"channel\":99,\"value\":1000}");
	ASSERT(len > 0);
	ASSERT(strstr(resp, "\"error\"") != NULL);
	ASSERT_EQ(0, rec.call_count);
	teardown();
	PASS();
}

TEST reject_jpeg_channel_for_pipeline_cmd(void)
{
	setup();
	/* Stream 2 is JPEG — pipeline cmds should reject it */
	int len = call("{\"cmd\":\"stream-restart\",\"channel\":2}");
	ASSERT(len > 0);
	ASSERT(strstr(resp, "\"error\"") != NULL);
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  Privacy toggle semantics
 * ══════════════════════════════════════════════════════════════════ */

TEST privacy_on_sets_all_streams(void)
{
	setup();
	call("{\"cmd\":\"privacy\",\"value\":\"on\"}");
	ASSERT(st.privacy[0] == true);
	ASSERT(st.privacy[1] == true);
	teardown();
	PASS();
}

TEST privacy_toggle_flips_current(void)
{
	setup();
	st.privacy[0] = false;
	call("{\"cmd\":\"privacy\",\"channel\":0}");
	ASSERT(st.privacy[0] == true);
	/* Toggle again */
	call("{\"cmd\":\"privacy\",\"channel\":0}");
	ASSERT(st.privacy[0] == false);
	teardown();
	PASS();
}

TEST privacy_per_channel_doesnt_affect_other(void)
{
	setup();
	call("{\"cmd\":\"privacy\",\"channel\":0,\"value\":\"on\"}");
	ASSERT(st.privacy[0] == true);
	ASSERT(st.privacy[1] == false);
	teardown();
	PASS();
}

TEST privacy_response_excludes_jpeg(void)
{
	setup();
	call("{\"cmd\":\"privacy\",\"value\":\"off\"}");
	/* 3 streams, but stream 2 is JPEG — response array should have 2 entries */
	/* Count "off" occurrences in privacy array */
	int count = 0;
	const char *p = resp;
	while ((p = strstr(p, "\"off\"")) != NULL) {
		count++;
		p++;
	}
	ASSERT_EQ(2, count);
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  IVS gating: commands require ivs_active
 * ══════════════════════════════════════════════════════════════════ */

TEST ivs_detections_blocked_when_inactive(void)
{
	setup();
	atomic_store(&st.ivs_active, false);
	call("{\"cmd\":\"ivs-detections\"}");
	ASSERT(strstr(resp, "ivs not active") != NULL);
	teardown();
	PASS();
}

TEST ivs_set_sensitivity_blocked_when_inactive(void)
{
	setup();
	atomic_store(&st.ivs_active, false);
	call("{\"cmd\":\"ivs-set-sensitivity\",\"value\":50}");
	ASSERT(strstr(resp, "\"error\"") != NULL);
	teardown();
	PASS();
}

TEST ivs_status_works_regardless_of_active(void)
{
	setup();
	atomic_store(&st.ivs_active, false);
	atomic_store(&st.ivs_motion, true);
	call("{\"cmd\":\"ivs-status\"}");
	ASSERT(strstr(resp, "\"ok\"") != NULL);
	ASSERT(strstr(resp, "\"active\":false") != NULL);
	ASSERT(strstr(resp, "\"motion\":true") != NULL);
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  Dispatch / error paths
 * ══════════════════════════════════════════════════════════════════ */

TEST unknown_command_returns_error(void)
{
	setup();
	call("{\"cmd\":\"not-a-command\"}");
	ASSERT(strstr(resp, "unknown command") != NULL);
	teardown();
	PASS();
}

TEST missing_cmd_field_returns_error(void)
{
	setup();
	call("{\"value\":42}");
	ASSERT(strstr(resp, "missing cmd") != NULL);
	teardown();
	PASS();
}

TEST get_enc_caps_null_get_caps_returns_error(void)
{
	setup();
	ops.get_caps = NULL;
	call("{\"cmd\":\"get-enc-caps\"}");
	ASSERT(strstr(resp, "caps not available") != NULL);
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  get-qp-bounds reads from enc_cfg, not HAL
 * ══════════════════════════════════════════════════════════════════ */

TEST get_qp_bounds_reads_enc_cfg(void)
{
	setup();
	st.streams[0].enc_cfg.min_qp = 7;
	st.streams[0].enc_cfg.max_qp = 42;
	call("{\"cmd\":\"get-qp-bounds\",\"channel\":0}");
	ASSERT(strstr(resp, "\"min_qp\":7") != NULL);
	ASSERT(strstr(resp, "\"max_qp\":42") != NULL);
	teardown();
	PASS();
}

TEST get_rc_mode_reads_enc_cfg(void)
{
	setup();
	st.streams[0].enc_cfg.rc_mode = RSS_RC_SMART;
	call("{\"cmd\":\"get-rc-mode\",\"channel\":0}");
	ASSERT(strstr(resp, "\"rc_mode\":\"smart\"") != NULL);
	ASSERT(strstr(resp, "\"rc_mode_id\":3") != NULL);
	teardown();
	PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  Suite
 * ══════════════════════════════════════════════════════════════════ */

SUITE(ctrl_suite)
{
	/* State mutation on success vs failure */
	RUN_TEST(set_bitrate_updates_state_on_success);
	RUN_TEST(set_bitrate_no_state_change_on_hal_failure);
	RUN_TEST(set_gop_no_state_change_on_hal_failure);
	RUN_TEST(set_fps_updates_state_on_success);
	RUN_TEST(set_qp_bounds_atomic_update);
	RUN_TEST(set_qp_bounds_neither_updated_on_failure);
	RUN_TEST(set_rc_mode_stores_enum_not_string);
	RUN_TEST(set_rc_mode_persists_string);

	/* HW channel mapping */
	RUN_TEST(set_bitrate_passes_hw_channel);
	RUN_TEST(set_gop_passes_hw_channel);
	RUN_TEST(set_rc_mode_passes_hw_channel_and_bitrate);
	RUN_TEST(set_rc_mode_uses_current_bitrate_when_not_specified);

	/* Multi-stream isolation */
	RUN_TEST(set_bitrate_ch0_doesnt_affect_ch1);
	RUN_TEST(config_written_to_correct_section);

	/* Table-driven enc-set/get */
	RUN_TEST(enc_set_int_value_reaches_hal);
	RUN_TEST(enc_set_u32_value_reaches_hal);
	RUN_TEST(enc_set_bool_value_reaches_hal);
	RUN_TEST(enc_get_int_roundtrip);
	RUN_TEST(enc_get_bool_roundtrip);
	RUN_TEST(enc_set_null_fn_ptr_returns_not_supported);
	RUN_TEST(enc_get_set_only_param_returns_no_getter);
	RUN_TEST(enc_set_unknown_param);
	RUN_TEST(enc_list_has_all_params_with_types);
	RUN_TEST(enc_list_with_channel_includes_values);

	/* WB partial merge */
	RUN_TEST(set_wb_mode_only_preserves_gains);
	RUN_TEST(set_wb_gain_only_preserves_mode);
	RUN_TEST(get_wb_returns_current_state);

	/* ISP roundtrip */
	RUN_TEST(isp_brightness_roundtrip);

	/* Channel validation */
	RUN_TEST(reject_negative_channel);
	RUN_TEST(reject_out_of_range_channel);
	RUN_TEST(reject_jpeg_channel_for_pipeline_cmd);

	/* Privacy */
	RUN_TEST(privacy_on_sets_all_streams);
	RUN_TEST(privacy_toggle_flips_current);
	RUN_TEST(privacy_per_channel_doesnt_affect_other);
	RUN_TEST(privacy_response_excludes_jpeg);

	/* IVS gating */
	RUN_TEST(ivs_detections_blocked_when_inactive);
	RUN_TEST(ivs_set_sensitivity_blocked_when_inactive);
	RUN_TEST(ivs_status_works_regardless_of_active);

	/* Dispatch / error */
	RUN_TEST(unknown_command_returns_error);
	RUN_TEST(missing_cmd_field_returns_error);
	RUN_TEST(get_enc_caps_null_get_caps_returns_error);

	/* Getters read from correct source */
	RUN_TEST(get_qp_bounds_reads_enc_cfg);
	RUN_TEST(get_rc_mode_reads_enc_cfg);
}
