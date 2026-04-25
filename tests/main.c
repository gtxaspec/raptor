#include "greatest.h"

extern SUITE(nal_suite);
extern SUITE(prebuf_suite);
extern SUITE(mux_suite);
extern SUITE(sdp_suite);
extern SUITE(codec_suite);
extern SUITE(ring_suite);
extern SUITE(rsp_url_suite);
extern SUITE(rsp_amf0_suite);
extern SUITE(rsp_chunk_suite);
extern SUITE(rsp_flv_suite);
extern SUITE(rsp_audio_suite);
extern SUITE(rsp_annexb_suite);
extern SUITE(rsp_state_suite);
extern SUITE(ctrl_suite);

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
	GREATEST_MAIN_BEGIN();
	RUN_SUITE(nal_suite);
	RUN_SUITE(prebuf_suite);
	RUN_SUITE(mux_suite);
	RUN_SUITE(sdp_suite);
	RUN_SUITE(codec_suite);
	RUN_SUITE(ring_suite);
	RUN_SUITE(rsp_url_suite);
	RUN_SUITE(rsp_amf0_suite);
	RUN_SUITE(rsp_chunk_suite);
	RUN_SUITE(rsp_flv_suite);
	RUN_SUITE(rsp_audio_suite);
	RUN_SUITE(rsp_annexb_suite);
	RUN_SUITE(rsp_state_suite);
	RUN_SUITE(ctrl_suite);
	GREATEST_MAIN_END();
}
