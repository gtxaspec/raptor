#include "greatest.h"

extern SUITE(nal_suite);
extern SUITE(prebuf_suite);
extern SUITE(mux_suite);
extern SUITE(sdp_suite);
extern SUITE(codec_suite);

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
	GREATEST_MAIN_BEGIN();
	RUN_SUITE(nal_suite);
	RUN_SUITE(prebuf_suite);
	RUN_SUITE(mux_suite);
	RUN_SUITE(sdp_suite);
	RUN_SUITE(codec_suite);
	GREATEST_MAIN_END();
}
