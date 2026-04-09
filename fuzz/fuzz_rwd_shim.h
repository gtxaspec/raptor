/*
 * Minimal shim replacing rwd.h for fuzz targets.
 * Provides only the types and declarations the SDP parser needs,
 * avoiding compy/mbedTLS transitive includes.
 */

#ifndef FUZZ_RWD_SHIM_H
#define FUZZ_RWD_SHIM_H

#include <stdbool.h>
#include <stdint.h>
#include <rss_common.h>

#define RWD_MAX_CANDIDATES 8

typedef struct {
	char ice_ufrag[64];
	char ice_pwd[256];
	char fingerprint[256];
	char setup[16];
	int video_pt;
	char video_fmtp[256];
	int audio_pt;
	bool has_pcmu;
	bool has_pcma;
	char mid_video[16];
	char mid_audio[16];
	int mid_ext_id;
	bool has_video;
	bool has_audio;
	struct {
		char ip[64];
		uint16_t port;
	} candidates[RWD_MAX_CANDIDATES];
	int candidate_count;
} rwd_sdp_offer_t;

int rwd_sdp_parse_offer(const char *sdp, rwd_sdp_offer_t *offer);

/* Stub types so rwd_sdp_generate_answer compiles (not fuzzed) */
typedef struct rwd_client rwd_client_t;
typedef struct rwd_server rwd_server_t;

#define RWD_CODEC_PCMU 0
#define RWD_CODEC_PCMA 8
#define RWD_CODEC_OPUS 111

#endif
