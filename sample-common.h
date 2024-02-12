#ifndef __SAMPLE_COMMON_H__
#define __SAMPLE_COMMON_H__

#include <imp/imp_common.h>
#include <imp/imp_osd.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_system.h>
#include <imp/imp_encoder.h>
#include <unistd.h>

#ifdef __cplusplus
#if __cplusplus
extern "C"
{
#endif
#endif /* __cplusplus */

#define SENSOR_FRAME_RATE_NUM		30
#define SENSOR_FRAME_RATE_DEN		1

#define SENSOR_GC2053

#if defined SENSOR_JXF23
#define SENSOR_NAME				"jxf23"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x40
#define SENSOR_WIDTH			1920
#define SENSOR_HEIGHT			1080
#define CHN0_EN                 1
#define CHN1_EN                 0
#define CHN2_EN                 0
#define CHN3_EN                 1
#define CROP_EN					1

#elif defined SENSOR_GC2053
#define SENSOR_NAME				"gc2053"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x37
#define SENSOR_WIDTH			1920
#define SENSOR_HEIGHT			1080
#define CHN0_EN                 1
#define CHN1_EN                 0
#define CHN2_EN                 0
#define CHN3_EN                 0
#define CROP_EN					1

#endif

#define SENSOR_WIDTH_SECOND		1280
#define SENSOR_HEIGHT_SECOND	720

#define SENSOR_WIDTH_THIRD		640
#define SENSOR_HEIGHT_THIRD		360

#define BITRATE_720P_Kbs        1000

#define NR_FRAMES_TO_SAVE		200
#define STREAM_BUFFER_SIZE		(1 * 1024 * 1024)

#define STREAM_FILE_PATH_PREFIX		"/tmp"

#define SLEEP_TIME			1

#define FS_CHN_NUM			4  //MIN 1,MAX 3
#define IVS_CHN_ID          3

#define CH0_INDEX  0
#define CH1_INDEX  1
#define CH2_INDEX  2
#define CH3_INDEX  3
#define CHN_ENABLE 1
#define CHN_DISABLE 0
int get_stream(int fd, int chn);
struct chn_conf{
	unsigned int index;//0 for main channel ,1 for second channel
	unsigned int enable;
  IMPEncoderProfile payloadType;
	IMPFSChnAttr fs_chn_attr;
	IMPCell framesource_chn;
	IMPCell imp_encoder;
};

typedef struct {
	uint8_t *streamAddr;
	int streamLen;
}streamInfo;

typedef struct {
	IMPEncoderEncType type;
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

int sample_system_init();
int sample_system_exit();

int sample_framesource_streamon();
int sample_framesource_streamoff();

int sample_framesource_init();
int sample_framesource_exit();

int sample_encoder_init();
int sample_encoder_exit(void);

int sample_get_frame();
int sample_get_video_stream();
int sample_get_video_stream_byfd();

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __SAMPLE_COMMON_H__ */
