/*
 * rmr_mux.c -- Fragmented MP4 muxer
 *
 * Writes ISO BMFF fragmented MP4 (fMP4) with:
 *   - ftyp + moov (init segment, written once)
 *   - moof + mdat pairs (media segments, one per GOP)
 *
 * Each moof+mdat is self-contained. If the file is truncated at any
 * moof boundary, all prior fragments are playable.
 *
 * Box format: [4-byte size][4-byte type][payload]
 * All multi-byte integers are big-endian per ISO 14496-12.
 */

#include "rmr_mux.h"

#include <stdlib.h>
#include <string.h>

/* ── Internal sample tracking ── */

typedef struct {
	uint32_t size;	    /* sample size in bytes */
	uint32_t duration;  /* in timescale units */
	uint32_t flags;	    /* 0x02000000 = key (depends_on=2), 0x00000000 = non-key */
	int32_t cts_offset; /* composition time offset (pts - dts) */
} mux_sample_t;

#define MUX_MAX_SAMPLES 2048 /* max samples per fragment (~80s at 25fps) */

struct rmr_mux {
	rmr_write_fn write_fn;
	void *write_ctx;

	/* Track config */
	bool has_video;
	bool has_audio;
	rmr_video_params_t video;
	rmr_audio_params_t audio;

	/* Codec config (raw NAL payloads, no start codes) */
	uint8_t sps[256];
	uint32_t sps_len;
	uint8_t pps[128];
	uint32_t pps_len;
	uint8_t vps[256];
	uint32_t vps_len;

	/* Fragment state */
	uint32_t frag_seq; /* moof sequence number (1-based) */

	/* Video samples for current fragment */
	mux_sample_t v_samples[MUX_MAX_SAMPLES];
	uint32_t v_count;
	int64_t v_base_dts; /* first video DTS in this fragment */

	/* Audio samples for current fragment */
	mux_sample_t a_samples[MUX_MAX_SAMPLES];
	uint32_t a_count;
	int64_t a_base_dts;

	/* Fragment data accumulators (separate for correct mdat layout) */
	uint8_t *v_data;
	uint32_t v_data_len;
	uint32_t v_data_cap;
	uint8_t *a_data;
	uint32_t a_data_len;
	uint32_t a_data_cap;

	/* Running totals for duration tracking */
	int64_t v_duration; /* total video duration in timescale */
	int64_t a_duration;

	/* Scratch buffer for building boxes */
	uint8_t *box_buf;
	uint32_t box_len;
	uint32_t box_cap;

	/* Total bytes written (for mfra offsets) */
	uint64_t bytes_written;
};

/* ── Big-endian write helpers ── */

static inline void w16(uint8_t *p, uint16_t v)
{
	p[0] = (v >> 8) & 0xFF;
	p[1] = v & 0xFF;
}

static inline void w32(uint8_t *p, uint32_t v)
{
	p[0] = (v >> 24) & 0xFF;
	p[1] = (v >> 16) & 0xFF;
	p[2] = (v >> 8) & 0xFF;
	p[3] = v & 0xFF;
}

static inline void w64(uint8_t *p, uint64_t v)
{
	w32(p, (uint32_t)(v >> 32));
	w32(p + 4, (uint32_t)v);
}

/* ── Box buffer helpers ── */

static void bb_reset(rmr_mux_t *m)
{
	m->box_len = 0;
}

static int bb_ensure(rmr_mux_t *m, uint32_t need)
{
	/* H6: overflow check */
	if (need > UINT32_MAX - m->box_len)
		return -1;
	uint32_t req = m->box_len + need;
	if (req <= m->box_cap)
		return 0;
	uint32_t cap = m->box_cap ? m->box_cap : 4096;
	while (cap < req) {
		if (cap > UINT32_MAX / 2) /* M10: prevent infinite loop */
			return -1;
		cap *= 2;
	}
	uint8_t *p = realloc(m->box_buf, cap);
	if (!p)
		return -1;
	m->box_buf = p;
	m->box_cap = cap;
	return 0;
}

static int bb_write(rmr_mux_t *m, const void *data, uint32_t len)
{
	if (bb_ensure(m, len) < 0)
		return -1;
	memcpy(m->box_buf + m->box_len, data, len);
	m->box_len += len;
	return 0;
}

static int bb_w8(rmr_mux_t *m, uint8_t v)
{
	return bb_write(m, &v, 1);
}

static int bb_w16(rmr_mux_t *m, uint16_t v)
{
	uint8_t b[2];
	w16(b, v);
	return bb_write(m, b, 2);
}

static int bb_w32(rmr_mux_t *m, uint32_t v)
{
	uint8_t b[4];
	w32(b, v);
	return bb_write(m, b, 4);
}

static int bb_w64(rmr_mux_t *m, uint64_t v)
{
	uint8_t b[8];
	w64(b, v);
	return bb_write(m, b, 8);
}

static int bb_fourcc(rmr_mux_t *m, const char *cc)
{
	return bb_write(m, cc, 4);
}

static int bb_zeros(rmr_mux_t *m, uint32_t n)
{
	if (bb_ensure(m, n) < 0)
		return -1;
	memset(m->box_buf + m->box_len, 0, n);
	m->box_len += n;
	return 0;
}

/* Begin a box: write placeholder size + fourcc, return offset */
static uint32_t bb_box_begin(rmr_mux_t *m, const char *fourcc)
{
	uint32_t off = m->box_len;
	bb_w32(m, 0); /* size placeholder */
	bb_fourcc(m, fourcc);
	return off;
}

/* End a box: patch size at saved offset */
static void bb_box_end(rmr_mux_t *m, uint32_t off)
{
	uint32_t size = m->box_len - off;
	w32(m->box_buf + off, size);
}

/* Begin a full box (with version + flags) */
static uint32_t bb_fullbox_begin(rmr_mux_t *m, const char *fourcc, uint8_t version, uint32_t flags)
{
	uint32_t off = bb_box_begin(m, fourcc);
	uint32_t vf = ((uint32_t)version << 24) | (flags & 0x00FFFFFF);
	bb_w32(m, vf);
	return off;
}

/* Flush box buffer to write callback */
static int bb_flush(rmr_mux_t *m)
{
	if (m->box_len == 0)
		return 0;
	int ret = m->write_fn(m->box_buf, m->box_len, m->write_ctx);
	m->bytes_written += m->box_len;
	m->box_len = 0;
	return ret;
}

/* ── Fragment data accumulators ── */

static int buf_append(uint8_t **buf, uint32_t *buf_len, uint32_t *buf_cap, const uint8_t *data,
		      uint32_t len)
{
	/* H5: overflow check */
	if (len > UINT32_MAX - *buf_len)
		return -1;
	uint32_t need = *buf_len + len;
	if (need > *buf_cap) {
		uint32_t cap = *buf_cap ? *buf_cap : 256 * 1024;
		while (cap < need) {
			if (cap > UINT32_MAX / 2) /* M10: prevent infinite loop */
				return -1;
			cap *= 2;
		}
		uint8_t *p = realloc(*buf, cap);
		if (!p)
			return -1;
		*buf = p;
		*buf_cap = cap;
	}
	memcpy(*buf + *buf_len, data, len);
	*buf_len += len;
	return 0;
}

/* ── Box writers ── */

static int write_ftyp(rmr_mux_t *m)
{
	bb_reset(m);
	uint32_t off = bb_box_begin(m, "ftyp");
	bb_fourcc(m, "isom"); /* major brand */
	bb_w32(m, 0x200);     /* minor version */
	bb_fourcc(m, "isom"); /* compatible brands */
	bb_fourcc(m, "iso5");
	bb_fourcc(m, "iso6");
	bb_fourcc(m, "mp41");
	bb_box_end(m, off);
	return bb_flush(m);
}

/* mvhd — movie header */
static void write_mvhd(rmr_mux_t *m, uint32_t timescale, uint32_t next_track_id)
{
	uint32_t off = bb_fullbox_begin(m, "mvhd", 0, 0);
	bb_w32(m, 0); /* creation_time */
	bb_w32(m, 0); /* modification_time */
	bb_w32(m, timescale);
	bb_w32(m, 0);	       /* duration (unknown for fMP4) */
	bb_w32(m, 0x00010000); /* rate = 1.0 */
	bb_w16(m, 0x0100);     /* volume = 1.0 */
	bb_zeros(m, 10);       /* reserved */
	/* unity matrix (3x3 fixed-point) */
	bb_w32(m, 0x00010000);
	bb_zeros(m, 4);
	bb_zeros(m, 4);
	bb_zeros(m, 4);
	bb_w32(m, 0x00010000);
	bb_zeros(m, 4);
	bb_zeros(m, 4);
	bb_zeros(m, 4);
	bb_w32(m, 0x40000000);
	bb_zeros(m, 24); /* pre_defined */
	bb_w32(m, next_track_id);
	bb_box_end(m, off);
}

/* tkhd — track header */
static void write_tkhd(rmr_mux_t *m, uint32_t track_id, uint16_t width, uint16_t height,
		       bool is_audio)
{
	uint32_t off = bb_fullbox_begin(m, "tkhd", 0, 0x03); /* enabled + in_movie */
	bb_w32(m, 0);					     /* creation_time */
	bb_w32(m, 0);					     /* modification_time */
	bb_w32(m, track_id);
	bb_w32(m, 0);			  /* reserved */
	bb_w32(m, 0);			  /* duration */
	bb_zeros(m, 8);			  /* reserved */
	bb_w16(m, 0);			  /* layer */
	bb_w16(m, is_audio ? 1 : 0);	  /* alternate_group */
	bb_w16(m, is_audio ? 0x0100 : 0); /* volume */
	bb_w16(m, 0);			  /* reserved */
	/* unity matrix */
	bb_w32(m, 0x00010000);
	bb_zeros(m, 4);
	bb_zeros(m, 4);
	bb_zeros(m, 4);
	bb_w32(m, 0x00010000);
	bb_zeros(m, 4);
	bb_zeros(m, 4);
	bb_zeros(m, 4);
	bb_w32(m, 0x40000000);
	/* width/height in 16.16 fixed point */
	bb_w32(m, (uint32_t)width << 16);
	bb_w32(m, (uint32_t)height << 16);
	bb_box_end(m, off);
}

/* mdhd — media header */
static void write_mdhd(rmr_mux_t *m, uint32_t timescale)
{
	uint32_t off = bb_fullbox_begin(m, "mdhd", 0, 0);
	bb_w32(m, 0); /* creation_time */
	bb_w32(m, 0); /* modification_time */
	bb_w32(m, timescale);
	bb_w32(m, 0);	   /* duration */
	bb_w16(m, 0x55C4); /* language: und */
	bb_w16(m, 0);	   /* pre_defined */
	bb_box_end(m, off);
}

/* hdlr — handler reference */
static void write_hdlr(rmr_mux_t *m, const char *handler_type, const char *name)
{
	uint32_t off = bb_fullbox_begin(m, "hdlr", 0, 0);
	bb_w32(m, 0); /* pre_defined */
	bb_fourcc(m, handler_type);
	bb_zeros(m, 12); /* reserved */
	bb_write(m, (const uint8_t *)name, (uint32_t)strlen(name) + 1);
	bb_box_end(m, off);
}

/* stsd — sample description (H.264) */
static void write_stsd_avc1(rmr_mux_t *m)
{
	uint32_t stsd_off = bb_fullbox_begin(m, "stsd", 0, 0);
	bb_w32(m, 1); /* entry_count */

	uint32_t avc1_off = bb_box_begin(m, "avc1");
	bb_zeros(m, 6);	 /* reserved */
	bb_w16(m, 1);	 /* data_reference_index */
	bb_zeros(m, 16); /* pre_defined + reserved */
	bb_w16(m, m->video.width);
	bb_w16(m, m->video.height);
	bb_w32(m, 0x00480000); /* horiz resolution 72 dpi */
	bb_w32(m, 0x00480000); /* vert resolution 72 dpi */
	bb_w32(m, 0);	       /* reserved */
	bb_w16(m, 1);	       /* frame_count */
	bb_zeros(m, 32);       /* compressorname */
	bb_w16(m, 0x0018);     /* depth = 24 */
	bb_w16(m, 0xFFFF);     /* pre_defined = -1 */

	/* avcC — AVC decoder configuration record */
	uint32_t avcc_off = bb_box_begin(m, "avcC");
	bb_w8(m, 1);				    /* configurationVersion */
	bb_w8(m, m->sps_len > 1 ? m->sps[1] : 100); /* AVCProfileIndication */
	bb_w8(m, m->sps_len > 2 ? m->sps[2] : 0);   /* profile_compatibility */
	bb_w8(m, m->sps_len > 3 ? m->sps[3] : 40);  /* AVCLevelIndication */
	bb_w8(m, 0xFF);				    /* lengthSizeMinusOne = 3 (4-byte lengths) */
	bb_w8(m, 0xE1);				    /* numOfSequenceParameterSets = 1 */
	bb_w16(m, (uint16_t)m->sps_len);
	bb_write(m, m->sps, m->sps_len);
	bb_w8(m, 1); /* numOfPictureParameterSets */
	bb_w16(m, (uint16_t)m->pps_len);
	bb_write(m, m->pps, m->pps_len);
	bb_box_end(m, avcc_off);

	bb_box_end(m, avc1_off);
	bb_box_end(m, stsd_off);
}

/* stsd — sample description (H.265) */
static void write_stsd_hev1(rmr_mux_t *m)
{
	uint32_t stsd_off = bb_fullbox_begin(m, "stsd", 0, 0);
	bb_w32(m, 1); /* entry_count */

	uint32_t hev1_off = bb_box_begin(m, "hev1");
	bb_zeros(m, 6);	 /* reserved */
	bb_w16(m, 1);	 /* data_reference_index */
	bb_zeros(m, 16); /* pre_defined + reserved */
	bb_w16(m, m->video.width);
	bb_w16(m, m->video.height);
	bb_w32(m, 0x00480000);
	bb_w32(m, 0x00480000);
	bb_w32(m, 0);
	bb_w16(m, 1);
	bb_zeros(m, 32);
	bb_w16(m, 0x0018);
	bb_w16(m, 0xFFFF);

	/* hvcC — HEVC decoder configuration record */
	uint32_t hvcc_off = bb_box_begin(m, "hvcC");
	bb_w8(m, 1); /* configurationVersion */
	/* Extract profile/tier/level from SPS if available */
	uint8_t general_profile_space = 0;
	uint8_t general_tier_flag = 0;
	uint8_t general_profile_idc = 1; /* Main */
	uint32_t general_profile_compat = 0x60000000;
	uint8_t general_level_idc = 120; /* Level 4.0 */
	uint8_t b = (general_profile_space << 6) | (general_tier_flag << 5) | general_profile_idc;
	bb_w8(m, b);
	bb_w32(m, general_profile_compat);
	bb_zeros(m, 6); /* general_constraint_indicator (48 bits) */
	bb_w8(m, general_level_idc);
	bb_w16(m, 0xF000);  /* min_spatial_segmentation_idc = 0 */
	bb_w8(m, 0xFC);	    /* parallelismType = 0 */
	bb_w8(m, 0xFC | 1); /* chromaFormat = 1 (4:2:0) */
	bb_w8(m, 0xF8);	    /* bitDepthLumaMinus8 = 0 */
	bb_w8(m, 0xF8);	    /* bitDepthChromaMinus8 = 0 */
	bb_w16(m, 0);	    /* avgFrameRate */
	bb_w8(m, 0x0F);	    /* constantFrameRate=0, numTemporalLayers=1, lengthSizeMinusOne=3 */
	/* numOfArrays: VPS + SPS + PPS */
	uint8_t num_arrays = 0;
	if (m->vps_len > 0)
		num_arrays++;
	if (m->sps_len > 0)
		num_arrays++;
	if (m->pps_len > 0)
		num_arrays++;
	bb_w8(m, num_arrays);
	if (m->vps_len > 0) {
		bb_w8(m, 0x20 | 32); /* array_completeness=1, NAL_unit_type=VPS(32) */
		bb_w16(m, 1);	     /* numNalus */
		bb_w16(m, (uint16_t)m->vps_len);
		bb_write(m, m->vps, m->vps_len);
	}
	if (m->sps_len > 0) {
		bb_w8(m, 0x20 | 33); /* SPS(33) */
		bb_w16(m, 1);
		bb_w16(m, (uint16_t)m->sps_len);
		bb_write(m, m->sps, m->sps_len);
	}
	if (m->pps_len > 0) {
		bb_w8(m, 0x20 | 34); /* PPS(34) */
		bb_w16(m, 1);
		bb_w16(m, (uint16_t)m->pps_len);
		bb_write(m, m->pps, m->pps_len);
	}
	bb_box_end(m, hvcc_off);

	bb_box_end(m, hev1_off);
	bb_box_end(m, stsd_off);
}

/* AAC AudioSpecificConfig — compute from sample rate + AAC-LC + mono */
static uint16_t aac_audio_specific_config(uint32_t sample_rate)
{
	static const int sr_table[] = {96000, 88200, 64000, 48000, 44100, 32000,
				       24000, 22050, 16000, 12000, 11025, 8000, 7350};
	int sr_idx = 4; /* default 44100 */
	for (int i = 0; i < 13; i++) {
		if (sr_table[i] == (int)sample_rate) {
			sr_idx = i;
			break;
		}
	}
	return (uint16_t)((2 << 11) | (sr_idx << 7) | (1 << 3)); /* AAC-LC, mono */
}

/* esds box for AAC in MP4 */
static void write_esds(rmr_mux_t *m, uint16_t asc)
{
	uint32_t esds_off = bb_fullbox_begin(m, "esds", 0, 0);

	/* ES_Descriptor */
	bb_w8(m, 0x03);	 /* tag: ES_DescrTag */
	bb_w8(m, 23);		 /* length */
	bb_w16(m, 1);		 /* ES_ID */
	bb_w8(m, 0);		 /* stream priority */

	/* DecoderConfigDescriptor */
	bb_w8(m, 0x04);	 /* tag: DecoderConfigDescrTag */
	bb_w8(m, 15);		 /* length */
	bb_w8(m, 0x40);	 /* objectTypeIndication: Audio ISO/IEC 14496-3 */
	bb_w8(m, 0x15);	 /* streamType: audio (5<<2 | 1) */
	bb_w8(m, 0); bb_w16(m, 0); /* bufferSizeDB (3 bytes) */
	bb_w32(m, 0);		 /* maxBitrate (0 = unknown) */
	bb_w32(m, 0);		 /* avgBitrate (0 = unknown) */

	/* DecoderSpecificInfo (AudioSpecificConfig) */
	bb_w8(m, 0x05);	 /* tag: DecSpecificInfoTag */
	bb_w8(m, 2);		 /* length */
	bb_w8(m, (uint8_t)(asc >> 8));
	bb_w8(m, (uint8_t)(asc & 0xFF));

	/* SLConfigDescriptor */
	bb_w8(m, 0x06);	 /* tag: SLConfigDescrTag */
	bb_w8(m, 1);		 /* length */
	bb_w8(m, 0x02);	 /* predefined: MP4 */

	bb_box_end(m, esds_off);
}

/* dOps box for Opus in MP4 (RFC 7845 / Opus in ISOBMFF) */
static void write_dops(rmr_mux_t *m, uint32_t sample_rate)
{
	uint32_t dops_off = bb_box_begin(m, "dOps");
	bb_w8(m, 0);			/* Version */
	bb_w8(m, 1);			/* OutputChannelCount (mono) */
	bb_w16(m, 312);		/* PreSkip (typical for Opus) */
	bb_w32(m, sample_rate);		/* InputSampleRate */
	bb_w16(m, 0);			/* OutputGain (0 dB) */
	bb_w8(m, 0);			/* ChannelMappingFamily (0 = mono/stereo) */
	bb_box_end(m, dops_off);
}

/* stsd — sample description (audio) */
static void write_stsd_audio(rmr_mux_t *m)
{
	uint32_t stsd_off = bb_fullbox_begin(m, "stsd", 0, 0);
	bb_w32(m, 1); /* entry_count */

	if (m->audio.codec == RMR_AUDIO_AAC) {
		/* mp4a + esds */
		uint32_t entry_off = bb_box_begin(m, "mp4a");
		bb_zeros(m, 6);				       /* reserved */
		bb_w16(m, 1);				       /* data_reference_index */
		bb_zeros(m, 8);				       /* reserved */
		bb_w16(m, m->audio.channels);
		bb_w16(m, 16);				       /* sample_size */
		bb_w16(m, 0);				       /* compression_id */
		bb_w16(m, 0);				       /* packet_size */
		bb_w32(m, m->audio.sample_rate << 16);	       /* sample_rate 16.16 */
		write_esds(m, aac_audio_specific_config(m->audio.sample_rate));
		bb_box_end(m, entry_off);
	} else if (m->audio.codec == RMR_AUDIO_OPUS) {
		/* Opus + dOps */
		uint32_t entry_off = bb_box_begin(m, "Opus");
		bb_zeros(m, 6);				       /* reserved */
		bb_w16(m, 1);				       /* data_reference_index */
		bb_zeros(m, 8);				       /* reserved */
		bb_w16(m, m->audio.channels);
		bb_w16(m, 16);				       /* sample_size */
		bb_w16(m, 0);				       /* compression_id */
		bb_w16(m, 0);				       /* packet_size */
		bb_w32(m, 48000 << 16);			       /* sample_rate 16.16 (always 48kHz) */
		write_dops(m, m->audio.sample_rate);
		bb_box_end(m, entry_off);
	} else {
		/* PCM: ulaw/alaw/twos */
		const char *fourcc;
		switch (m->audio.codec) {
		case RMR_AUDIO_PCMU:
			fourcc = "ulaw";
			break;
		case RMR_AUDIO_PCMA:
			fourcc = "alaw";
			break;
		default:
			fourcc = "twos";
			break;
		}
		uint32_t entry_off = bb_box_begin(m, fourcc);
		bb_zeros(m, 6);
		bb_w16(m, 1);
		bb_zeros(m, 8);
		bb_w16(m, m->audio.channels);
		bb_w16(m, m->audio.bits_per_sample);
		bb_w16(m, 0);
		bb_w16(m, 0);
		bb_w32(m, m->audio.sample_rate << 16);
		bb_box_end(m, entry_off);
	}

	bb_box_end(m, stsd_off);
}

/* Empty stbl required by fMP4 spec (sample tables in moof, not moov) */
static void write_empty_stbl(rmr_mux_t *m, bool is_video)
{
	uint32_t stbl_off = bb_box_begin(m, "stbl");

	/* stsd */
	if (is_video) {
		if (m->video.codec == RMR_CODEC_H265)
			write_stsd_hev1(m);
		else
			write_stsd_avc1(m);
	} else {
		write_stsd_audio(m);
	}

	/* stts — empty */
	uint32_t off = bb_fullbox_begin(m, "stts", 0, 0);
	bb_w32(m, 0); /* entry_count */
	bb_box_end(m, off);

	/* stsc — empty */
	off = bb_fullbox_begin(m, "stsc", 0, 0);
	bb_w32(m, 0);
	bb_box_end(m, off);

	/* stsz — empty */
	off = bb_fullbox_begin(m, "stsz", 0, 0);
	bb_w32(m, 0); /* sample_size */
	bb_w32(m, 0); /* sample_count */
	bb_box_end(m, off);

	/* stco — empty */
	off = bb_fullbox_begin(m, "stco", 0, 0);
	bb_w32(m, 0);
	bb_box_end(m, off);

	bb_box_end(m, stbl_off);
}

/* Write a complete track box */
static void write_trak(rmr_mux_t *m, uint32_t track_id, bool is_audio)
{
	uint32_t trak_off = bb_box_begin(m, "trak");

	if (is_audio) {
		write_tkhd(m, track_id, 0, 0, true);
	} else {
		write_tkhd(m, track_id, m->video.width, m->video.height, false);
	}

	uint32_t mdia_off = bb_box_begin(m, "mdia");
	write_mdhd(m, is_audio ? m->audio.sample_rate : m->video.timescale);
	write_hdlr(m, is_audio ? "soun" : "vide", is_audio ? "Raptor Audio" : "Raptor Video");

	uint32_t minf_off = bb_box_begin(m, "minf");

	if (is_audio) {
		uint32_t smhd_off = bb_fullbox_begin(m, "smhd", 0, 0);
		bb_w16(m, 0); /* balance */
		bb_w16(m, 0); /* reserved */
		bb_box_end(m, smhd_off);
	} else {
		uint32_t vmhd_off = bb_fullbox_begin(m, "vmhd", 0, 1);
		bb_w16(m, 0);	/* graphicsmode */
		bb_zeros(m, 6); /* opcolor */
		bb_box_end(m, vmhd_off);
	}

	/* dinf + dref */
	uint32_t dinf_off = bb_box_begin(m, "dinf");
	uint32_t dref_off = bb_fullbox_begin(m, "dref", 0, 0);
	bb_w32(m, 1);					      /* entry_count */
	uint32_t url_off = bb_fullbox_begin(m, "url ", 0, 1); /* self-contained */
	bb_box_end(m, url_off);
	bb_box_end(m, dref_off);
	bb_box_end(m, dinf_off);

	write_empty_stbl(m, !is_audio);

	bb_box_end(m, minf_off);
	bb_box_end(m, mdia_off);
	bb_box_end(m, trak_off);
}

/* mvex + trex boxes (required for fMP4) */
static void write_mvex(rmr_mux_t *m)
{
	uint32_t mvex_off = bb_box_begin(m, "mvex");

	/* Video trex */
	if (m->has_video) {
		uint32_t off = bb_fullbox_begin(m, "trex", 0, 0);
		bb_w32(m, 1); /* track_id */
		bb_w32(m, 1); /* default_sample_description_index */
		bb_w32(m, 0); /* default_sample_duration */
		bb_w32(m, 0); /* default_sample_size */
		bb_w32(m, 0); /* default_sample_flags */
		bb_box_end(m, off);
	}

	/* Audio trex */
	if (m->has_audio) {
		uint32_t off = bb_fullbox_begin(m, "trex", 0, 0);
		bb_w32(m, 2); /* track_id */
		bb_w32(m, 1);
		bb_w32(m, 0);
		bb_w32(m, 0);
		bb_w32(m, 0);
		bb_box_end(m, off);
	}

	bb_box_end(m, mvex_off);
}

static int write_moov(rmr_mux_t *m)
{
	bb_reset(m);
	uint32_t moov_off = bb_box_begin(m, "moov");

	uint32_t next_track = 1;
	if (m->has_video)
		next_track++;
	if (m->has_audio)
		next_track++;

	write_mvhd(m, m->has_video ? m->video.timescale : m->audio.sample_rate, next_track);

	if (m->has_video)
		write_trak(m, 1, false);
	if (m->has_audio)
		write_trak(m, 2, true);

	write_mvex(m);

	bb_box_end(m, moov_off);
	return bb_flush(m);
}

/* ── Fragment writing (moof + mdat) ── */

static int write_fragment(rmr_mux_t *m)
{
	if (m->v_count == 0 && m->a_count == 0)
		return 0;

	m->frag_seq++;

	/* Calculate video data size and audio data offset */
	uint32_t v_data_size = 0;
	for (uint32_t i = 0; i < m->v_count; i++)
		v_data_size += m->v_samples[i].size;

	uint32_t a_data_size = 0;
	for (uint32_t i = 0; i < m->a_count; i++)
		a_data_size += m->a_samples[i].size;

	/* Build moof */
	bb_reset(m);
	uint32_t moof_off = bb_box_begin(m, "moof");

	/* mfhd */
	uint32_t mfhd_off = bb_fullbox_begin(m, "mfhd", 0, 0);
	bb_w32(m, m->frag_seq);
	bb_box_end(m, mfhd_off);

	/* We need to know moof size to compute data_offset in trun.
	 * data_offset = distance from moof start to mdat payload.
	 * We'll patch it after building the moof. Save the trun offset. */
	uint32_t v_trun_data_offset_pos = 0;
	uint32_t a_trun_data_offset_pos = 0;

	/* Video traf */
	if (m->v_count > 0) {
		uint32_t traf_off = bb_box_begin(m, "traf");

		/* tfhd: track_id=1, default-base-is-moof flag */
		uint32_t tfhd_off = bb_fullbox_begin(m, "tfhd", 0, 0x020000);
		bb_w32(m, 1); /* track_id */
		bb_box_end(m, tfhd_off);

		/* tfdt: base_media_decode_time (version 1 = 64-bit) */
		uint32_t tfdt_off = bb_fullbox_begin(m, "tfdt", 1, 0);
		bb_w64(m, (uint64_t)m->v_base_dts);
		bb_box_end(m, tfdt_off);

		/* trun: per-sample duration, size, flags, cts_offset */
		uint32_t trun_flags = 0x000001	  /* data_offset_present */
				      | 0x000100  /* sample_duration_present */
				      | 0x000200  /* sample_size_present */
				      | 0x000400  /* sample_flags_present */
				      | 0x000800; /* sample_composition_time_offsets_present */

		uint32_t trun_off = bb_fullbox_begin(m, "trun", 0, trun_flags);
		bb_w32(m, m->v_count); /* sample_count */
		v_trun_data_offset_pos = m->box_len;
		bb_w32(m, 0); /* data_offset placeholder */
		for (uint32_t i = 0; i < m->v_count; i++) {
			bb_w32(m, m->v_samples[i].duration);
			bb_w32(m, m->v_samples[i].size);
			bb_w32(m, m->v_samples[i].flags);
			bb_w32(m, (uint32_t)m->v_samples[i].cts_offset);
		}
		bb_box_end(m, trun_off);

		bb_box_end(m, traf_off);
	}

	/* Audio traf */
	if (m->a_count > 0) {
		uint32_t traf_off = bb_box_begin(m, "traf");

		uint32_t tfhd_off = bb_fullbox_begin(m, "tfhd", 0, 0x020000);
		bb_w32(m, 2); /* track_id */
		bb_box_end(m, tfhd_off);

		uint32_t tfdt_off = bb_fullbox_begin(m, "tfdt", 1, 0);
		bb_w64(m, (uint64_t)m->a_base_dts);
		bb_box_end(m, tfdt_off);

		uint32_t trun_flags = 0x000001 | 0x000100 | 0x000200;
		uint32_t trun_off = bb_fullbox_begin(m, "trun", 0, trun_flags);
		bb_w32(m, m->a_count);
		a_trun_data_offset_pos = m->box_len;
		bb_w32(m, 0); /* data_offset placeholder */
		for (uint32_t i = 0; i < m->a_count; i++) {
			bb_w32(m, m->a_samples[i].duration);
			bb_w32(m, m->a_samples[i].size);
		}
		bb_box_end(m, trun_off);

		bb_box_end(m, traf_off);
	}

	bb_box_end(m, moof_off);

	/* Now compute data_offset values.
	 * data_offset = moof_size + 8 (mdat header) for video (first track in mdat).
	 * Audio starts after all video data. */
	uint32_t moof_size = m->box_len;
	uint32_t mdat_header = 8;

	if (v_trun_data_offset_pos > 0) {
		int32_t v_offset = (int32_t)(moof_size + mdat_header);
		w32(m->box_buf + v_trun_data_offset_pos, (uint32_t)v_offset);
	}
	if (a_trun_data_offset_pos > 0) {
		int32_t a_offset = (int32_t)(moof_size + mdat_header + v_data_size);
		w32(m->box_buf + a_trun_data_offset_pos, (uint32_t)a_offset);
	}

	/* Write moof */
	if (bb_flush(m) < 0)
		return -1;

	/* Write mdat: video data first, then audio data.
	 * Write in chunks to avoid exceeding the write buffer capacity
	 * (GOP-sized video data can exceed 1MB). */
	bb_reset(m);
	uint32_t mdat_size = 8 + m->v_data_len + m->a_data_len;
	bb_w32(m, mdat_size);
	bb_fourcc(m, "mdat");
	if (bb_flush(m) < 0)
		return -1;

	/* Chunked write helper — 64KB at a time */
#define MUX_WRITE_CHUNK 65536
	for (uint32_t off = 0; off < m->v_data_len;) {
		uint32_t chunk = m->v_data_len - off;
		if (chunk > MUX_WRITE_CHUNK)
			chunk = MUX_WRITE_CHUNK;
		int ret = m->write_fn(m->v_data + off, chunk, m->write_ctx);
		if (ret < 0)
			return -1;
		m->bytes_written += chunk;
		off += chunk;
	}
	for (uint32_t off = 0; off < m->a_data_len;) {
		uint32_t chunk = m->a_data_len - off;
		if (chunk > MUX_WRITE_CHUNK)
			chunk = MUX_WRITE_CHUNK;
		int ret = m->write_fn(m->a_data + off, chunk, m->write_ctx);
		if (ret < 0)
			return -1;
		m->bytes_written += chunk;
		off += chunk;
	}

	/* Update running durations */
	for (uint32_t i = 0; i < m->v_count; i++)
		m->v_duration += m->v_samples[i].duration;
	for (uint32_t i = 0; i < m->a_count; i++)
		m->a_duration += m->a_samples[i].duration;

	/* Reset fragment state */
	m->v_count = 0;
	m->a_count = 0;
	m->v_data_len = 0;
	m->a_data_len = 0;

	return 0;
}

/* ── Public API ── */

rmr_mux_t *rmr_mux_create(rmr_write_fn write_fn, void *write_ctx)
{
	if (!write_fn)
		return NULL;
	rmr_mux_t *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->write_fn = write_fn;
	m->write_ctx = write_ctx;
	return m;
}

void rmr_mux_destroy(rmr_mux_t *mux)
{
	if (!mux)
		return;
	free(mux->v_data);
	free(mux->a_data);
	free(mux->box_buf);
	free(mux);
}

int rmr_mux_set_video(rmr_mux_t *mux, const rmr_video_params_t *params, const uint8_t *sps,
		      uint32_t sps_len, const uint8_t *pps, uint32_t pps_len, const uint8_t *vps,
		      uint32_t vps_len)
{
	if (!mux || !params || !sps || !sps_len || !pps || !pps_len)
		return -1;
	mux->video = *params;
	mux->has_video = true;

	memcpy(mux->sps, sps, sps_len < sizeof(mux->sps) ? sps_len : sizeof(mux->sps));
	mux->sps_len = sps_len < sizeof(mux->sps) ? sps_len : sizeof(mux->sps);

	memcpy(mux->pps, pps, pps_len < sizeof(mux->pps) ? pps_len : sizeof(mux->pps));
	mux->pps_len = pps_len < sizeof(mux->pps) ? pps_len : sizeof(mux->pps);

	if (vps && vps_len > 0) {
		memcpy(mux->vps, vps, vps_len < sizeof(mux->vps) ? vps_len : sizeof(mux->vps));
		mux->vps_len = vps_len < sizeof(mux->vps) ? vps_len : sizeof(mux->vps);
	}

	return 0;
}

int rmr_mux_set_audio(rmr_mux_t *mux, const rmr_audio_params_t *params)
{
	if (!mux || !params)
		return -1;
	mux->audio = *params;
	mux->has_audio = true;
	return 0;
}

int rmr_mux_start(rmr_mux_t *mux)
{
	if (!mux || (!mux->has_video && !mux->has_audio))
		return -1;
	if (write_ftyp(mux) < 0)
		return -1;
	if (write_moov(mux) < 0)
		return -1;
	return 0;
}

int rmr_mux_write_video(rmr_mux_t *mux, const rmr_video_sample_t *sample)
{
	if (!mux || !sample || !sample->data || !sample->size)
		return -1;
	if (mux->v_count >= MUX_MAX_SAMPLES)
		return -1;

	/* Set base DTS for this fragment */
	if (mux->v_count == 0)
		mux->v_base_dts = sample->dts;

	/* Calculate duration (will be patched when next sample arrives or at flush) */
	mux_sample_t *s = &mux->v_samples[mux->v_count];
	s->size = sample->size;
	s->flags = sample->is_key ? 0x02000000 : 0x01010000;
	s->cts_offset = (int32_t)(sample->pts - sample->dts);
	s->duration = 0; /* patched below */

	/* Patch previous sample's duration = current_dts - previous_dts */
	if (mux->v_count > 0) {
		int64_t delta = sample->dts - mux->v_base_dts;
		int64_t prev_sum = 0;
		for (uint32_t i = 0; i < mux->v_count; i++)
			prev_sum += mux->v_samples[i].duration;
		mux->v_samples[mux->v_count - 1].duration = (uint32_t)(delta - prev_sum);
	}

	/* Append data to video buffer */
	if (buf_append(&mux->v_data, &mux->v_data_len, &mux->v_data_cap, sample->data,
		       sample->size) < 0)
		return -1;

	mux->v_count++;
	return 0;
}

int rmr_mux_write_audio(rmr_mux_t *mux, const rmr_audio_sample_t *sample)
{
	if (!mux || !sample || !sample->data || !sample->size)
		return -1;
	if (mux->a_count >= MUX_MAX_SAMPLES)
		return -1;

	if (mux->a_count == 0)
		mux->a_base_dts = sample->dts;

	mux_sample_t *s = &mux->a_samples[mux->a_count];
	s->size = sample->size;
	s->duration = 0;

	/* Patch previous duration */
	if (mux->a_count > 0) {
		int64_t delta = sample->dts - mux->a_base_dts;
		int64_t prev_sum = 0;
		for (uint32_t i = 0; i < mux->a_count; i++)
			prev_sum += mux->a_samples[i].duration;
		mux->a_samples[mux->a_count - 1].duration = (uint32_t)(delta - prev_sum);
	}

	if (buf_append(&mux->a_data, &mux->a_data_len, &mux->a_data_cap, sample->data,
		       sample->size) < 0)
		return -1;

	mux->a_count++;
	return 0;
}

int rmr_mux_flush_fragment(rmr_mux_t *mux)
{
	if (!mux)
		return -1;

	/* Patch last video sample duration (use same as previous, or default) */
	if (mux->v_count > 0 && mux->v_samples[mux->v_count - 1].duration == 0) {
		if (mux->v_count > 1)
			mux->v_samples[mux->v_count - 1].duration =
				mux->v_samples[mux->v_count - 2].duration;
		else if (mux->video.timescale > 0 && mux->video.timescale > 0)
			mux->v_samples[0].duration = mux->video.timescale / 25; /* fallback */
	}

	/* Patch last audio sample duration */
	if (mux->a_count > 0 && mux->a_samples[mux->a_count - 1].duration == 0) {
		if (mux->a_count > 1)
			mux->a_samples[mux->a_count - 1].duration =
				mux->a_samples[mux->a_count - 2].duration;
		else
			mux->a_samples[0].duration = mux->audio.sample_rate / 50; /* 20ms */
	}

	return write_fragment(mux);
}

int rmr_mux_finalize(rmr_mux_t *mux)
{
	if (!mux)
		return -1;

	/* Flush any remaining samples */
	if (mux->v_count > 0 || mux->a_count > 0) {
		if (rmr_mux_flush_fragment(mux) < 0)
			return -1;
	}

	/* Write mfra (movie fragment random access) — optional but helps seeking */
	bb_reset(mux);
	uint32_t mfra_off = bb_box_begin(mux, "mfra");

	/* mfro — points back to mfra start */
	uint32_t mfro_off = bb_fullbox_begin(mux, "mfro", 0, 0);
	bb_w32(mux, 0); /* size placeholder — patched after mfra_end */
	bb_box_end(mux, mfro_off);

	bb_box_end(mux, mfra_off);

	/* Patch mfro size to be the mfra box size */
	uint32_t mfra_size = mux->box_len;
	w32(mux->box_buf + mux->box_len - 8, mfra_size);

	return bb_flush(mux);
}
