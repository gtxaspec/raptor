#include <imp/imp_log.h>
#include <imp/imp_framesource.h>
#include "framesource.h"
#include "encoder.h"
#include "libimp_dynamic_t31.h"

#define TAG "framesource"

/*
void setup_framesource(FrameSource *framesource)
{
int ret;
IMPFSChnAttr *fs_chn_attr = &framesource->imp_fs_attrs;

fs_chn_attr->pixFmt = framesource->pixel_format;
fs_chn_attr->outFrmRateNum = framesource->frame_rate_numerator;
fs_chn_attr->outFrmRateDen = framesource->frame_rate_denominator;
fs_chn_attr->nrVBs = framesource->buffer_size;
fs_chn_attr->crop.enable = framesource->crop_enable;
fs_chn_attr->crop.top = framesource->crop_top;
fs_chn_attr->crop.left = framesource->crop_left;
fs_chn_attr->crop.width = framesource->crop_width;
fs_chn_attr->crop.height = framesource->crop_height;
fs_chn_attr->scaler.enable = framesource->scaling_enable;
fs_chn_attr->scaler.outwidth = framesource->scaling_width;
fs_chn_attr->scaler.outheight = framesource->scaling_height;
fs_chn_attr->picWidth = framesource->pic_width;
fs_chn_attr->picHeight = framesource->pic_height;
fs_chn_attr->type = framesource->channel_type;

ret = IMP_FrameSource_CreateChn(framesource->id, fs_chn_attr);
if(ret < 0){
	exit(1);
}

ret = IMP_FrameSource_SetChnAttr(framesource->id, fs_chn_attr);
if (ret < 0) {
	exit(1);
}

}*/

struct chn_conf chn[FS_CHN_NUM] = {
	{
		.index = CH0_INDEX,
		.enable = CHN0_EN,
	//.payloadType = IMP_ENC_PROFILE_HEVC_MAIN,
		.fs_chn_attr = {
			.pixFmt = PIX_FMT_NV12,
			//.outFrmRateNum = SENSOR_FRAME_RATE_NUM,
			.outFrmRateDen = SENSOR_FRAME_RATE_DEN,
			.nrVBs = 2,
			.type = FS_PHY_CHANNEL,

			.scaler.enable = 0,

			.crop.enable = CROP_EN,
			.crop.top = 0,
			.crop.left = 0,
			//.crop.width = SENSOR_WIDTH,
			//.crop.height = SENSOR_HEIGHT,

			//.picWidth = SENSOR_WIDTH,
			//.picHeight = SENSOR_HEIGHT,
		},
		.framesource_chn =	{ DEV_ID_FS, CH0_INDEX, 0},
		.imp_encoder = { DEV_ID_ENC, CH0_INDEX, 0},
	},
	{
		.index = CH1_INDEX,
		.enable = CHN1_EN,
// .payloadType = IMP_ENC_PROFILE_HEVC_MAIN,
		.fs_chn_attr = {
			.pixFmt = PIX_FMT_NV12,
			//.outFrmRateNum = SENSOR_FRAME_RATE_NUM,
			.outFrmRateDen = SENSOR_FRAME_RATE_DEN,
			.nrVBs = 2,
			.type = FS_PHY_CHANNEL,

			.scaler.enable = 1,
			.scaler.outwidth = SENSOR_WIDTH_THIRD,
			.scaler.outheight = SENSOR_HEIGHT_THIRD,

			.crop.enable = 0,
			.crop.top = 0,
			.crop.left = 0,
			.crop.width = SENSOR_WIDTH_THIRD,
			.crop.height = SENSOR_HEIGHT_THIRD,

			.picWidth = SENSOR_WIDTH_THIRD,
			.picHeight = SENSOR_HEIGHT_THIRD,
		},
		.framesource_chn =	{ DEV_ID_FS, CH1_INDEX, 0},
		.imp_encoder = { DEV_ID_ENC, CH1_INDEX, 0},
	},
	{
		.index = CH2_INDEX,
		.enable = CHN2_EN,
// .payloadType = IMP_ENC_PROFILE_HEVC_MAIN,
		.fs_chn_attr = {
			.pixFmt = PIX_FMT_NV12,
			//.outFrmRateNum = SENSOR_FRAME_RATE_NUM,
			.outFrmRateDen = SENSOR_FRAME_RATE_DEN,
			.nrVBs = 2,
			.type = FS_PHY_CHANNEL,

			.scaler.enable = 1,
			.scaler.outwidth = SENSOR_WIDTH_SECOND,
			.scaler.outheight = SENSOR_HEIGHT_SECOND,

			.crop.enable = 0,
			.crop.top = 0,
			.crop.left = 0,
			.crop.width = SENSOR_WIDTH_SECOND,
			.crop.height = SENSOR_HEIGHT_SECOND,

			.picWidth = SENSOR_WIDTH_SECOND,
			.picHeight = SENSOR_HEIGHT_SECOND,
		},
		.framesource_chn =	{ DEV_ID_FS, CH2_INDEX, 0},
		.imp_encoder = { DEV_ID_ENC, CH2_INDEX, 0},
	},
	{
		.index = CH3_INDEX,
		.enable = CHN3_EN,
	//.payloadType = IMP_ENC_PROFILE_HEVC_MAIN,
		.fs_chn_attr = {
			.pixFmt = PIX_FMT_NV12,
			//.outFrmRateNum = SENSOR_FRAME_RATE_NUM,
			.outFrmRateDen = SENSOR_FRAME_RATE_DEN,
			.nrVBs = 2,
			.type = FS_EXT_CHANNEL,

			.scaler.enable = 1,
			.scaler.outwidth = SENSOR_WIDTH_SECOND,
			.scaler.outheight = SENSOR_HEIGHT_SECOND,

			.crop.enable = 0,
			.crop.top = 0,
			.crop.left = 0,
			.crop.width = SENSOR_WIDTH_SECOND,
			.crop.height = SENSOR_HEIGHT_SECOND,

			.picWidth = SENSOR_WIDTH_SECOND,
			.picHeight = SENSOR_HEIGHT_SECOND,
		},
		.framesource_chn =	{ DEV_ID_FS, CH3_INDEX, 0},
		.imp_encoder = { DEV_ID_ENC, CH3_INDEX, 0},
	},
};

int framesource_streamon()
{
	int ret = 0, i = 0;
	/* Enable channels */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_FrameSource_EnableChn(chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_FrameSource_EnableChn(%d) error: %d\n", ret, chn[i].index);
				return -1;
			}
		}
	}
	return 0;
}

int framesource_streamoff()
{
	int ret = 0, i = 0;
	/* Enable channels */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable){
			ret = IMP_FrameSource_DisableChn(chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_FrameSource_DisableChn(%d) error: %d\n", ret, chn[i].index);
				return -1;
			}
		}
	}
	return 0;
}

int framesource_init()
{
	int i, ret;

	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_FrameSource_CreateChn(chn[i].index, &chn[i].fs_chn_attr);
			if(ret < 0){
				IMP_LOG_ERR(TAG, "IMP_FrameSource_CreateChn(chn%d) error !\n", chn[i].index);
				return -1;
			}

			ret = IMP_FrameSource_SetChnAttr(chn[i].index, &chn[i].fs_chn_attr);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_FrameSource_SetChnAttr(chn%d) error !\n",  chn[i].index);
				return -1;
			}
		}
	}

	return 0;
}

int framesource_exit()
{
	int ret,i;

	for (i = 0; i <  FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			/*Destroy channel */
			ret = IMP_FrameSource_DestroyChn(chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_FrameSource_DestroyChn(%d) error: %d\n", chn[i].index, ret);
				return -1;
			}
		}
	}
	return 0;
}