#ifndef __SAMPLE_COMMON_H__
#define __SAMPLE_COMMON_H__

#include <imp/imp_common.h>
#include <imp/imp_osd.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_system.h>
#include <imp/imp_encoder.h>
#include <unistd.h>

//#define SENSOR_FRAME_RATE_NUM		30
#define SENSOR_FRAME_RATE_DEN	1

//#define SENSOR_GC2053


#define CHN0_EN				1
#define CHN1_EN				0
#define CHN2_EN				0
#define CHN3_EN				0
#define CROP_EN				1

#define SENSOR_WIDTH_SECOND		1280
#define SENSOR_HEIGHT_SECOND	720

#define SENSOR_WIDTH_THIRD		640
#define SENSOR_HEIGHT_THIRD		360

#define BITRATE_720P_Kbs		1000

#define FS_CHN_NUM				4  //MIN 1,MAX 3

#define CH0_INDEX				0
#define CH1_INDEX				1
#define CH2_INDEX				2
#define CH3_INDEX				3
#define CHN_ENABLE				1
#define CHN_DISABLE				0

void* video_feeder_thread(void *arg);
typedef struct ring_buffer_t ring_buffer_t;  // Forward declaration

int feed_video_to_ring_buffer(ring_buffer_t *rb, int chn);

struct chn_conf{
	unsigned int index;//0 for main channel ,1 for second channel
	unsigned int enable;
#ifdef PLATFORM_T31
	IMPEncoderProfile payloadType;
#endif
	IMPFSChnAttr fs_chn_attr;
	IMPCell framesource_chn;
	IMPCell imp_encoder;
};

typedef struct {
	uint8_t *streamAddr;
	int streamLen;
}streamInfo;

typedef struct {
#ifdef PLATFORM_T31
	IMPEncoderEncType type;
#endif
	IMPEncoderRcMode mode;
	uint16_t frameRate;
	uint16_t gopLength;
	uint32_t targetBitrate;
	uint32_t maxBitrate;
	int16_t initQp;
	int16_t minQp;
	int16_t maxQp;
	uint32_t maxPictureSize;
} encInfo;

#define  CHN_NUM  ARRAY_SIZE(chn)

int encoder_init();
int encoder_exit(void);

int sample_get_frame();
int sample_get_video_stream();
int sample_get_video_stream_byfd();

#endif /* __SAMPLE_COMMON_H__ */
