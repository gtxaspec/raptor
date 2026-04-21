/*
 * rfs_main.c -- Raptor File Source Daemon
 *
 * Reads video and audio from files, publishes to ring buffers at
 * real-time rate. Replaces RVD+RAD on platforms without ISP/encoder
 * hardware (A1, x86 testing).
 *
 * Input formats:
 *   - MP4/MOV containers (H.264/H.265 video, AAC/Opus/G.711 audio,
 *     MP3 audio transcoded via libhelix)
 *   - Raw Annex B H.264/H.265 files + raw PCM audio files
 *
 * Audio encoding: reuses RAD codec plugins (L16, PCMU, PCMA, AAC, Opus).
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <rss_common.h>
#include <rss_ipc.h>
#include <raptor_hal.h>
#include "rad.h"
#include "rfs_annexb.h"
#include "rfs_mp4.h"

/* ── Constants ── */

#define RFS_AUDIO_CHUNK_MS 20
#define RFS_CTRL_POLL_MS   50
#define RFS_MAX_FILE_SIZE  (256 * 1024 * 1024)

/* ── Types ── */

typedef struct {
	char video_path[256];
	int fps;
	char video_ring_name[32];
	int video_slots;
	int video_data_mb;
	int codec; /* RSS_CODEC_H264, RSS_CODEC_H265, or -1 for auto */

	char audio_path[256];
	char audio_codec_name[16];
	int audio_sample_rate;
	char audio_ring_name[32];
	int audio_slots;
	int audio_data_mb;

	bool loop;
	bool enabled;
} rfs_config_t;

typedef struct {
	rss_config_t *cfg;
	const char *config_path;
	volatile sig_atomic_t *running;
	rss_ctrl_t *ctrl;

	rfs_config_t settings;

	/* Video */
	uint8_t *video_data;
	size_t video_size;
	int codec;
	uint32_t width;
	uint32_t height;
	uint8_t profile;
	uint8_t level;
	rfs_frame_t *frames;
	uint32_t frame_count;
	uint32_t frame_pos;
	rss_ring_t *video_ring;

	/* Audio */
	uint8_t *audio_data;
	size_t audio_size;
	size_t audio_pos;
	bool audio_data_owned; /* true = mmap'd by us, false = borrowed (e.g. mp4 pcm) */
	uint32_t audio_chunk_samples;
	rss_ring_t *audio_ring;
	const rad_codec_ops_t *audio_ops;
	rad_codec_ctx_t audio_codec;
	uint8_t *encode_buf;

	/* MP4 container */
	rfs_mp4_ctx_t mp4;
	bool is_mp4;
	uint32_t mp4_audio_pos;
	uint8_t *scratch;
	uint32_t scratch_cap;

	/* PCM read buffer (avoids aliasing cast from mmap'd uint8_t* to int16_t*) */
	int16_t *pcm_chunk;

	/* Playback */
	bool paused;
	int64_t epoch;
	uint64_t video_published;
	uint64_t audio_published;
} rfs_state_t;

/* ── Video file setup ── */

static uint8_t *mmap_file(const char *path, size_t *out_size)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		RSS_ERROR("open %s: %s", path, strerror(errno));
		return NULL;
	}

	struct stat sb;
	if (fstat(fd, &sb) < 0) {
		RSS_ERROR("stat %s: %s", path, strerror(errno));
		close(fd);
		return NULL;
	}

	if (sb.st_size == 0 || sb.st_size > RFS_MAX_FILE_SIZE) {
		RSS_ERROR("%s: invalid size %lld (max %d MB)", path, (long long)sb.st_size,
			  RFS_MAX_FILE_SIZE / (1024 * 1024));
		close(fd);
		return NULL;
	}

	uint8_t *data = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);

	if (data == MAP_FAILED) {
		RSS_ERROR("mmap %s: %s", path, strerror(errno));
		return NULL;
	}

	madvise(data, (size_t)sb.st_size, MADV_SEQUENTIAL);
	*out_size = (size_t)sb.st_size;
	return data;
}

static int setup_video(rfs_state_t *st)
{
	if (st->settings.video_path[0] == '\0')
		return 0;

	st->video_data = mmap_file(st->settings.video_path, &st->video_size);
	if (!st->video_data)
		return -1;

	rfs_annexb_info_t vi;
	if (rfs_annexb_scan(st->video_data, st->video_size, st->settings.video_path,
			    st->settings.codec, &vi) < 0)
		return -1;

	st->codec = vi.codec;
	st->width = vi.width;
	st->height = vi.height;
	st->profile = vi.profile;
	st->level = vi.level;
	st->frames = vi.frames;
	st->frame_count = vi.frame_count;

	if (st->width == 0 || st->height == 0) {
		RSS_ERROR("resolution unknown -- SPS not found in file");
		return -1;
	}

	int data_mb = st->settings.video_data_mb;
	if (data_mb <= 0)
		data_mb = 2;

	st->video_ring =
		rss_ring_create(st->settings.video_ring_name, (uint32_t)st->settings.video_slots,
				(uint32_t)data_mb * 1024 * 1024);
	if (!st->video_ring) {
		RSS_ERROR("failed to create video ring '%s'", st->settings.video_ring_name);
		return -1;
	}

	rss_ring_set_stream_info(st->video_ring, 0, (uint32_t)st->codec, st->width, st->height,
				 (uint32_t)st->settings.fps, 1, st->profile, st->level);

	RSS_INFO("video ring '%s': %d slots, %d MB", st->settings.video_ring_name,
		 st->settings.video_slots, data_mb);
	return 0;
}

/* ── Audio file setup ── */

static int setup_audio(rfs_state_t *st)
{
	if (st->settings.audio_path[0] == '\0')
		return 0;

	st->audio_data = mmap_file(st->settings.audio_path, &st->audio_size);
	if (!st->audio_data) {
		RSS_WARN("audio file unavailable, continuing without audio");
		return 0;
	}
	st->audio_data_owned = true;

	st->audio_ops = rad_codec_find(st->settings.audio_codec_name);
	if (!st->audio_ops) {
		RSS_ERROR("unknown audio codec '%s'", st->settings.audio_codec_name);
		return -1;
	}

	int rate = st->settings.audio_sample_rate;

	st->audio_chunk_samples = (uint32_t)(rate * RFS_AUDIO_CHUNK_MS / 1000);
	if (st->audio_chunk_samples == 0) {
		RSS_ERROR("invalid audio sample rate %d", rate);
		return -1;
	}

	int data_mb = st->settings.audio_data_mb;
	if (data_mb <= 0)
		data_mb = 1;

	st->audio_ring =
		rss_ring_create(st->settings.audio_ring_name, (uint32_t)st->settings.audio_slots,
				(uint32_t)data_mb * 1024 * 1024);
	if (!st->audio_ring) {
		RSS_ERROR("failed to create audio ring '%s'", st->settings.audio_ring_name);
		return -1;
	}

	rss_ring_set_stream_info(st->audio_ring, 0x10, (uint32_t)st->audio_ops->codec_id, 0, 0,
				 (uint32_t)rate, 1, 0, 0);

	/* Initialize codec encoder */
	st->audio_codec.ring = st->audio_ring;
	st->audio_codec.codec_id = st->audio_ops->codec_id;
	if (st->audio_ops->init(&st->audio_codec, st->cfg, rate) < 0) {
		RSS_ERROR("audio codec '%s' init failed", st->audio_ops->name);
		return -1;
	}

	st->encode_buf = malloc((size_t)st->audio_codec.encode_buf_size);
	if (!st->encode_buf) {
		RSS_ERROR("encode buffer alloc failed");
		return -1;
	}

	st->pcm_chunk = malloc(st->audio_chunk_samples * sizeof(int16_t));
	if (!st->pcm_chunk)
		return -1;

	RSS_INFO("audio ring '%s': %d slots, %d MB, %s @ %d Hz, %u samples/chunk",
		 st->settings.audio_ring_name, st->settings.audio_slots, data_mb,
		 st->audio_ops->name, rate, st->audio_chunk_samples);
	return 0;
}

/* ── MP4 container setup ── */

static int setup_mp4(rfs_state_t *st, const char *path)
{
	if (rfs_mp4_open(&st->mp4, path) < 0)
		return -1;

	st->is_mp4 = true;

	/* Video ring */
	if (st->mp4.video_count > 0) {
		st->codec = st->mp4.video_codec;
		st->width = st->mp4.width;
		st->height = st->mp4.height;
		st->profile = st->mp4.profile;
		st->level = st->mp4.level;
		st->frame_count = st->mp4.video_count;
		st->settings.fps = st->mp4.fps;

		int data_mb = st->settings.video_data_mb;
		if (data_mb <= 0)
			data_mb = 2;

		st->video_ring = rss_ring_create(st->settings.video_ring_name,
						 (uint32_t)st->settings.video_slots,
						 (uint32_t)data_mb * 1024 * 1024);
		if (!st->video_ring) {
			RSS_ERROR("failed to create video ring");
			return -1;
		}
		rss_ring_set_stream_info(st->video_ring, 0, (uint32_t)st->codec, st->width,
					 st->height, (uint32_t)st->settings.fps, 1, st->profile,
					 st->level);

		uint32_t max_frame = 0;
		for (uint32_t i = 0; i < st->mp4.video_count; i++)
			if (st->mp4.video_frames[i].length > max_frame)
				max_frame = st->mp4.video_frames[i].length;
		st->scratch_cap = st->mp4.codec_config_len + max_frame * 2;
		st->scratch = malloc(st->scratch_cap);
		if (!st->scratch)
			return -1;
	}

	/* Audio ring */
	bool has_mp4_audio = st->mp4.audio_count > 0 && st->mp4.audio_codec_id >= 0;
	bool has_transcoded_pcm = st->mp4.needs_transcode && st->mp4.pcm_samples > 0;

	if (has_mp4_audio || has_transcoded_pcm) {
		int data_mb = st->settings.audio_data_mb;
		if (data_mb <= 0)
			data_mb = 1;

		st->audio_ring = rss_ring_create(st->settings.audio_ring_name,
						 (uint32_t)st->settings.audio_slots,
						 (uint32_t)data_mb * 1024 * 1024);
		if (!st->audio_ring) {
			RSS_WARN("failed to create audio ring, continuing without audio");
		} else if (has_transcoded_pcm) {
			st->audio_data = (uint8_t *)st->mp4.pcm_data;
			st->audio_size = (size_t)st->mp4.pcm_samples * 2;
			st->audio_pos = 0;

			st->audio_ops = rad_codec_find(st->settings.audio_codec_name);
			if (!st->audio_ops) {
				RSS_ERROR("unknown audio codec '%s'",
					  st->settings.audio_codec_name);
				return -1;
			}

			int rate = st->mp4.pcm_rate > 0 ? st->mp4.pcm_rate
							: st->settings.audio_sample_rate;
			st->audio_chunk_samples = (uint32_t)(rate * RFS_AUDIO_CHUNK_MS / 1000);

			rss_ring_set_stream_info(st->audio_ring, 0x10,
						 (uint32_t)st->audio_ops->codec_id, 0, 0,
						 (uint32_t)rate, 1, 0, 0);

			st->audio_codec.ring = st->audio_ring;
			st->audio_codec.codec_id = st->audio_ops->codec_id;
			if (st->audio_ops->init(&st->audio_codec, st->cfg, rate) < 0) {
				RSS_ERROR("audio codec init failed");
				return -1;
			}
			st->encode_buf = malloc((size_t)st->audio_codec.encode_buf_size);
			if (!st->encode_buf)
				return -1;
			st->pcm_chunk = malloc(st->audio_chunk_samples * sizeof(int16_t));
			if (!st->pcm_chunk)
				return -1;

			RSS_INFO("mp4 audio: transcoded PCM → %s @ %d Hz", st->audio_ops->name,
				 rate);
		} else {
			rss_ring_set_stream_info(st->audio_ring, 0x10,
						 (uint32_t)st->mp4.audio_codec_id, 0, 0,
						 (uint32_t)st->mp4.audio_sample_rate, 1, 0, 0);
			RSS_INFO("mp4 audio ring: codec_id=%d %dHz", st->mp4.audio_codec_id,
				 st->mp4.audio_sample_rate);
		}
	}

	return 0;
}

/* ── Config ── */

static void load_config(rfs_state_t *st)
{
	rss_config_t *c = st->cfg;
	rfs_config_t *s = &st->settings;

	s->enabled = rss_config_get_bool(c, "filesource", "enabled", true);

	rss_strlcpy(s->video_path, rss_config_get_str(c, "filesource", "video_file", ""),
		    sizeof(s->video_path));
	rss_strlcpy(s->audio_path, rss_config_get_str(c, "filesource", "audio_file", ""),
		    sizeof(s->audio_path));

	s->fps = rss_config_get_int(c, "filesource", "fps", 25);
	s->loop = rss_config_get_bool(c, "filesource", "loop", true);

	const char *codec_str = rss_config_get_str(c, "filesource", "codec", "auto");
	if (strcmp(codec_str, "h264") == 0)
		s->codec = RSS_CODEC_H264;
	else if (strcmp(codec_str, "h265") == 0)
		s->codec = RSS_CODEC_H265;
	else
		s->codec = -1;

	rss_strlcpy(s->video_ring_name,
		    rss_config_get_str(c, "filesource", "video_ring_name", "main"),
		    sizeof(s->video_ring_name));
	s->video_slots = rss_config_get_int(c, "filesource", "video_slots", 32);
	s->video_data_mb = rss_config_get_int(c, "filesource", "video_data_mb", 2);

	const char *acodec = rss_config_get_str(c, "filesource", "audio_codec", "l16");
	if (strcmp(acodec, "g711u") == 0)
		acodec = "pcmu";
	else if (strcmp(acodec, "g711a") == 0)
		acodec = "pcma";
	rss_strlcpy(s->audio_codec_name, acodec, sizeof(s->audio_codec_name));

	s->audio_sample_rate = rss_config_get_int(c, "filesource", "audio_sample_rate", 16000);

	rss_strlcpy(s->audio_ring_name,
		    rss_config_get_str(c, "filesource", "audio_ring_name", "audio"),
		    sizeof(s->audio_ring_name));
	s->audio_slots = rss_config_get_int(c, "filesource", "audio_slots", 32);
	s->audio_data_mb = rss_config_get_int(c, "filesource", "audio_data_mb", 1);
}

/* ── Control socket ── */

static int ctrl_handler(const char *cmd_json, char *resp, int resp_size, void *ud)
{
	rfs_state_t *st = ud;

	int rc = rss_ctrl_handle_common(cmd_json, resp, resp_size, st->cfg, st->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp, resp_size, "missing cmd");

	if (strcmp(cmd, "pause") == 0) {
		st->paused = true;
		RSS_INFO("paused");
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "resume") == 0) {
		if (st->paused) {
			st->paused = false;
			st->epoch = rss_timestamp_us() -
				    (int64_t)st->frame_pos * 1000000LL / st->settings.fps;
			RSS_INFO("resumed at frame %u/%u", st->frame_pos, st->frame_count);
		}
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	if (strcmp(cmd, "seek") == 0) {
		if (st->frame_count == 0)
			return rss_ctrl_resp_error(resp, resp_size, "no frames");
		int target = 0;
		rss_json_get_int(cmd_json, "frame", &target);
		if (target < 0)
			target = 0;
		if ((uint32_t)target >= st->frame_count)
			target = (int)(st->frame_count - 1);
		st->frame_pos = (uint32_t)target;
		st->epoch =
			rss_timestamp_us() - (int64_t)st->frame_pos * 1000000LL / st->settings.fps;
		RSS_INFO("seek to frame %u", st->frame_pos);
		return rss_ctrl_resp_ok(resp, resp_size);
	}

	/* Default: status */
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON_AddStringToObject(r, "state", st->paused ? "paused" : "playing");
	cJSON_AddStringToObject(r, "codec", st->codec == RSS_CODEC_H265 ? "h265" : "h264");
	cJSON_AddNumberToObject(r, "width", st->width);
	cJSON_AddNumberToObject(r, "height", st->height);
	cJSON_AddNumberToObject(r, "fps", st->settings.fps);
	cJSON_AddNumberToObject(r, "frame", st->frame_pos);
	cJSON_AddNumberToObject(r, "total_frames", st->frame_count);
	cJSON_AddBoolToObject(r, "loop", st->settings.loop);
	if (st->video_ring)
		cJSON_AddStringToObject(r, "video_file", st->settings.video_path);
	if (st->audio_ring)
		cJSON_AddStringToObject(r, "audio_file", st->settings.audio_path);
	return rss_ctrl_resp_json(resp, resp_size, r);
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rfs", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	rfs_state_t st = {0};
	int epoll_fd = -1;

	st.cfg = ctx.cfg;
	st.config_path = ctx.config_path;
	st.running = ctx.running;
	load_config(&st);

	if (!st.settings.enabled) {
		RSS_INFO("filesource disabled in config");
		goto cleanup;
	}

	if (st.settings.video_path[0] == '\0' && st.settings.audio_path[0] == '\0') {
		RSS_ERROR("no video_file or audio_file configured");
		goto cleanup;
	}

	if (st.settings.video_path[0] != '\0' && rfs_mp4_is_container(st.settings.video_path)) {
		if (setup_mp4(&st, st.settings.video_path) < 0)
			goto cleanup;
	} else {
		if (setup_video(&st) < 0)
			goto cleanup;
		if (setup_audio(&st) < 0)
			goto cleanup;
	}

	/* Control socket */
	rss_mkdir_p("/var/run/rss");
	st.ctrl = rss_ctrl_listen("/var/run/rss/rfs.sock");

	int ctrl_fd = st.ctrl ? rss_ctrl_get_fd(st.ctrl) : -1;
	if (ctrl_fd >= 0) {
		epoll_fd = epoll_create1(0);
		if (epoll_fd >= 0) {
			struct epoll_event ev = {.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl: %s", strerror(errno));
		}
	}

	RSS_INFO("rfs running (%s, %ux%u, %dfps, loop=%s)",
		 st.codec == RSS_CODEC_H265 ? "H.265" : "H.264", st.width, st.height,
		 st.settings.fps, st.settings.loop ? "yes" : "no");

	st.epoch = rss_timestamp_us();

	/* ── Main loop ── */
	while (*st.running) {
		if (st.paused) {
			if (epoll_fd >= 0) {
				struct epoll_event ev;
				int n = epoll_wait(epoll_fd, &ev, 1, 100);
				if (n > 0 && st.ctrl)
					rss_ctrl_accept_and_handle(st.ctrl, ctrl_handler, &st);
			} else {
				usleep(100000);
			}
			continue;
		}

		int64_t now = rss_timestamp_us();
		bool did_work = false;

		if (st.is_mp4) {
			/* ── MP4: use container PTS, convert AVCC→Annex B on-the-fly ── */
			if (st.video_ring && st.frame_pos < st.frame_count) {
				rfs_mp4_frame_t *mf = &st.mp4.video_frames[st.frame_pos];
				int64_t rel = mf->pts_us - st.mp4.video_frames[0].pts_us;
				if (now >= st.epoch + rel) {
					int len = rfs_mp4_to_annexb(&st.mp4, mf, st.scratch,
								    st.scratch_cap);
					if (len > 0)
						rss_ring_publish(st.video_ring, st.scratch,
								 (uint32_t)len, st.epoch + rel,
								 mf->nal_type, mf->is_key);
					st.frame_pos++;
					st.video_published++;
					did_work = true;
				}
			}
			if (st.audio_ring && st.audio_ops && st.audio_pos < st.audio_size) {
				/* Transcoded PCM: chunk + encode via RAD codec */
				int64_t target = st.epoch + (int64_t)st.audio_published *
								    RFS_AUDIO_CHUNK_MS * 1000LL;
				if (now >= target) {
					uint32_t remain = (uint32_t)(st.audio_size - st.audio_pos);
					uint32_t chunk_bytes = st.audio_chunk_samples * 2;
					if (chunk_bytes > remain)
						chunk_bytes = remain & ~1u;
					int samples = (int)(chunk_bytes / 2);
					memcpy(st.pcm_chunk, st.audio_data + st.audio_pos,
					       chunk_bytes);
					int encoded = st.audio_ops->encode(
						&st.audio_codec, st.pcm_chunk, samples,
						st.encode_buf, st.audio_codec.encode_buf_size, now);
					if (encoded > 0)
						rss_ring_publish(st.audio_ring, st.encode_buf,
								 (uint32_t)encoded, now,
								 (uint16_t)st.audio_codec.codec_id,
								 0);
					st.audio_pos += chunk_bytes;
					st.audio_published++;
					did_work = true;
				}
			} else if (st.audio_ring && st.mp4_audio_pos < st.mp4.audio_count) {
				/* Direct passthrough from MP4 container */
				rfs_mp4_frame_t *af = &st.mp4.audio_frames[st.mp4_audio_pos];
				int64_t rel = af->pts_us - st.mp4.audio_frames[0].pts_us;
				if (now >= st.epoch + rel) {
					rss_ring_publish(st.audio_ring,
							 st.mp4.file_data + af->file_offset,
							 af->length, st.epoch + rel,
							 (uint16_t)st.mp4.audio_codec_id, 0);
					st.mp4_audio_pos++;
					st.audio_published++;
					did_work = true;
				}
			}
		} else {
			/* ── Raw Annex B + PCM path ── */
			if (st.video_ring && st.frame_pos < st.frame_count) {
				int64_t target = st.epoch + (int64_t)st.frame_pos * 1000000LL /
								    st.settings.fps;
				if (now >= target) {
					rfs_frame_t *f = &st.frames[st.frame_pos];
					int64_t pts = st.epoch + (int64_t)f->display_pos *
									 1000000LL /
									 st.settings.fps;
					rss_ring_publish(st.video_ring, st.video_data + f->offset,
							 f->length, pts, f->nal_type, f->is_key);
					st.frame_pos++;
					st.video_published++;
					did_work = true;
				}
			}
			if (st.audio_ring && st.audio_pos < st.audio_size) {
				int64_t target = st.epoch + (int64_t)st.audio_published *
								    RFS_AUDIO_CHUNK_MS * 1000LL;
				if (now >= target) {
					uint32_t remain = (uint32_t)(st.audio_size - st.audio_pos);
					uint32_t chunk_bytes = st.audio_chunk_samples * 2;
					if (chunk_bytes > remain)
						chunk_bytes = remain & ~1u;
					int samples = (int)(chunk_bytes / 2);
					memcpy(st.pcm_chunk, st.audio_data + st.audio_pos,
					       chunk_bytes);
					int encoded = st.audio_ops->encode(
						&st.audio_codec, st.pcm_chunk, samples,
						st.encode_buf, st.audio_codec.encode_buf_size, now);
					if (encoded > 0)
						rss_ring_publish(st.audio_ring, st.encode_buf,
								 (uint32_t)encoded, now,
								 (uint16_t)st.audio_codec.codec_id,
								 0);
					st.audio_pos += chunk_bytes;
					st.audio_published++;
					did_work = true;
				}
			}
		}

		/* Flush trailing audio when video is done (audio PTS may extend slightly past
		 * video) */
		if (st.is_mp4 && !st.audio_ops &&
		    (!st.video_ring || st.frame_pos >= st.frame_count)) {
			while (st.audio_ring && st.mp4_audio_pos < st.mp4.audio_count) {
				rfs_mp4_frame_t *af = &st.mp4.audio_frames[st.mp4_audio_pos];
				rss_ring_publish(st.audio_ring, st.mp4.file_data + af->file_offset,
						 af->length, now, (uint16_t)st.mp4.audio_codec_id,
						 0);
				st.mp4_audio_pos++;
				st.audio_published++;
			}
		}

		/* Check for EOF / loop */
		bool vdone = !st.video_ring || st.frame_pos >= st.frame_count;
		bool adone;
		if (st.audio_ops)
			adone = !st.audio_ring || st.audio_pos + 1 >= st.audio_size;
		else if (st.is_mp4)
			adone = !st.audio_ring || st.mp4_audio_pos >= st.mp4.audio_count;
		else
			adone = !st.audio_ring || st.audio_pos + 1 >= st.audio_size;
		if (vdone && adone) {
			if (st.settings.loop) {
				st.frame_pos = 0;
				st.audio_pos = 0;
				st.mp4_audio_pos = 0;
				st.epoch = rss_timestamp_us();
				st.video_published = 0;
				st.audio_published = 0;
				RSS_DEBUG("loop restart");
				continue;
			}
			RSS_INFO("end of file");
			while (*st.running) {
				if (epoll_fd >= 0) {
					struct epoll_event ev;
					int n = epoll_wait(epoll_fd, &ev, 1, 1000);
					if (n > 0 && st.ctrl)
						rss_ctrl_accept_and_handle(st.ctrl, ctrl_handler,
									   &st);
				} else {
					usleep(1000000);
				}
			}
			break;
		}

		/* Sleep until next frame/chunk, handle ctrl socket meanwhile */
		if (!did_work) {
			int64_t next_v = INT64_MAX, next_a = INT64_MAX;
			if (st.video_ring && st.frame_pos < st.frame_count) {
				if (st.is_mp4) {
					rfs_mp4_frame_t *mf = &st.mp4.video_frames[st.frame_pos];
					next_v = st.epoch + mf->pts_us -
						 st.mp4.video_frames[0].pts_us;
				} else {
					next_v = st.epoch + (int64_t)st.frame_pos * 1000000LL /
								    st.settings.fps;
				}
			}
			if (st.audio_ops && st.audio_ring && st.audio_pos < st.audio_size) {
				next_a = st.epoch +
					 (int64_t)st.audio_published * RFS_AUDIO_CHUNK_MS * 1000LL;
			} else if (st.is_mp4 && st.audio_ring &&
				   st.mp4_audio_pos < st.mp4.audio_count) {
				rfs_mp4_frame_t *af = &st.mp4.audio_frames[st.mp4_audio_pos];
				next_a = st.epoch + af->pts_us - st.mp4.audio_frames[0].pts_us;
			} else if (st.audio_ring && st.audio_pos < st.audio_size) {
				next_a = st.epoch +
					 (int64_t)st.audio_published * RFS_AUDIO_CHUNK_MS * 1000LL;
			}

			int64_t next = (next_v < next_a) ? next_v : next_a;
			now = rss_timestamp_us();
			int wait_ms = (next > now) ? (int)((next - now + 999) / 1000) : 0;
			if (wait_ms > RFS_CTRL_POLL_MS)
				wait_ms = RFS_CTRL_POLL_MS;

			struct epoll_event ev;
			int n = epoll_wait(epoll_fd, &ev, 1, wait_ms);
			if (n > 0 && st.ctrl)
				rss_ctrl_accept_and_handle(st.ctrl, ctrl_handler, &st);
		}
	}

	RSS_INFO("rfs shutting down (%" PRIu64 " video frames, %" PRIu64 " audio chunks published)",
		 st.video_published, st.audio_published);

cleanup:
	if (st.audio_ops && st.audio_ops->deinit)
		st.audio_ops->deinit(&st.audio_codec);
	free(st.encode_buf);
	free(st.pcm_chunk);
	if (st.video_ring)
		rss_ring_destroy(st.video_ring);
	if (st.audio_ring)
		rss_ring_destroy(st.audio_ring);
	free(st.scratch);
	if (st.is_mp4)
		rfs_mp4_close(&st.mp4);
	if (st.video_data)
		munmap(st.video_data, st.video_size);
	if (st.audio_data && st.audio_data_owned)
		munmap(st.audio_data, st.audio_size);
	free(st.frames);
	if (epoll_fd >= 0)
		close(epoll_fd);
	if (st.ctrl)
		rss_ctrl_destroy(st.ctrl);
	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("rfs");
	return 0;
}
