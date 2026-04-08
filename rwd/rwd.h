/*
 * rwd.h -- Raptor WebRTC Daemon internal types and declarations
 *
 * Minimal WebRTC-over-WHIP daemon. Sends live H.264 + Opus to browsers
 * with sub-second latency. No transcoding, no plugins.
 *
 * Stack: ICE-lite + DTLS-SRTP (mbedTLS) + SRTP (compy) + WHIP signaling.
 */

#ifndef RWD_H
#define RWD_H

#include <compy.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#ifdef RSS_HAS_TLS
#include <rss_tls.h>
#endif

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/timing.h>
#include <mbedtls/sha256.h>

#define RWD_MAX_CLIENTS	   4
#define RWD_UDP_BUF_SIZE   2048
#define RWD_HTTP_BUF_SIZE  16384
#define RWD_SDP_BUF_SIZE   4096
#define RWD_SESSION_ID_LEN 16

/* RTP payload types and clocks */
#define RWD_VIDEO_PT	96
#define RWD_AUDIO_PT	111
#define RWD_VIDEO_CLOCK 90000
#define RWD_AUDIO_CLOCK 48000 /* Opus RTP clock per RFC 7587 */

/* STUN constants (RFC 5389) */
#define STUN_HEADER_SIZE	    20
#define STUN_MAGIC_COOKIE	    0x2112A442
#define STUN_BINDING_REQUEST	    0x0001
#define STUN_BINDING_RESPONSE	    0x0101
#define STUN_ATTR_USERNAME	    0x0006
#define STUN_ATTR_MESSAGE_INTEGRITY 0x0008
#define STUN_ATTR_XOR_MAPPED_ADDR   0x0020
#define STUN_ATTR_FINGERPRINT	    0x8028
#define STUN_FINGERPRINT_XOR	    0x5354554E

/* ── DTLS server context (shared by all clients) ── */

typedef struct {
	mbedtls_ssl_config conf;
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_ssl_cookie_ctx cookie;
	char fingerprint[128]; /* "sha-256 AA:BB:CC:..." for SDP */
} rwd_dtls_ctx_t;

/* ── SDP offer (parsed from browser) ── */

typedef struct {
	char ice_ufrag[64];
	char ice_pwd[256];
	char fingerprint[256]; /* browser's DTLS fingerprint */
	char setup[16];	       /* actpass / active / passive */

	int video_pt;	      /* browser's H264 payload type (-1 if none) */
	char video_fmtp[256]; /* browser's fmtp for the matched H264 PT */
	int audio_pt;	      /* browser's Opus payload type (-1 if none) */
	char mid_video[16];   /* BUNDLE mid for video */
	char mid_audio[16];   /* BUNDLE mid for audio */

	int mid_ext_id; /* extmap ID for sdes:mid (-1 if not offered) */
	bool has_video;
	bool has_audio;

	/* ICE candidates from offer (for NAT hole punching) */
#define RWD_MAX_CANDIDATES 8
	struct {
		char ip[64];
		uint16_t port;
	} candidates[RWD_MAX_CANDIDATES];
	int candidate_count;
} rwd_sdp_offer_t;

/* ── DTLS connection state ── */

typedef enum {
	RWD_DTLS_NEW,
	RWD_DTLS_HANDSHAKING,
	RWD_DTLS_ESTABLISHED,
	RWD_DTLS_FAILED,
} rwd_dtls_state_t;

/* ── Per-client state ── */

typedef struct rwd_client rwd_client_t;

typedef struct rwd_server rwd_server_t;

struct rwd_client {
	struct sockaddr_storage addr;
	socklen_t addr_len;
	bool active;

	/* ICE */
	char local_ufrag[16];
	char local_pwd[32];
	char remote_ufrag[64];
	char remote_pwd[256];
	bool ice_verified;
	int64_t last_stun_at; /* last STUN binding request (consent freshness) */

	/* DTLS */
	rwd_dtls_state_t dtls_state;
	mbedtls_ssl_context ssl;
	mbedtls_timing_delay_context timer;
	uint8_t dtls_buf[RWD_UDP_BUF_SIZE]; /* buffered incoming DTLS record */
	size_t dtls_buf_len;

	/* Exported TLS key material (filled by callback during handshake) */
	uint8_t master_secret[48];
	uint8_t randbytes[64]; /* client_random[32] + server_random[32] */
	mbedtls_tls_prf_types tls_prf;
	bool keys_exported;

	/* SRTP (populated after DTLS handshake) */
	Compy_Transport srtp_video;
	Compy_Transport srtp_audio;
	Compy_RtpTransport *rtp_video;
	Compy_RtpTransport *rtp_audio;
	Compy_NalTransport *nal_video;
	Compy_Rtcp *rtcp_video;
	Compy_Rtcp *rtcp_audio;
	int64_t last_rtcp_video;
	int64_t last_rtcp_audio;
	bool media_ready; /* set after SRTP stack fully initialized */

	/* Pre-generated SSRCs (declared in SDP, set on RTP transports) */
	uint32_t video_ssrc;
	uint32_t audio_ssrc;

	/* Backchannel (browser → camera audio) */
	Compy_SrtpRecvCtx *srtp_recv;	/* decrypt incoming SRTP */
	Compy_Backchannel *backchannel; /* RTP receiver + audio callback */
	rss_ring_t *speaker_ring;	/* output ring for RAD playback */
	void *bc_recv;			/* rwd_bc_recv_t, kept alive for callback */

	/* Media state */
	int stream_idx; /* 0=main, 1=sub */
	bool sending;
	bool waiting_keyframe;
	uint32_t video_ts_offset;
	bool video_ts_base_set;
	uint32_t audio_ts_offset;
	bool audio_ts_base_set;

	/* Session */
	char session_id[RWD_SESSION_ID_LEN * 2 + 1]; /* hex string */
	int64_t created_at;

	/* SDP offer data (kept for reference) */
	rwd_sdp_offer_t offer;

	/* Back pointer */
	rwd_server_t *server;
};

/* ── Server state ── */

struct rwd_server {
	int udp_fd;  /* STUN / DTLS / SRTP */
	int http_fd; /* HTTP listener for WHIP signaling */
	int epoll_fd;
	rss_ctrl_t *ctrl; /* raptorctl control socket */

	rss_config_t *cfg;
	const char *config_path;
	volatile sig_atomic_t *running;

	rwd_dtls_ctx_t *dtls;

	rwd_client_t *clients[RWD_MAX_CLIENTS];
	int client_count;
	pthread_mutex_t clients_lock;

	/* Video ring readers (0=main, 1=sub, 2+=multi-sensor) */
#define RWD_STREAM_COUNT 6
	rss_ring_t *video_rings[RWD_STREAM_COUNT];
	uint64_t video_read_seq[RWD_STREAM_COUNT];
	uint8_t *video_bufs[RWD_STREAM_COUNT];
	uint32_t video_buf_sizes[RWD_STREAM_COUNT];

	/* Audio ring */
	rss_ring_t *audio_ring;
	uint64_t audio_read_seq;
	bool has_audio;

	int udp_port;
	int http_port;
	int max_clients;
	char local_ip[64];
	bool local_ip_configured; /* true if set from config (don't auto-refresh) */

	/* Server-reflexive address (STUN-discovered, for external candidates) */
	char srflx_ip[64];
	uint16_t srflx_port;
	bool has_srflx;

	void *webtorrent; /* rwd_webtorrent_t* if active, NULL otherwise */

#ifdef RSS_HAS_TLS
	rss_tls_ctx_t *tls; /* HTTPS for signaling (NULL = plain HTTP) */
#endif
};

/* ── rwd_dtls.c ── */

int rwd_dtls_init(rwd_dtls_ctx_t *ctx, const char *cert_path, const char *key_path);
void rwd_dtls_free(rwd_dtls_ctx_t *ctx);
int rwd_dtls_client_init(rwd_client_t *c, rwd_dtls_ctx_t *ctx);
void rwd_dtls_client_free(rwd_client_t *c);
int rwd_dtls_handshake_step(rwd_client_t *c);
int rwd_dtls_export_srtp_keys(rwd_client_t *c, Compy_SrtpKeyMaterial *send_key,
			      Compy_SrtpKeyMaterial *recv_key);

/* ── rwd_ice.c ── */

void rwd_crc32_init(void);
int rwd_ice_process(rwd_server_t *srv, const uint8_t *buf, size_t len,
		    const struct sockaddr_storage *from, socklen_t from_len);
int rwd_ice_send_check(rwd_server_t *srv, rwd_client_t *c, const char *dest_ip, uint16_t dest_port);

/* ── rwd_sdp.c ── */

int rwd_sdp_parse_offer(const char *sdp, rwd_sdp_offer_t *offer);
int rwd_sdp_generate_answer(rwd_client_t *c, const rwd_server_t *srv, char *buf, size_t buf_size);

/* ── rwd_signaling.c ── */

void rwd_signaling_handle(rwd_server_t *srv, int client_fd,
			  const struct sockaddr_storage *local_addr);
rwd_client_t *rwd_client_from_offer(rwd_server_t *srv, const char *sdp, int stream_idx,
				    char *sdp_answer, size_t sdp_answer_size);

/* ── rwd_media.c ── */

Compy_Transport rwd_transport_sendto(int fd, const struct sockaddr_storage *addr,
				     socklen_t addr_len);
int rwd_media_setup(rwd_client_t *c);
void rwd_media_teardown(rwd_client_t *c);
void rwd_media_feed_rtp(rwd_client_t *c, uint8_t *data, size_t len);
void *rwd_video_reader_thread(void *arg);
void *rwd_audio_reader_thread(void *arg);

/* ── Utility (rwd_main.c) ── */

int rwd_get_local_ip(char *buf, size_t buflen);
void rwd_generate_ice_credentials(char *ufrag, size_t ufrag_len, char *pwd, size_t pwd_len);
void rwd_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
		   uint8_t out[20]);
void rwd_random_init(void);
int rwd_random_bytes(uint8_t *buf, size_t len);

/* ── rwd_webtorrent.c (optional, RAPTOR_WEBTORRENT) ── */

#ifdef RAPTOR_WEBTORRENT

typedef struct {
	rwd_server_t *srv;
	pthread_t thread;
	volatile bool running;

	char share_key[32];
	char info_hash[48]; /* base64 SHA256, 44 chars + null */
	char peer_id[41];   /* hex random, 40 chars + null */

	char tracker_url[256];
	char stun_server[128];
	int stun_port;
	char viewer_base_url[256];
	bool tls_verify;
} rwd_webtorrent_t;

int rwd_webtorrent_start(rwd_webtorrent_t *wt, rwd_server_t *srv);
void rwd_webtorrent_stop(rwd_webtorrent_t *wt);
void rwd_webtorrent_rotate_key(rwd_webtorrent_t *wt);
int rwd_stun_discover_srflx(int udp_fd, const char *server, int port, char *ip_out, size_t ip_size,
			    uint16_t *port_out);

#endif /* RAPTOR_WEBTORRENT */

#endif /* RWD_H */
