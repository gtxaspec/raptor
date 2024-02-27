#include <string.h>
#include <errno.h>
#include <math.h>

#include <imp/imp_log.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
#include <imp/imp_isp.h>

#include "ringbuffer.h"
#include "encoder.h"
#include "config.h"
#include "framesource.h"

#define TAG "encoder"

#ifdef PLATFORM_T31
	#define IMPEncoderCHNAttr IMPEncoderChnAttr
	#define IMPEncoderCHNStat IMPEncoderChnStat
#endif

//static const IMPEncoderRcMode S_RC_METHOD = IMP_ENC_RC_MODE_CAPPED_QUALITY;
extern struct chn_conf chn[];

#ifdef PLATFORM_T31
static const int S_RC_METHOD = IMP_ENC_RC_MODE_CAPPED_QUALITY;
#else
static const int S_RC_METHOD = ENC_RC_MODE_SMART;
#endif

//#define LOW_BITSTREAM

int encoder_init()
{
	int i, ret, chnNum = 0;
	IMPFSChnAttr *imp_chn_attr_tmp;
	IMPEncoderCHNAttr channel_attr;

	for (i = 0; i <  FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			imp_chn_attr_tmp = &chn[i].fs_chn_attr;
			chnNum = chn[i].index;			
			memset(&channel_attr, 0, sizeof(IMPEncoderCHNAttr));

#ifdef PLATFORM_T31
			unsigned int uTargetBitRate = 2666;

			//ret = IMP_Encoder_SetDefaultParam(&channel_attr, chn[i].payloadType, IMP_ENC_RC_MODE_VBR,
			ret = IMP_Encoder_SetDefaultParam(&channel_attr, IMP_ENC_PROFILE_AVC_HIGH, S_RC_METHOD,
					imp_chn_attr_tmp->picWidth, imp_chn_attr_tmp->picHeight,
					imp_chn_attr_tmp->outFrmRateNum, imp_chn_attr_tmp->outFrmRateDen,
					30, 1, -1, uTargetBitRate);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_SetDefaultParam(%d) error !\n", chnNum);
				return -1;
			}
			IMPEncoderRcAttr *rcAttr = &channel_attr.rcAttr;
			uTargetBitRate /= 2;

			switch (rcAttr->attrRcMode.rcMode) {
				case IMP_ENC_RC_MODE_FIXQP:
					rcAttr->attrRcMode.attrFixQp.iInitialQP = 38;
					break;
				case IMP_ENC_RC_MODE_CBR:
					rcAttr->attrRcMode.attrCbr.uTargetBitRate = uTargetBitRate;
					rcAttr->attrRcMode.attrCbr.iInitialQP = -1;
					rcAttr->attrRcMode.attrCbr.iMinQP = 34;
					rcAttr->attrRcMode.attrCbr.iMaxQP = 51;
					rcAttr->attrRcMode.attrCbr.iIPDelta = -1;
					rcAttr->attrRcMode.attrCbr.iPBDelta = -1;
					rcAttr->attrRcMode.attrCbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
					rcAttr->attrRcMode.attrCbr.uMaxPictureSize = uTargetBitRate * 4 / 3;
					break;
				case IMP_ENC_RC_MODE_VBR:
					rcAttr->attrRcMode.attrVbr.uTargetBitRate = uTargetBitRate;
					rcAttr->attrRcMode.attrVbr.uMaxBitRate = uTargetBitRate * 4 / 3;
					rcAttr->attrRcMode.attrVbr.iInitialQP = -1;
					rcAttr->attrRcMode.attrVbr.iMinQP = 34;
					rcAttr->attrRcMode.attrVbr.iMaxQP = 51;
					rcAttr->attrRcMode.attrVbr.iIPDelta = -1;
					rcAttr->attrRcMode.attrVbr.iPBDelta = -1;
					rcAttr->attrRcMode.attrVbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
					rcAttr->attrRcMode.attrVbr.uMaxPictureSize = uTargetBitRate * 4 / 3;
					break;
				case IMP_ENC_RC_MODE_CAPPED_VBR:
					rcAttr->attrRcMode.attrCappedVbr.uTargetBitRate = uTargetBitRate;
					rcAttr->attrRcMode.attrCappedVbr.uMaxBitRate = uTargetBitRate * 4 / 3;
					rcAttr->attrRcMode.attrCappedVbr.iInitialQP = -1;
					rcAttr->attrRcMode.attrCappedVbr.iMinQP = 34;
					rcAttr->attrRcMode.attrCappedVbr.iMaxQP = 51;
					rcAttr->attrRcMode.attrCappedVbr.iIPDelta = -1;
					rcAttr->attrRcMode.attrCappedVbr.iPBDelta = -1;
					rcAttr->attrRcMode.attrCappedVbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
					rcAttr->attrRcMode.attrCappedVbr.uMaxPictureSize = uTargetBitRate * 4 / 3;
					rcAttr->attrRcMode.attrCappedVbr.uMaxPSNR = 42;
					break;
				case IMP_ENC_RC_MODE_CAPPED_QUALITY:
					rcAttr->attrRcMode.attrCappedQuality.uTargetBitRate = 2000;
					rcAttr->attrRcMode.attrCappedQuality.uMaxBitRate = 2666;
					rcAttr->attrRcMode.attrCappedQuality.iInitialQP = -1;
					rcAttr->attrRcMode.attrCappedQuality.iMinQP = 20;
					rcAttr->attrRcMode.attrCappedQuality.iMaxQP = 45;
					rcAttr->attrRcMode.attrCappedQuality.iIPDelta = 3;
					rcAttr->attrRcMode.attrCappedQuality.iPBDelta = 3;
					rcAttr->attrRcMode.attrCappedQuality.eRcOptions = IMP_ENC_RC_SCN_CHG_RES;
					rcAttr->attrRcMode.attrCappedQuality.uMaxPictureSize = 1920;
					rcAttr->attrRcMode.attrCappedQuality.uMaxPSNR = 42;
					break;
				case IMP_ENC_RC_MODE_INVALID:
					IMP_LOG_ERR(TAG, "unsupported rcmode:%d, we only support fixqp, cbr vbr and capped vbr\n", rcAttr->attrRcMode.rcMode);
					return -1;
			}
#else
			IMPEncoderRcAttr *rc_attr;
			IMPEncoderAttr *enc_attr;

			enc_attr = &channel_attr.encAttr;
			enc_attr->enType = PT_H264;
			enc_attr->bufSize = 0;
			enc_attr->profile = 1;
			enc_attr->picWidth = imp_chn_attr_tmp->picWidth;
			enc_attr->picHeight = imp_chn_attr_tmp->picHeight;
			rc_attr = &channel_attr.rcAttr;
			rc_attr->outFrmRate.frmRateNum = imp_chn_attr_tmp->outFrmRateNum;
			rc_attr->outFrmRate.frmRateDen = imp_chn_attr_tmp->outFrmRateDen;
			rc_attr->maxGop = 2 * rc_attr->outFrmRate.frmRateNum / rc_attr->outFrmRate.frmRateDen;
			if (S_RC_METHOD == ENC_RC_MODE_CBR) {
				rc_attr->attrRcMode.rcMode = ENC_RC_MODE_CBR;
				rc_attr->attrRcMode.attrH264Cbr.outBitRate = (double)2000.0 * (imp_chn_attr_tmp->picWidth * imp_chn_attr_tmp->picHeight) / (1280 * 720);
				rc_attr->attrRcMode.attrH264Cbr.maxQp = 45;
				rc_attr->attrRcMode.attrH264Cbr.minQp = 15;
				rc_attr->attrRcMode.attrH264Cbr.iBiasLvl = 0;
				rc_attr->attrRcMode.attrH264Cbr.frmQPStep = 3;
				rc_attr->attrRcMode.attrH264Cbr.gopQPStep = 15;
				rc_attr->attrRcMode.attrH264Cbr.adaptiveMode = false;
				rc_attr->attrRcMode.attrH264Cbr.gopRelation = false;

				rc_attr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
				rc_attr->attrHSkip.hSkipAttr.m = 0;
				rc_attr->attrHSkip.hSkipAttr.n = 0;
				rc_attr->attrHSkip.hSkipAttr.maxSameSceneCnt = 0;
				rc_attr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
				rc_attr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
				rc_attr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
			} else if (S_RC_METHOD == ENC_RC_MODE_VBR) {
				rc_attr->attrRcMode.rcMode = ENC_RC_MODE_VBR;
				rc_attr->attrRcMode.attrH264Vbr.maxQp = 45;
				rc_attr->attrRcMode.attrH264Vbr.minQp = 15;
				rc_attr->attrRcMode.attrH264Vbr.staticTime = 2;
				rc_attr->attrRcMode.attrH264Vbr.maxBitRate = (double)2000.0 * (imp_chn_attr_tmp->picWidth * imp_chn_attr_tmp->picHeight) / (1280 * 720);
				rc_attr->attrRcMode.attrH264Vbr.iBiasLvl = 0;
				rc_attr->attrRcMode.attrH264Vbr.changePos = 80;
				rc_attr->attrRcMode.attrH264Vbr.qualityLvl = 2;
				rc_attr->attrRcMode.attrH264Vbr.frmQPStep = 3;
				rc_attr->attrRcMode.attrH264Vbr.gopQPStep = 15;
				rc_attr->attrRcMode.attrH264Vbr.gopRelation = false;

				rc_attr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
				rc_attr->attrHSkip.hSkipAttr.m = 0;
				rc_attr->attrHSkip.hSkipAttr.n = 0;
				rc_attr->attrHSkip.hSkipAttr.maxSameSceneCnt = 0;
				rc_attr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
				rc_attr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
				rc_attr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
			} else if (S_RC_METHOD == ENC_RC_MODE_SMART) {
				rc_attr->attrRcMode.rcMode = ENC_RC_MODE_SMART;
				rc_attr->attrRcMode.attrH264Smart.maxQp = 45;
				rc_attr->attrRcMode.attrH264Smart.minQp = 15;
				rc_attr->attrRcMode.attrH264Smart.staticTime = 2;
				rc_attr->attrRcMode.attrH264Smart.maxBitRate = (double)2000.0 * (imp_chn_attr_tmp->picWidth * imp_chn_attr_tmp->picHeight) / (1280 * 720);
				rc_attr->attrRcMode.attrH264Smart.iBiasLvl = 0;
				rc_attr->attrRcMode.attrH264Smart.changePos = 80;
				rc_attr->attrRcMode.attrH264Smart.qualityLvl = 2;
				rc_attr->attrRcMode.attrH264Smart.frmQPStep = 3;
				rc_attr->attrRcMode.attrH264Smart.gopQPStep = 15;
				rc_attr->attrRcMode.attrH264Smart.gopRelation = false;

				rc_attr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
				rc_attr->attrHSkip.hSkipAttr.m = rc_attr->maxGop - 1;
				rc_attr->attrHSkip.hSkipAttr.n = 1;
				rc_attr->attrHSkip.hSkipAttr.maxSameSceneCnt = 6;
				rc_attr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
				rc_attr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
				rc_attr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
			} else { /* fixQp */
				rc_attr->attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
				rc_attr->attrRcMode.attrH264FixQp.qp = 42;

				rc_attr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
				rc_attr->attrHSkip.hSkipAttr.m = 0;
				rc_attr->attrHSkip.hSkipAttr.n = 0;
				rc_attr->attrHSkip.hSkipAttr.maxSameSceneCnt = 0;
				rc_attr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
				rc_attr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
				rc_attr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
			}

#endif
			ret = IMP_Encoder_CreateChn(chnNum, &channel_attr);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_CreateChn(%d) error !\n", chnNum);
				return -1;
			}

			ret = IMP_Encoder_RegisterChn(chn[i].index, chnNum);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_RegisterChn(%d, %d) error: %d\n", chn[i].index, chnNum, ret);
				return -1;
			}
		}
	}

	return 0;
}

int encoder_exit(void)
{
	int ret = 0, i = 0, chnNum = 0;
	IMPEncoderCHNStat chn_stat;

	for (i = 0; i <  FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			chnNum = chn[i].index;
			memset(&chn_stat, 0, sizeof(IMPEncoderCHNStat));
			ret = IMP_Encoder_Query(chnNum, &chn_stat);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_Query(%d) error: %d\n", chnNum, ret);
				return -1;
			}

			if (chn_stat.registered) {
				ret = IMP_Encoder_UnRegisterChn(chnNum);
				if (ret < 0) {
					IMP_LOG_ERR(TAG, "IMP_Encoder_UnRegisterChn(%d) error: %d\n", chnNum, ret);
					return -1;
				}

				ret = IMP_Encoder_DestroyChn(chnNum);
				if (ret < 0) {
					IMP_LOG_ERR(TAG, "IMP_Encoder_DestroyChn(%d) error: %d\n", chnNum, ret);
					return -1;
				}

				ret = IMP_Encoder_DestroyGroup(chnNum);
				if (ret < 0) {
					IMP_LOG_ERR(TAG, "IMP_Encoder_DestroyGroup(%d) error: %d\n", chnNum, ret);
					return -1;
				}
			}
		}
	}

	return 0;
}

int feed_video_to_ring_buffer(ring_buffer_t *rb, int chn)
{
	int ret;
	IMPEncoderStream stream;

	ret = IMP_Encoder_StartRecvPic(chn);
	if (ret < 0){
		IMP_LOG_ERR(TAG, "IMP_Encoder_StartRecvPic(%d) failed\n", 1);
		return ret;
	}

	// Poll for the stream first
	ret = IMP_Encoder_PollingStream(chn, 100); // Timeout of 100 ms
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Polling stream timeout\n");
		return -1;
	}

	// Get the stream
	ret = IMP_Encoder_GetStream(chn, &stream, 0); // Blocking call
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_Encoder_GetStream() failed\n");
		return -1;
	}

	// Iterate through each packet in the stream
	for (int i = 0; i < stream.packCount; i++) {
		IMPEncoderPack *pack = &stream.pack[i];
		if(pack->length > 0){
			// Assuming the ring buffer has been initialized correctly elsewhere
			// Feed each packet into the ring buffer
		#ifdef PLATFORM_T31
			ring_buffer_queue_arr(rb, (const char *)(stream.virAddr + pack->offset), pack->length);
		#else
			ring_buffer_queue_arr(rb, (const char *)(pack->virAddr), pack->length);
		#endif
		}
	}

	// Release the stream after processing
	IMP_Encoder_ReleaseStream(chn, &stream);

	return 0; // Success
}
