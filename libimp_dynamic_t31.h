#include <imp/imp_audio.h>
#include <imp/imp_common.h>
#include <imp/imp_decoder.h>
#include <imp/imp_dmic.h>
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_base_move.h>
#include <imp/imp_ivs_move.h>
#include <imp/imp_osd.h>
#include <imp/imp_system.h>
#include <imp/imp_utils.h>

// Function declaration
void init_libimp_v1();

typedef const char* (*IMP_System_GetCPUInfo_t)(void);
typedef int (*IMP_IVS_CreateGroup_t)(int GrpNum);
typedef int (*IMP_IVS_DestroyGroup_t)(int GrpNum);
typedef int (*IMP_IVS_CreateChn_t)(int ChnNum, IMPIVSInterface *handler);
typedef int (*IMP_IVS_DestroyChn_t)(int ChnNum);
typedef int (*IMP_IVS_RegisterChn_t)(int GrpNum, int ChnNum);
typedef int (*IMP_IVS_UnRegisterChn_t)(int ChnNum);
typedef int (*IMP_IVS_StartRecvPic_t)(int ChnNum);
typedef int (*IMP_IVS_StopRecvPic_t)(int ChnNum);
typedef int (*IMP_IVS_PollingResult_t)(int ChnNum, int timeoutMs);
typedef int (*IMP_IVS_GetResult_t)(int ChnNum, void **result);
typedef int (*IMP_IVS_ReleaseResult_t)(int ChnNum, void *result);
typedef int (*IMP_IVS_ReleaseData_t)(void *vaddr);
typedef int (*IMP_IVS_GetParam_t)(int chnNum, void *param);
typedef int (*IMP_IVS_SetParam_t)(int chnNum, void *param);
typedef int (*IMP_DMIC_SetUserInfo_t)(int dmicDevId, int aecDmicId, int need_aec);
typedef int (*IMP_DMIC_SetPubAttr_t)(int dmicDevId, IMPDmicAttr *attr);
typedef int (*IMP_DMIC_GetPubAttr_t)(int dmicDevId, IMPDmicAttr *attr);
typedef int (*IMP_DMIC_Enable_t)(int dmicDevId);
typedef int (*IMP_DMIC_Disable_t)(int dmicDevId);
typedef int (*IMP_DMIC_EnableChn_t)(int dmicDevId, int dmicChnId);
typedef int (*IMP_DMIC_DisableChn_t)(int dmicDevId, int dmicChnId);
typedef int (*IMP_DMIC_SetChnParam_t)(int dmicDevId, int dmicChnId, IMPDmicChnParam *chnParam);
typedef int (*IMP_DMIC_GetChnParam_t)(int dmicDevId, int dmicChnId, IMPDmicChnParam *chnParam);
typedef int (*IMP_DMIC_GetFrame_t)(int dmicDevId, int dmicChnId, IMPDmicChnFrame *chnFrm, IMPBlock block);
typedef int (*IMP_DMIC_ReleaseFrame_t)(int dmicDevId, int dmicChnId, IMPDmicChnFrame *chnFrm);
typedef int (*IMP_DMIC_EnableAecRefFrame_t)(int dmicDevId, int dmicChnId, int audioAoDevId, int aoChn);
typedef int (*IMP_DMIC_GetFrameAndRef_t)(int dmicDevId, int dmicChnId, IMPDmicChnFrame *chnFrm, IMPDmicFrame *ref, IMPBlock block);
typedef int (*IMP_DMIC_EnableAec_t)(int dmicDevId, int dmicChnId, int aoDevId, int aoChId);
typedef int (*IMP_DMIC_DisableAec_t)(int dmicDevId, int dmicChnId);
typedef int (*IMP_DMIC_PollingFrame_t)(int dmicDevId, int dmicChnId, unsigned int timeout_ms);
typedef int (*IMP_DMIC_SetVol_t)(int dmicDevId, int dmicChnId, int dmicVol);
typedef int (*IMP_DMIC_GetVol_t)(int dmicDevId, int dmicChnId, int *dmicVol);
typedef int (*IMP_DMIC_SetGain_t)(int dmicDevId, int dmicChnId, int dmicGain);
typedef int (*IMP_DMIC_GetGain_t)(int dmicDevId, int dmicChnId, int *dmicGain);
typedef int (*IMP_AI_SetPubAttr_t)(int audioDevId, IMPAudioIOAttr *attr);
typedef int (*IMP_AI_GetPubAttr_t)(int audioDevId, IMPAudioIOAttr *attr);
typedef int (*IMP_AI_Enable_t)(int audioDevId);
typedef int (*IMP_AI_Disable_t)(int audioDevId);
typedef int (*IMP_AI_EnableChn_t)(int audioDevId, int aiChn);
typedef int (*IMP_AI_DisableChn_t)(int audioDevId, int aiChn);
typedef int (*IMP_AI_PollingFrame_t)(int audioDevId, int aiChn, unsigned int timeout_ms);
typedef int (*IMP_AI_GetFrame_t)(int audioDevId, int aiChn, IMPAudioFrame *frm, IMPBlock block);
typedef int (*IMP_AI_ReleaseFrame_t)(int audioDevId, int aiChn, IMPAudioFrame *frm);
typedef int (*IMP_AI_SetChnParam_t)(int audioDevId, int aiChn, IMPAudioIChnParam *chnParam);
typedef int (*IMP_AI_GetChnParam_t)(int audioDevId, int aiChn, IMPAudioIChnParam *chnParam);
typedef int (*IMP_AI_EnableAec_t)(int aiDevId, int aiChn, int aoDevId, int aoChn);
typedef int (*IMP_AI_Set_WebrtcProfileIni_Path_t)(char *path);
typedef int (*IMP_AI_DisableAec_t)(int aiDevId, int aiChn);
typedef int (*IMP_AI_EnableNs_t)(IMPAudioIOAttr *attr, int mode);
typedef int (*IMP_AI_DisableNs_t)(void);
typedef int (*IMP_AI_SetAgcMode_t)(int mode);
typedef int (*IMP_AI_EnableAgc_t)(IMPAudioIOAttr *attr, IMPAudioAgcConfig agcConfig);
typedef int (*IMP_AI_DisableAgc_t)(void);
typedef int (*IMP_AO_EnableAgc_t)(IMPAudioIOAttr *attr, IMPAudioAgcConfig agcConfig);
typedef int (*IMP_AO_DisableAgc_t)(void);
typedef int (*IMP_AI_EnableHpf_t)(IMPAudioIOAttr *attr);
typedef int (*IMP_AI_SetHpfCoFrequency_t)(int cofrequency);
typedef int (*IMP_AI_DisableHpf_t)(void);
typedef int (*IMP_AO_EnableHpf_t)(IMPAudioIOAttr *attr);
typedef int (*IMP_AO_SetHpfCoFrequency_t)(int cofrequency);
typedef int (*IMP_AO_DisableHpf_t)(void);
typedef int (*IMP_AO_SetPubAttr_t)(int audioDevId, IMPAudioIOAttr *attr);
typedef int (*IMP_AO_GetPubAttr_t)(int audioDevId, IMPAudioIOAttr *attr);
typedef int (*IMP_AO_Enable_t)(int audioDevId);
typedef int (*IMP_AO_Disable_t)(int audioDevId);
typedef int (*IMP_AO_EnableChn_t)(int audioDevId, int aoChn);
typedef int (*IMP_AO_DisableChn_t)(int audioDevId, int aoChn);
typedef int (*IMP_AO_SendFrame_t)(int audioDevId, int aoChn, IMPAudioFrame *data, IMPBlock block);
typedef int (*IMP_AO_PauseChn_t)(int audioDevId, int aoChn);
typedef int (*IMP_AO_ResumeChn_t)(int audioDevId, int aoChn);
typedef int (*IMP_AO_ClearChnBuf_t)(int audioDevId, int aoChn);
typedef int (*IMP_AO_QueryChnStat_t)(int audioDevId, int aoChn, IMPAudioOChnState *status);
typedef int (*IMP_AENC_CreateChn_t)(int aeChn, IMPAudioEncChnAttr *attr);
typedef int (*IMP_AENC_DestroyChn_t)(int aeChn);
typedef int (*IMP_AENC_SendFrame_t)(int aeChn, IMPAudioFrame *frm);
typedef int (*IMP_AENC_PollingStream_t)(int AeChn, unsigned int timeout_ms);
typedef int (*IMP_AENC_GetStream_t)(int aeChn, IMPAudioStream *stream ,IMPBlock block);
typedef int (*IMP_AENC_ReleaseStream_t)(int aeChn,IMPAudioStream *stream);
typedef int (*IMP_AENC_RegisterEncoder_t)(int *handle, IMPAudioEncEncoder *encoder);
typedef int (*IMP_AENC_UnRegisterEncoder_t)(int *handle);
typedef int (*IMP_ADEC_CreateChn_t)(int adChn, IMPAudioDecChnAttr *attr);
typedef int (*IMP_ADEC_DestroyChn_t)(int adChn);
typedef int (*IMP_ADEC_SendStream_t)(int adChn, IMPAudioStream *stream, IMPBlock block);
typedef int (*IMP_ADEC_PollingStream_t)(int AdChn, unsigned int timeout_ms);
typedef int (*IMP_ADEC_GetStream_t)(int adChn, IMPAudioStream *stream ,IMPBlock block);
typedef int (*IMP_ADEC_ReleaseStream_t)(int adChn,IMPAudioStream *stream);
typedef int (*IMP_ADEC_ClearChnBuf_t)(int adChn);
typedef int (*IMP_ADEC_RegisterDecoder_t)(int *handle, IMPAudioDecDecoder *decoder);
typedef int (*IMP_ADEC_UnRegisterDecoder_t)(int *handle);
typedef int (*IMP_AI_SetVol_t)(int audioDevId, int aiChn, int aiVol);
typedef int (*IMP_AI_GetVol_t)(int audioDevId, int aiChn, int *vol);
typedef int (*IMP_AI_SetVolMute_t)(int audioDevId, int aiChn, int mute);
typedef int (*IMP_AO_SetVol_t)(int audioDevId, int aoChn, int aoVol);
typedef int (*IMP_AO_GetVol_t)(int audioDevId, int aoChn, int *vol);
typedef int (*IMP_AO_SetVolMute_t)(int audioDevId, int aoChn, int mute);
typedef int (*IMP_AI_SetGain_t)(int audioDevId, int aiChn, int aiGain);
typedef int (*IMP_AI_GetGain_t)(int audioDevId, int aiChn, int *aiGain);
typedef int (*IMP_AI_SetAlcGain_t)(int audioDevId, int aiChn, int aiPgaGain);
typedef int (*IMP_AI_GetAlcGain_t)(int audioDevId, int aiChn, int *aiPgaGain);
typedef int (*IMP_AO_SetGain_t)(int audioDevId, int aoChn, int aoGain);
typedef int (*IMP_AO_GetGain_t)(int audioDevId, int aoChn, int *aoGain);
typedef int (*IMP_AO_Soft_Mute_t)(int audioDevId, int aoChn);
typedef int (*IMP_AO_Soft_UNMute_t)(int audioDevId, int aoChn);
typedef int (*IMP_AI_GetFrameAndRef_t)(int audioDevId, int aiChn, IMPAudioFrame *frm, IMPAudioFrame *ref, IMPBlock block);
typedef int (*IMP_AI_EnableAecRefFrame_t)(int audioDevId, int aiChn, int audioAoDevId, int aoChn);
typedef int (*IMP_AI_DisableAecRefFrame_t)(int audioDevId, int aiChn, int audioAoDevId, int aoChn);
typedef int (*IMP_AO_CacheSwitch_t)(int audioDevId, int aoChn, int cache_en);
typedef int (*IMP_AO_FlushChnBuf_t)(int audioDevId, int aoChn);
typedef IMPIVSInterface (**IMP_IVS_CreateBaseMoveInterface_t)(IMP_IVS_BaseMoveParam *param);
typedef void (*IMP_IVS_DestroyBaseMoveInterface_t)(IMPIVSInterface *moveInterface);
typedef char (**IMPPixfmtToString_t)(IMPPixelFormat pixfmt);
typedef int (*IMP_OSD_CreateGroup_t)(int grpNum);
typedef int (*IMP_OSD_DestroyGroup_t)(int grpNum);
typedef int (*IMP_OSD_AttachToGroup_t)(IMPCell *from, IMPCell *to);
typedef IMPRgnHandle (*IMP_OSD_CreateRgn_t)(IMPOSDRgnAttr *prAttr);
typedef void (*IMP_OSD_DestroyRgn_t)(IMPRgnHandle handle);
typedef int (*IMP_OSD_RegisterRgn_t)(IMPRgnHandle handle, int grpNum, IMPOSDGrpRgnAttr *pgrAttr);
typedef int (*IMP_OSD_UnRegisterRgn_t)(IMPRgnHandle handle, int grpNum);
typedef int (*IMP_OSD_SetRgnAttr_t)(IMPRgnHandle handle, IMPOSDRgnAttr *prAttr);
typedef int (*IMP_OSD_SetRgnAttrWithTimestamp_t)(IMPRgnHandle handle, IMPOSDRgnAttr *prAttr, IMPOSDRgnTimestamp *prTs);
typedef int (*IMP_OSD_GetRgnAttr_t)(IMPRgnHandle handle, IMPOSDRgnAttr *prAttr);
typedef int (*IMP_OSD_UpdateRgnAttrData_t)(IMPRgnHandle handle, IMPOSDRgnAttrData *prAttrData);
typedef int (*IMP_OSD_SetGrpRgnAttr_t)(IMPRgnHandle handle, int grpNum, IMPOSDGrpRgnAttr *pgrAttr);
typedef int (*IMP_OSD_GetGrpRgnAttr_t)(IMPRgnHandle handle, int grpNum, IMPOSDGrpRgnAttr *pgrAttr);
typedef int (*IMP_OSD_ShowRgn_t)(IMPRgnHandle handle, int grpNum, int showFlag);
typedef int (*IMP_OSD_Start_t)(int grpNum);
typedef int (*IMP_OSD_Stop_t)(int grpNum);
typedef int (*IMP_ISP_Open_t)(void);
typedef int (*IMP_ISP_Close_t)(void);
typedef int32_t (*IMP_ISP_SetDefaultBinPath_t)(char *path);
typedef int32_t (*IMP_ISP_GetDefaultBinPath_t)(char *path);
typedef int (*IMP_ISP_AddSensor_t)(IMPSensorInfo *pinfo);
typedef int (*IMP_ISP_DelSensor_t)(IMPSensorInfo *pinfo);
typedef int (*IMP_ISP_EnableSensor_t)(void);
typedef int (*IMP_ISP_DisableSensor_t)(void);
typedef int (*IMP_ISP_SetSensorRegister_t)(uint32_t reg, uint32_t value);
typedef int (*IMP_ISP_GetSensorRegister_t)(uint32_t reg, uint32_t *value);
typedef int (*IMP_ISP_Tuning_SetAutoZoom_t)(IMPISPAutoZoom *ispautozoom);
typedef int (*IMP_ISP_EnableTuning_t)(void);
typedef int (*IMP_ISP_DisableTuning_t)(void);
typedef int (*IMP_ISP_Tuning_SetSensorFPS_t)(uint32_t fps_num, uint32_t fps_den);
typedef int (*IMP_ISP_Tuning_GetSensorFPS_t)(uint32_t *fps_num, uint32_t *fps_den);
typedef int (*IMP_ISP_Tuning_SetAntiFlickerAttr_t)(IMPISPAntiflickerAttr attr);
typedef int (*IMP_ISP_Tuning_GetAntiFlickerAttr_t)(IMPISPAntiflickerAttr *pattr);
typedef int (*IMP_ISP_Tuning_SetBrightness_t)(unsigned char bright);
typedef int (*IMP_ISP_Tuning_GetBrightness_t)(unsigned char *pbright);
typedef int (*IMP_ISP_Tuning_SetContrast_t)(unsigned char contrast);
typedef int (*IMP_ISP_Tuning_GetContrast_t)(unsigned char *pcontrast);
typedef int (*IMP_ISP_Tuning_SetSharpness_t)(unsigned char sharpness);
typedef int (*IMP_ISP_Tuning_GetSharpness_t)(unsigned char *psharpness);
typedef int (*IMP_ISP_Tuning_SetBcshHue_t)(unsigned char hue);
typedef int (*IMP_ISP_Tuning_GetBcshHue_t)(unsigned char *hue);
typedef int (*IMP_ISP_Tuning_SetSaturation_t)(unsigned char sat);
typedef int (*IMP_ISP_Tuning_GetSaturation_t)(unsigned char *psat);
typedef int (*IMP_ISP_Tuning_SetISPBypass_t)(IMPISPTuningOpsMode enable);
typedef int (*IMP_ISP_Tuning_GetTotalGain_t)(uint32_t *gain);
typedef int (*IMP_ISP_Tuning_SetISPHflip_t)(IMPISPTuningOpsMode mode);
typedef int (*IMP_ISP_Tuning_GetISPHflip_t)(IMPISPTuningOpsMode *pmode);
typedef int (*IMP_ISP_Tuning_SetISPVflip_t)(IMPISPTuningOpsMode mode);
typedef int (*IMP_ISP_Tuning_GetISPVflip_t)(IMPISPTuningOpsMode *pmode);
typedef int (*IMP_ISP_Tuning_SetISPRunningMode_t)(IMPISPRunningMode mode);
typedef int (*IMP_ISP_Tuning_GetISPRunningMode_t)(IMPISPRunningMode *pmode);
typedef int (*IMP_ISP_Tuning_SetISPCustomMode_t)(IMPISPTuningOpsMode mode);
typedef int (*IMP_ISP_Tuning_GetISPCustomMode_t)(IMPISPTuningOpsMode *pmode);
typedef int (*IMP_ISP_Tuning_SetGamma_t)(IMPISPGamma *gamma);
typedef int (*IMP_ISP_Tuning_GetGamma_t)(IMPISPGamma *gamma);
typedef int (*IMP_ISP_Tuning_SetAeComp_t)(int comp);
typedef int (*IMP_ISP_Tuning_GetAeComp_t)(int *comp);
typedef int (*IMP_ISP_Tuning_GetAeLuma_t)(int *luma);
typedef int (*IMP_ISP_Tuning_SetAeFreeze_t)(IMPISPTuningOpsMode mode);
typedef int (*IMP_ISP_Tuning_SetExpr_t)(IMPISPExpr *expr);
typedef int (*IMP_ISP_Tuning_GetExpr_t)(IMPISPExpr *expr);
typedef int (*IMP_ISP_Tuning_SetWB_t)(IMPISPWB *wb);
typedef int (*IMP_ISP_Tuning_GetWB_t)(IMPISPWB *wb);
typedef int (*IMP_ISP_Tuning_GetWB_Statis_t)(IMPISPWB *wb);
typedef int (*IMP_ISP_Tuning_GetWB_GOL_Statis_t)(IMPISPWB *wb);
typedef int (*IMP_ISP_Tuning_SetAwbClust_t)(IMPISPAWBCluster *awb_cluster);
typedef int (*IMP_ISP_Tuning_GetAwbClust_t)(IMPISPAWBCluster *awb_cluster);
typedef int (*IMP_ISP_Tuning_SetAwbCtTrend_t)(IMPISPAWBCtTrend *ct_trend);
typedef int (*IMP_ISP_Tuning_GetAwbCtTrend_t)(IMPISPAWBCtTrend *ct_trend);
typedef int (*IMP_ISP_Tuning_Awb_GetRgbCoefft_t)(IMPISPCOEFFTWB *isp_core_rgb_coefft_wb_attr);
typedef int (*IMP_ISP_Tuning_Awb_SetRgbCoefft_t)(IMPISPCOEFFTWB *isp_core_rgb_coefft_wb_attr);
typedef int (*IMP_ISP_Tuning_SetMaxAgain_t)(uint32_t gain);
typedef int (*IMP_ISP_Tuning_GetMaxAgain_t)(uint32_t *gain);
typedef int (*IMP_ISP_Tuning_SetMaxDgain_t)(uint32_t gain);
typedef int (*IMP_ISP_Tuning_GetMaxDgain_t)(uint32_t *gain);
typedef int (*IMP_ISP_Tuning_SetHiLightDepress_t)(uint32_t strength);
typedef int (*IMP_ISP_Tuning_GetHiLightDepress_t)(uint32_t *strength);
typedef int (*IMP_ISP_Tuning_SetBacklightComp_t)(uint32_t strength);
typedef int (*IMP_ISP_Tuning_GetBacklightComp_t)(uint32_t *strength);
typedef int (*IMP_ISP_Tuning_SetTemperStrength_t)(uint32_t ratio);
typedef int (*IMP_ISP_Tuning_SetSinterStrength_t)(uint32_t ratio);
typedef int (*IMP_ISP_Tuning_GetEVAttr_t)(IMPISPEVAttr *attr);
typedef int (*IMP_ISP_Tuning_EnableMovestate_t)(void);
typedef int (*IMP_ISP_Tuning_DisableMovestate_t)(void);
typedef int (*IMP_ISP_Tuning_SetAeWeight_t)(IMPISPWeight *ae_weight);
typedef int (*IMP_ISP_Tuning_GetAeWeight_t)(IMPISPWeight *ae_weight);
typedef int (*IMP_ISP_Tuning_AE_GetROI_t)(IMPISPWeight *roi_weight);
typedef int (*IMP_ISP_Tuning_AE_SetROI_t)(IMPISPWeight *roi_weight);
typedef int (*IMP_ISP_Tuning_SetAwbWeight_t)(IMPISPWeight *awb_weight);
typedef int (*IMP_ISP_Tuning_GetAwbWeight_t)(IMPISPWeight *awb_weight);
typedef int (*IMP_ISP_Tuning_GetAwbZone_t)(IMPISPAWBZone *awb_zone);
typedef int (*IMP_ISP_Tuning_SetWB_ALGO_t)(IMPISPAWBAlgo wb_algo);
typedef int (*IMP_ISP_Tuning_SetAeHist_t)(IMPISPAEHist *ae_hist);
typedef int (*IMP_ISP_Tuning_GetAeHist_t)(IMPISPAEHist *ae_hist);
typedef int (*IMP_ISP_Tuning_GetAeHist_Origin_t)(IMPISPAEHistOrigin *ae_hist);
typedef int (*IMP_ISP_Tuning_GetAwbHist_t)(IMPISPAWBHist *awb_hist);
typedef int (*IMP_ISP_Tuning_SetAwbHist_t)(IMPISPAWBHist *awb_hist);
typedef int (*IMP_ISP_Tuning_GetAFMetrices_t)(unsigned int *metric);
typedef int (*IMP_ISP_Tuning_GetAfHist_t)(IMPISPAFHist *af_hist);
typedef int (*IMP_ISP_Tuning_SetAfHist_t)(IMPISPAFHist *af_hist);
typedef int (*IMP_ISP_Tuning_SetAfWeight_t)(IMPISPWeight *af_weight);
typedef int (*IMP_ISP_Tuning_GetAfWeight_t)(IMPISPWeight *af_weight);
typedef int (*IMP_ISP_Tuning_GetAfZone_t)(IMPISPZone *af_zone);
typedef int (*IMP_ISP_Tuning_WaitFrame_t)(IMPISPWaitFrameAttr *attr);
typedef int (*IMP_ISP_Tuning_SetAeMin_t)(IMPISPAEMin *ae_min);
typedef int (*IMP_ISP_Tuning_GetAeMin_t)(IMPISPAEMin *ae_min);
typedef int (*IMP_ISP_Tuning_SetAe_IT_MAX_t)(unsigned int it_max);
typedef int (*IMP_ISP_Tuning_GetAE_IT_MAX_t)(unsigned int *it_max);
typedef int (*IMP_ISP_Tuning_GetAeZone_t)(IMPISPZone *ae_zone);
typedef int (*IMP_ISP_Tuning_SetAeTargetList_t)(IMPISPAETargetList *at_list);
typedef int (*IMP_ISP_Tuning_GetAeTargetList_t)(IMPISPAETargetList *at_list);
typedef int (*IMP_ISP_Tuning_SetModuleControl_t)(IMPISPModuleCtl *ispmodule);
typedef int (*IMP_ISP_Tuning_GetModuleControl_t)(IMPISPModuleCtl *ispmodule);
typedef int (*IMP_ISP_Tuning_SetFrontCrop_t)(IMPISPFrontCrop *ispfrontcrop);
typedef int (*IMP_ISP_Tuning_GetFrontCrop_t)(IMPISPFrontCrop *ispfrontcrop);
typedef int (*IMP_ISP_WDR_ENABLE_t)(IMPISPTuningOpsMode mode);
typedef int (*IMP_ISP_WDR_ENABLE_Get_t)(IMPISPTuningOpsMode* mode);
typedef int (*IMP_ISP_Tuning_SetDPC_Strength_t)(unsigned int ratio);
typedef int (*IMP_ISP_Tuning_GetDPC_Strength_t)(unsigned int *ratio);
typedef int (*IMP_ISP_Tuning_SetDRC_Strength_t)(unsigned int ratio);
typedef int (*IMP_ISP_Tuning_GetDRC_Strength_t)(unsigned int *ratio);
typedef int (*IMP_ISP_Tuning_SetHVFLIP_t)(IMPISPHVFLIP hvflip);
typedef int (*IMP_ISP_Tuning_GetHVFlip_t)(IMPISPHVFLIP *hvflip);
typedef int (*IMP_ISP_Tuning_SetMask_t)(IMPISPMASKAttr *mask);
typedef int (*IMP_ISP_Tuning_GetMask_t)(IMPISPMASKAttr *mask);
typedef int (*IMP_ISP_Tuning_GetSensorAttr_t)(IMPISPSENSORAttr *attr);
typedef int (*IMP_ISP_Tuning_EnableDRC_t)(IMPISPTuningOpsMode mode);
typedef int (*IMP_ISP_Tuning_EnableDefog_t)(IMPISPTuningOpsMode mode);
typedef int (*IMP_ISP_Tuning_SetAwbCt_t)(unsigned int *ct);
typedef int (*IMP_ISP_Tuning_GetAWBCt_t)(unsigned int *ct);
typedef int (*IMP_ISP_Tuning_SetCCMAttr_t)(IMPISPCCMAttr *ccm);
typedef int (*IMP_ISP_Tuning_GetCCMAttr_t)(IMPISPCCMAttr *ccm);
typedef int (*IMP_ISP_Tuning_SetAeAttr_t)(IMPISPAEAttr *ae);
typedef int (*IMP_ISP_Tuning_GetAeAttr_t)(IMPISPAEAttr *ae);
typedef int (*IMP_ISP_Tuning_GetAeState_t)(IMPISPAEState *ae_state);
typedef int (*IMP_ISP_Tuning_SetScalerLv_t)(IMPISPScalerLv *scaler_level);
typedef int32_t (*IMP_ISP_SetAeAlgoFunc_t)(IMPISPAeAlgoFunc *ae_func);
typedef int32_t (*IMP_ISP_SetAwbAlgoFunc_t)(IMPISPAwbAlgoFunc *awb_func);
typedef int (*IMP_ISP_Tuning_GetBlcAttr_t)(IMPISPBlcAttr *blc);
typedef int32_t (*IMP_ISP_Tuning_SetDefog_Strength_t)(uint8_t *ratio);
typedef int32_t (*IMP_ISP_Tuning_GetDefog_Strength_t)(uint8_t *ratio);
typedef int32_t (*IMP_ISP_Tuning_SetCsc_Attr_t)(IMPISPCscAttr *attr);
typedef int32_t (*IMP_ISP_Tuning_GetCsc_Attr_t)(IMPISPCscAttr *attr);
typedef int32_t (*IMP_ISP_Tuning_SetWdr_OutputMode_t)(IMPISPWdrOutputMode *mode);
typedef int32_t (*IMP_ISP_Tuning_GetWdr_OutputMode_t)(IMPISPWdrOutputMode *mode);
typedef int32_t (*IMP_ISP_SetFrameDrop_t)(IMPISPFrameDropAttr *attr);
typedef int32_t (*IMP_ISP_GetFrameDrop_t)(IMPISPFrameDropAttr *attr);
typedef int32_t (*IMP_ISP_SetFixedContraster_t)(IMPISPFixedContrastAttr *attr);
typedef int32_t (*IMP_ISP_SET_GPIO_INIT_OR_FREE_t)(IMPISPGPIO *attr);
typedef int32_t (*IMP_ISP_SET_GPIO_STA_t)(IMPISPGPIO *attr);
typedef int (*IMP_System_Init_t)(void);
typedef int (*IMP_System_Exit_t)(void);
typedef int64_t (*IMP_System_GetTimeStamp_t)(void);
typedef int (*IMP_System_RebaseTimeStamp_t)(int64_t basets);
typedef uint32_t (*IMP_System_ReadReg32_t)(uint32_t regAddr);
typedef void (*IMP_System_WriteReg32_t)(uint32_t regAddr, uint32_t value);
typedef int (*IMP_System_GetVersion_t)(IMPVersion *pstVersion);
typedef int (*IMP_System_Bind_t)(IMPCell *srcCell, IMPCell *dstCell);
typedef int (*IMP_System_UnBind_t)(IMPCell *srcCell, IMPCell *dstCell);
typedef int (*IMP_System_GetBindbyDest_t)(IMPCell *dstCell, IMPCell *srcCell);
typedef int (*IMP_System_MemPoolRequest_t)(int poolId, size_t size, const char *name);
typedef int (*IMP_System_MemPoolFree_t)(int poolId);
typedef int (*IMP_Encoder_CreateGroup_t)(int encGroup);
typedef int (*IMP_Encoder_DestroyGroup_t)(int encGroup);
typedef int (*IMP_Encoder_SetDefaultParam_t)(IMPEncoderChnAttr *chnAttr, IMPEncoderProfile profile, IMPEncoderRcMode rcMode, uint16_t uWidth, uint16_t uHeight, uint32_t frmRateNum, uint32_t frmRateDen, uint32_t uGopLength, int uMaxSameSenceCnt, int iInitialQP, uint32_t uTargetBitRate);
typedef int (*IMP_Encoder_CreateChn_t)(int encChn, const IMPEncoderChnAttr *attr);
typedef int (*IMP_Encoder_DestroyChn_t)(int encChn);
typedef int (*IMP_Encoder_GetChnAttr_t)(int encChn, IMPEncoderChnAttr * const attr);
typedef int (*IMP_Encoder_RegisterChn_t)(int encGroup, int encChn);
typedef int (*IMP_Encoder_UnRegisterChn_t)(int encChn);
typedef int (*IMP_Encoder_StartRecvPic_t)(int encChn);
typedef int (*IMP_Encoder_StopRecvPic_t)(int encChn);
typedef int (*IMP_Encoder_Query_t)(int encChn, IMPEncoderChnStat *stat);
typedef int (*IMP_Encoder_GetStream_t)(int encChn, IMPEncoderStream *stream, bool blockFlag);
typedef int (*IMP_Encoder_ReleaseStream_t)(int encChn, IMPEncoderStream *stream);
typedef int (*IMP_Encoder_PollingStream_t)(int encChn, uint32_t timeoutMsec);
typedef int (*IMP_Encoder_PollingModuleStream_t)(uint32_t *encChnBitmap, uint32_t timeoutMsec);
typedef int (*IMP_Encoder_GetFd_t)(int encChn);
typedef int (*IMP_Encoder_SetbufshareChn_t)(int encChn, int shareChn);
typedef int (*IMP_Encoder_SetChnResizeMode_t)(int encChn, int en);
typedef int (*IMP_Encoder_SetMaxStreamCnt_t)(int encChn, int nrMaxStream);
typedef int (*IMP_Encoder_GetMaxStreamCnt_t)(int encChn, int *nrMaxStream);
typedef int (*IMP_Encoder_RequestIDR_t)(int encChn);
typedef int (*IMP_Encoder_FlushStream_t)(int encChn);
typedef int (*IMP_Encoder_GetChnFrmRate_t)(int encChn, IMPEncoderFrmRate *pstFps);
typedef int (*IMP_Encoder_SetChnFrmRate_t)(int encChn, const IMPEncoderFrmRate *pstFps);
typedef int (*IMP_Encoder_SetChnBitRate_t)(int encChn, int iTargetBitRate, int iMaxBitRate);
typedef int (*IMP_Encoder_SetChnGopLength_t)(int encChn, int iGopLength);
typedef int (*IMP_Encoder_GetChnAttrRcMode_t)(int encChn, IMPEncoderAttrRcMode *pstRcModeCfg);
typedef int (*IMP_Encoder_SetChnAttrRcMode_t)(int encChn, const IMPEncoderAttrRcMode *pstRcModeCfg);
typedef int (*IMP_Encoder_GetChnGopAttr_t)(int encChn, IMPEncoderGopAttr *pGopAttr);
typedef int (*IMP_Encoder_SetChnGopAttr_t)(int encChn, const IMPEncoderGopAttr *pGopAttr);
typedef int (*IMP_Encoder_SetChnQp_t)(int encChn, int iQP);
typedef int (*IMP_Encoder_SetChnQpBounds_t)(int encChn, int iMinQP, int iMaxQP);
typedef int (*IMP_Encoder_SetChnQpIPDelta_t)(int encChn, int uIPDelta);
typedef int (*IMP_Encoder_SetFisheyeEnableStatus_t)(int encChn, int enable);
typedef int (*IMP_Encoder_GetFisheyeEnableStatus_t)(int encChn, int *enable);
typedef int (*IMP_Encoder_GetChnEncType_t)(int encChn, IMPEncoderEncType *encType);
typedef int (*IMP_Encoder_SetPool_t)(int chnNum, int poolID);
typedef int (*IMP_Encoder_GetPool_t)(int chnNum);
typedef int (*IMP_Encoder_SetStreamBufSize_t)(int encChn, uint32_t nrStreamSize);
typedef int (*IMP_Encoder_GetStreamBufSize_t)(int encChn, uint32_t *nrStreamSize);
typedef int (*IMP_Encoder_GetChnAveBitrate_t)(int encChn, IMPEncoderStream *stream, int frames, double *br);
typedef int (*IMP_Encoder_GetChnEvalInfo_t)(int encChn, void *info);
typedef int (*IMP_Encoder_SetChnEntropyMode_t)(int encChn,IMPEncoderEntropyMode eEntropyMode);
typedef int (*IMP_Encoder_SetFrameRelease_t)(int encChn, int num, int den);
typedef IMPIVSInterface (**IMP_IVS_CreateMoveInterface_t)(IMP_IVS_MoveParam *param);
typedef void (*IMP_IVS_DestroyMoveInterface_t)(IMPIVSInterface *moveInterface);
/*typedef void (*imp_log_fun_t)(int le, int op, int out, const char* tag, const char* file, int line, const char* func, const char* fmt, ...);
typedef void (*IMP_Log_Set_Option_t)(int op);
typedef int (*IMP_Log_Get_Option_t)(void);*/
typedef int (*IMP_FrameSource_CreateChn_t)(int chnNum, IMPFSChnAttr *chn_attr);
typedef int (*IMP_FrameSource_DestroyChn_t)(int chnNum);
typedef int (*IMP_FrameSource_EnableChn_t)(int chnNum);
typedef int (*IMP_FrameSource_DisableChn_t)(int chnNum);
typedef int (*IMP_FrameSource_SetSource_t)(int extchnNum, int sourcechnNum);
typedef int (*IMP_FrameSource_GetChnAttr_t)(int chnNum, IMPFSChnAttr *chnAttr);
typedef int (*IMP_FrameSource_SetChnAttr_t)(int chnNum, const IMPFSChnAttr *chnAttr);
typedef int (*IMP_FrameSource_SetFrameDepth_t)(int chnNum, int depth);
typedef int (*IMP_FrameSource_GetFrameDepth_t)(int chnNum, int *depth);
typedef int (*IMP_FrameSource_GetFrame_t)(int chnNum, IMPFrameInfo **frame);
typedef int (*IMP_FrameSource_GetTimedFrame_t)(int chnNum, IMPFrameTimestamp *framets, int block, void *framedata, IMPFrameInfo *frame);
typedef int (*IMP_FrameSource_ReleaseFrame_t)(int chnNum, IMPFrameInfo *frame);
typedef int (*IMP_FrameSource_SnapFrame_t)(int chnNum, IMPPixelFormat fmt, int width, int height, void *framedata, IMPFrameInfo *frame);
typedef int (*IMP_FrameSource_SetMaxDelay_t)(int chnNum, int maxcnt);
typedef int (*IMP_FrameSource_GetMaxDelay_t)(int chnNum, int *maxcnt);
typedef int (*IMP_FrameSource_SetDelay_t)(int chnNum, int cnt);
typedef int (*IMP_FrameSource_GetDelay_t)(int chnNum, int *cnt);
typedef int (*IMP_FrameSource_SetChnFifoAttr_t)(int chnNum, IMPFSChnFifoAttr *attr);
typedef int (*IMP_FrameSource_GetChnFifoAttr_t)(int chnNum, IMPFSChnFifoAttr *attr);
typedef int (*IMP_FrameSource_SetPool_t)(int chnNum, int poolID);
typedef int (*IMP_FrameSource_GetPool_t)(int chnNum);
typedef int (*IMP_FrameSource_ChnStatQuery_t)(int chnNum, IMPFSChannelState *pstate);
typedef int (*IMP_FrameSource_SetChnRotate_t)(int chnNum, uint8_t rotTo90, int width, int height);

extern IMP_System_GetCPUInfo_t IMP_System_GetCPUInfo_ptr;
extern IMP_IVS_CreateGroup_t IMP_IVS_CreateGroup_ptr;
extern IMP_IVS_DestroyGroup_t IMP_IVS_DestroyGroup_ptr;
extern IMP_IVS_CreateChn_t IMP_IVS_CreateChn_ptr;
extern IMP_IVS_DestroyChn_t IMP_IVS_DestroyChn_ptr;
extern IMP_IVS_RegisterChn_t IMP_IVS_RegisterChn_ptr;
extern IMP_IVS_UnRegisterChn_t IMP_IVS_UnRegisterChn_ptr;
extern IMP_IVS_StartRecvPic_t IMP_IVS_StartRecvPic_ptr;
extern IMP_IVS_StopRecvPic_t IMP_IVS_StopRecvPic_ptr;
extern IMP_IVS_PollingResult_t IMP_IVS_PollingResult_ptr;
extern IMP_IVS_GetResult_t IMP_IVS_GetResult_ptr;
extern IMP_IVS_ReleaseResult_t IMP_IVS_ReleaseResult_ptr;
extern IMP_IVS_ReleaseData_t IMP_IVS_ReleaseData_ptr;
extern IMP_IVS_GetParam_t IMP_IVS_GetParam_ptr;
extern IMP_IVS_SetParam_t IMP_IVS_SetParam_ptr;
extern IMP_DMIC_SetUserInfo_t IMP_DMIC_SetUserInfo_ptr;
extern IMP_DMIC_SetPubAttr_t IMP_DMIC_SetPubAttr_ptr;
extern IMP_DMIC_GetPubAttr_t IMP_DMIC_GetPubAttr_ptr;
extern IMP_DMIC_Enable_t IMP_DMIC_Enable_ptr;
extern IMP_DMIC_Disable_t IMP_DMIC_Disable_ptr;
extern IMP_DMIC_EnableChn_t IMP_DMIC_EnableChn_ptr;
extern IMP_DMIC_DisableChn_t IMP_DMIC_DisableChn_ptr;
extern IMP_DMIC_SetChnParam_t IMP_DMIC_SetChnParam_ptr;
extern IMP_DMIC_GetChnParam_t IMP_DMIC_GetChnParam_ptr;
extern IMP_DMIC_GetFrame_t IMP_DMIC_GetFrame_ptr;
extern IMP_DMIC_ReleaseFrame_t IMP_DMIC_ReleaseFrame_ptr;
extern IMP_DMIC_EnableAecRefFrame_t IMP_DMIC_EnableAecRefFrame_ptr;
extern IMP_DMIC_GetFrameAndRef_t IMP_DMIC_GetFrameAndRef_ptr;
extern IMP_DMIC_EnableAec_t IMP_DMIC_EnableAec_ptr;
extern IMP_DMIC_DisableAec_t IMP_DMIC_DisableAec_ptr;
extern IMP_DMIC_PollingFrame_t IMP_DMIC_PollingFrame_ptr;
extern IMP_DMIC_SetVol_t IMP_DMIC_SetVol_ptr;
extern IMP_DMIC_GetVol_t IMP_DMIC_GetVol_ptr;
extern IMP_DMIC_SetGain_t IMP_DMIC_SetGain_ptr;
extern IMP_DMIC_GetGain_t IMP_DMIC_GetGain_ptr;
extern IMP_AI_SetPubAttr_t IMP_AI_SetPubAttr_ptr;
extern IMP_AI_GetPubAttr_t IMP_AI_GetPubAttr_ptr;
extern IMP_AI_Enable_t IMP_AI_Enable_ptr;
extern IMP_AI_Disable_t IMP_AI_Disable_ptr;
extern IMP_AI_EnableChn_t IMP_AI_EnableChn_ptr;
extern IMP_AI_DisableChn_t IMP_AI_DisableChn_ptr;
extern IMP_AI_PollingFrame_t IMP_AI_PollingFrame_ptr;
extern IMP_AI_GetFrame_t IMP_AI_GetFrame_ptr;
extern IMP_AI_ReleaseFrame_t IMP_AI_ReleaseFrame_ptr;
extern IMP_AI_SetChnParam_t IMP_AI_SetChnParam_ptr;
extern IMP_AI_GetChnParam_t IMP_AI_GetChnParam_ptr;
extern IMP_AI_EnableAec_t IMP_AI_EnableAec_ptr;
extern IMP_AI_Set_WebrtcProfileIni_Path_t IMP_AI_Set_WebrtcProfileIni_Path_ptr;
extern IMP_AI_DisableAec_t IMP_AI_DisableAec_ptr;
extern IMP_AI_EnableNs_t IMP_AI_EnableNs_ptr;
extern IMP_AI_DisableNs_t IMP_AI_DisableNs_ptr;
extern IMP_AI_SetAgcMode_t IMP_AI_SetAgcMode_ptr;
extern IMP_AI_EnableAgc_t IMP_AI_EnableAgc_ptr;
extern IMP_AI_DisableAgc_t IMP_AI_DisableAgc_ptr;
extern IMP_AO_EnableAgc_t IMP_AO_EnableAgc_ptr;
extern IMP_AO_DisableAgc_t IMP_AO_DisableAgc_ptr;
extern IMP_AI_EnableHpf_t IMP_AI_EnableHpf_ptr;
extern IMP_AI_SetHpfCoFrequency_t IMP_AI_SetHpfCoFrequency_ptr;
extern IMP_AI_DisableHpf_t IMP_AI_DisableHpf_ptr;
extern IMP_AO_EnableHpf_t IMP_AO_EnableHpf_ptr;
extern IMP_AO_SetHpfCoFrequency_t IMP_AO_SetHpfCoFrequency_ptr;
extern IMP_AO_DisableHpf_t IMP_AO_DisableHpf_ptr;
extern IMP_AO_SetPubAttr_t IMP_AO_SetPubAttr_ptr;
extern IMP_AO_GetPubAttr_t IMP_AO_GetPubAttr_ptr;
extern IMP_AO_Enable_t IMP_AO_Enable_ptr;
extern IMP_AO_Disable_t IMP_AO_Disable_ptr;
extern IMP_AO_EnableChn_t IMP_AO_EnableChn_ptr;
extern IMP_AO_DisableChn_t IMP_AO_DisableChn_ptr;
extern IMP_AO_SendFrame_t IMP_AO_SendFrame_ptr;
extern IMP_AO_PauseChn_t IMP_AO_PauseChn_ptr;
extern IMP_AO_ResumeChn_t IMP_AO_ResumeChn_ptr;
extern IMP_AO_ClearChnBuf_t IMP_AO_ClearChnBuf_ptr;
extern IMP_AO_QueryChnStat_t IMP_AO_QueryChnStat_ptr;
extern IMP_AENC_CreateChn_t IMP_AENC_CreateChn_ptr;
extern IMP_AENC_DestroyChn_t IMP_AENC_DestroyChn_ptr;
extern IMP_AENC_SendFrame_t IMP_AENC_SendFrame_ptr;
extern IMP_AENC_PollingStream_t IMP_AENC_PollingStream_ptr;
extern IMP_AENC_GetStream_t IMP_AENC_GetStream_ptr;
extern IMP_AENC_ReleaseStream_t IMP_AENC_ReleaseStream_ptr;
extern IMP_AENC_RegisterEncoder_t IMP_AENC_RegisterEncoder_ptr;
extern IMP_AENC_UnRegisterEncoder_t IMP_AENC_UnRegisterEncoder_ptr;
extern IMP_ADEC_CreateChn_t IMP_ADEC_CreateChn_ptr;
extern IMP_ADEC_DestroyChn_t IMP_ADEC_DestroyChn_ptr;
extern IMP_ADEC_SendStream_t IMP_ADEC_SendStream_ptr;
extern IMP_ADEC_PollingStream_t IMP_ADEC_PollingStream_ptr;
extern IMP_ADEC_GetStream_t IMP_ADEC_GetStream_ptr;
extern IMP_ADEC_ReleaseStream_t IMP_ADEC_ReleaseStream_ptr;
extern IMP_ADEC_ClearChnBuf_t IMP_ADEC_ClearChnBuf_ptr;
extern IMP_ADEC_RegisterDecoder_t IMP_ADEC_RegisterDecoder_ptr;
extern IMP_ADEC_UnRegisterDecoder_t IMP_ADEC_UnRegisterDecoder_ptr;
extern IMP_AI_SetVol_t IMP_AI_SetVol_ptr;
extern IMP_AI_GetVol_t IMP_AI_GetVol_ptr;
extern IMP_AI_SetVolMute_t IMP_AI_SetVolMute_ptr;
extern IMP_AO_SetVol_t IMP_AO_SetVol_ptr;
extern IMP_AO_GetVol_t IMP_AO_GetVol_ptr;
extern IMP_AO_SetVolMute_t IMP_AO_SetVolMute_ptr;
extern IMP_AI_SetGain_t IMP_AI_SetGain_ptr;
extern IMP_AI_GetGain_t IMP_AI_GetGain_ptr;
extern IMP_AI_SetAlcGain_t IMP_AI_SetAlcGain_ptr;
extern IMP_AI_GetAlcGain_t IMP_AI_GetAlcGain_ptr;
extern IMP_AO_SetGain_t IMP_AO_SetGain_ptr;
extern IMP_AO_GetGain_t IMP_AO_GetGain_ptr;
extern IMP_AO_Soft_Mute_t IMP_AO_Soft_Mute_ptr;
extern IMP_AO_Soft_UNMute_t IMP_AO_Soft_UNMute_ptr;
extern IMP_AI_GetFrameAndRef_t IMP_AI_GetFrameAndRef_ptr;
extern IMP_AI_EnableAecRefFrame_t IMP_AI_EnableAecRefFrame_ptr;
extern IMP_AI_DisableAecRefFrame_t IMP_AI_DisableAecRefFrame_ptr;
extern IMP_AO_CacheSwitch_t IMP_AO_CacheSwitch_ptr;
extern IMP_AO_FlushChnBuf_t IMP_AO_FlushChnBuf_ptr;
extern IMP_IVS_CreateBaseMoveInterface_t *IMP_IVS_CreateBaseMoveInterface_ptr;
extern IMP_IVS_DestroyBaseMoveInterface_t IMP_IVS_DestroyBaseMoveInterface_ptr;
extern IMPPixfmtToString_t *IMPPixfmtToString_ptr;
extern IMP_OSD_CreateGroup_t IMP_OSD_CreateGroup_ptr;
extern IMP_OSD_DestroyGroup_t IMP_OSD_DestroyGroup_ptr;
extern IMP_OSD_AttachToGroup_t IMP_OSD_AttachToGroup_ptr;
extern IMP_OSD_CreateRgn_t IMP_OSD_CreateRgn_ptr;
extern IMP_OSD_DestroyRgn_t IMP_OSD_DestroyRgn_ptr;
extern IMP_OSD_RegisterRgn_t IMP_OSD_RegisterRgn_ptr;
extern IMP_OSD_UnRegisterRgn_t IMP_OSD_UnRegisterRgn_ptr;
extern IMP_OSD_SetRgnAttr_t IMP_OSD_SetRgnAttr_ptr;
extern IMP_OSD_SetRgnAttrWithTimestamp_t IMP_OSD_SetRgnAttrWithTimestamp_ptr;
extern IMP_OSD_GetRgnAttr_t IMP_OSD_GetRgnAttr_ptr;
extern IMP_OSD_UpdateRgnAttrData_t IMP_OSD_UpdateRgnAttrData_ptr;
extern IMP_OSD_SetGrpRgnAttr_t IMP_OSD_SetGrpRgnAttr_ptr;
extern IMP_OSD_GetGrpRgnAttr_t IMP_OSD_GetGrpRgnAttr_ptr;
extern IMP_OSD_ShowRgn_t IMP_OSD_ShowRgn_ptr;
extern IMP_OSD_Start_t IMP_OSD_Start_ptr;
extern IMP_OSD_Stop_t IMP_OSD_Stop_ptr;
extern IMP_ISP_Open_t IMP_ISP_Open_ptr;
extern IMP_ISP_Close_t IMP_ISP_Close_ptr;
extern IMP_ISP_SetDefaultBinPath_t IMP_ISP_SetDefaultBinPath_ptr;
extern IMP_ISP_GetDefaultBinPath_t IMP_ISP_GetDefaultBinPath_ptr;
extern IMP_ISP_AddSensor_t IMP_ISP_AddSensor_ptr;
extern IMP_ISP_DelSensor_t IMP_ISP_DelSensor_ptr;
extern IMP_ISP_EnableSensor_t IMP_ISP_EnableSensor_ptr;
extern IMP_ISP_DisableSensor_t IMP_ISP_DisableSensor_ptr;
extern IMP_ISP_SetSensorRegister_t IMP_ISP_SetSensorRegister_ptr;
extern IMP_ISP_GetSensorRegister_t IMP_ISP_GetSensorRegister_ptr;
extern IMP_ISP_Tuning_SetAutoZoom_t IMP_ISP_Tuning_SetAutoZoom_ptr;
extern IMP_ISP_EnableTuning_t IMP_ISP_EnableTuning_ptr;
extern IMP_ISP_DisableTuning_t IMP_ISP_DisableTuning_ptr;
extern IMP_ISP_Tuning_SetSensorFPS_t IMP_ISP_Tuning_SetSensorFPS_ptr;
extern IMP_ISP_Tuning_GetSensorFPS_t IMP_ISP_Tuning_GetSensorFPS_ptr;
extern IMP_ISP_Tuning_SetAntiFlickerAttr_t IMP_ISP_Tuning_SetAntiFlickerAttr_ptr;
extern IMP_ISP_Tuning_GetAntiFlickerAttr_t IMP_ISP_Tuning_GetAntiFlickerAttr_ptr;
extern IMP_ISP_Tuning_SetBrightness_t IMP_ISP_Tuning_SetBrightness_ptr;
extern IMP_ISP_Tuning_GetBrightness_t IMP_ISP_Tuning_GetBrightness_ptr;
extern IMP_ISP_Tuning_SetContrast_t IMP_ISP_Tuning_SetContrast_ptr;
extern IMP_ISP_Tuning_GetContrast_t IMP_ISP_Tuning_GetContrast_ptr;
extern IMP_ISP_Tuning_SetSharpness_t IMP_ISP_Tuning_SetSharpness_ptr;
extern IMP_ISP_Tuning_GetSharpness_t IMP_ISP_Tuning_GetSharpness_ptr;
extern IMP_ISP_Tuning_SetBcshHue_t IMP_ISP_Tuning_SetBcshHue_ptr;
extern IMP_ISP_Tuning_GetBcshHue_t IMP_ISP_Tuning_GetBcshHue_ptr;
extern IMP_ISP_Tuning_SetSaturation_t IMP_ISP_Tuning_SetSaturation_ptr;
extern IMP_ISP_Tuning_GetSaturation_t IMP_ISP_Tuning_GetSaturation_ptr;
extern IMP_ISP_Tuning_SetISPBypass_t IMP_ISP_Tuning_SetISPBypass_ptr;
extern IMP_ISP_Tuning_GetTotalGain_t IMP_ISP_Tuning_GetTotalGain_ptr;
extern IMP_ISP_Tuning_SetISPHflip_t IMP_ISP_Tuning_SetISPHflip_ptr;
extern IMP_ISP_Tuning_GetISPHflip_t IMP_ISP_Tuning_GetISPHflip_ptr;
extern IMP_ISP_Tuning_SetISPVflip_t IMP_ISP_Tuning_SetISPVflip_ptr;
extern IMP_ISP_Tuning_GetISPVflip_t IMP_ISP_Tuning_GetISPVflip_ptr;
extern IMP_ISP_Tuning_SetISPRunningMode_t IMP_ISP_Tuning_SetISPRunningMode_ptr;
extern IMP_ISP_Tuning_GetISPRunningMode_t IMP_ISP_Tuning_GetISPRunningMode_ptr;
extern IMP_ISP_Tuning_SetISPCustomMode_t IMP_ISP_Tuning_SetISPCustomMode_ptr;
extern IMP_ISP_Tuning_GetISPCustomMode_t IMP_ISP_Tuning_GetISPCustomMode_ptr;
extern IMP_ISP_Tuning_SetGamma_t IMP_ISP_Tuning_SetGamma_ptr;
extern IMP_ISP_Tuning_GetGamma_t IMP_ISP_Tuning_GetGamma_ptr;
extern IMP_ISP_Tuning_SetAeComp_t IMP_ISP_Tuning_SetAeComp_ptr;
extern IMP_ISP_Tuning_GetAeComp_t IMP_ISP_Tuning_GetAeComp_ptr;
extern IMP_ISP_Tuning_GetAeLuma_t IMP_ISP_Tuning_GetAeLuma_ptr;
extern IMP_ISP_Tuning_SetAeFreeze_t IMP_ISP_Tuning_SetAeFreeze_ptr;
extern IMP_ISP_Tuning_SetExpr_t IMP_ISP_Tuning_SetExpr_ptr;
extern IMP_ISP_Tuning_GetExpr_t IMP_ISP_Tuning_GetExpr_ptr;
extern IMP_ISP_Tuning_SetWB_t IMP_ISP_Tuning_SetWB_ptr;
extern IMP_ISP_Tuning_GetWB_t IMP_ISP_Tuning_GetWB_ptr;
extern IMP_ISP_Tuning_GetWB_Statis_t IMP_ISP_Tuning_GetWB_Statis_ptr;
extern IMP_ISP_Tuning_GetWB_GOL_Statis_t IMP_ISP_Tuning_GetWB_GOL_Statis_ptr;
extern IMP_ISP_Tuning_SetAwbClust_t IMP_ISP_Tuning_SetAwbClust_ptr;
extern IMP_ISP_Tuning_GetAwbClust_t IMP_ISP_Tuning_GetAwbClust_ptr;
extern IMP_ISP_Tuning_SetAwbCtTrend_t IMP_ISP_Tuning_SetAwbCtTrend_ptr;
extern IMP_ISP_Tuning_GetAwbCtTrend_t IMP_ISP_Tuning_GetAwbCtTrend_ptr;
extern IMP_ISP_Tuning_Awb_GetRgbCoefft_t IMP_ISP_Tuning_Awb_GetRgbCoefft_ptr;
extern IMP_ISP_Tuning_Awb_SetRgbCoefft_t IMP_ISP_Tuning_Awb_SetRgbCoefft_ptr;
extern IMP_ISP_Tuning_SetMaxAgain_t IMP_ISP_Tuning_SetMaxAgain_ptr;
extern IMP_ISP_Tuning_GetMaxAgain_t IMP_ISP_Tuning_GetMaxAgain_ptr;
extern IMP_ISP_Tuning_SetMaxDgain_t IMP_ISP_Tuning_SetMaxDgain_ptr;
extern IMP_ISP_Tuning_GetMaxDgain_t IMP_ISP_Tuning_GetMaxDgain_ptr;
extern IMP_ISP_Tuning_SetHiLightDepress_t IMP_ISP_Tuning_SetHiLightDepress_ptr;
extern IMP_ISP_Tuning_GetHiLightDepress_t IMP_ISP_Tuning_GetHiLightDepress_ptr;
extern IMP_ISP_Tuning_SetBacklightComp_t IMP_ISP_Tuning_SetBacklightComp_ptr;
extern IMP_ISP_Tuning_GetBacklightComp_t IMP_ISP_Tuning_GetBacklightComp_ptr;
extern IMP_ISP_Tuning_SetTemperStrength_t IMP_ISP_Tuning_SetTemperStrength_ptr;
extern IMP_ISP_Tuning_SetSinterStrength_t IMP_ISP_Tuning_SetSinterStrength_ptr;
extern IMP_ISP_Tuning_GetEVAttr_t IMP_ISP_Tuning_GetEVAttr_ptr;
extern IMP_ISP_Tuning_EnableMovestate_t IMP_ISP_Tuning_EnableMovestate_ptr;
extern IMP_ISP_Tuning_DisableMovestate_t IMP_ISP_Tuning_DisableMovestate_ptr;
extern IMP_ISP_Tuning_SetAeWeight_t IMP_ISP_Tuning_SetAeWeight_ptr;
extern IMP_ISP_Tuning_GetAeWeight_t IMP_ISP_Tuning_GetAeWeight_ptr;
extern IMP_ISP_Tuning_AE_GetROI_t IMP_ISP_Tuning_AE_GetROI_ptr;
extern IMP_ISP_Tuning_AE_SetROI_t IMP_ISP_Tuning_AE_SetROI_ptr;
extern IMP_ISP_Tuning_SetAwbWeight_t IMP_ISP_Tuning_SetAwbWeight_ptr;
extern IMP_ISP_Tuning_GetAwbWeight_t IMP_ISP_Tuning_GetAwbWeight_ptr;
extern IMP_ISP_Tuning_GetAwbZone_t IMP_ISP_Tuning_GetAwbZone_ptr;
extern IMP_ISP_Tuning_SetWB_ALGO_t IMP_ISP_Tuning_SetWB_ALGO_ptr;
extern IMP_ISP_Tuning_SetAeHist_t IMP_ISP_Tuning_SetAeHist_ptr;
extern IMP_ISP_Tuning_GetAeHist_t IMP_ISP_Tuning_GetAeHist_ptr;
extern IMP_ISP_Tuning_GetAeHist_Origin_t IMP_ISP_Tuning_GetAeHist_Origin_ptr;
extern IMP_ISP_Tuning_GetAwbHist_t IMP_ISP_Tuning_GetAwbHist_ptr;
extern IMP_ISP_Tuning_SetAwbHist_t IMP_ISP_Tuning_SetAwbHist_ptr;
extern IMP_ISP_Tuning_GetAFMetrices_t IMP_ISP_Tuning_GetAFMetrices_ptr;
extern IMP_ISP_Tuning_GetAfHist_t IMP_ISP_Tuning_GetAfHist_ptr;
extern IMP_ISP_Tuning_SetAfHist_t IMP_ISP_Tuning_SetAfHist_ptr;
extern IMP_ISP_Tuning_SetAfWeight_t IMP_ISP_Tuning_SetAfWeight_ptr;
extern IMP_ISP_Tuning_GetAfWeight_t IMP_ISP_Tuning_GetAfWeight_ptr;
extern IMP_ISP_Tuning_GetAfZone_t IMP_ISP_Tuning_GetAfZone_ptr;
extern IMP_ISP_Tuning_WaitFrame_t IMP_ISP_Tuning_WaitFrame_ptr;
extern IMP_ISP_Tuning_SetAeMin_t IMP_ISP_Tuning_SetAeMin_ptr;
extern IMP_ISP_Tuning_GetAeMin_t IMP_ISP_Tuning_GetAeMin_ptr;
extern IMP_ISP_Tuning_SetAe_IT_MAX_t IMP_ISP_Tuning_SetAe_IT_MAX_ptr;
extern IMP_ISP_Tuning_GetAE_IT_MAX_t IMP_ISP_Tuning_GetAE_IT_MAX_ptr;
extern IMP_ISP_Tuning_GetAeZone_t IMP_ISP_Tuning_GetAeZone_ptr;
extern IMP_ISP_Tuning_SetAeTargetList_t IMP_ISP_Tuning_SetAeTargetList_ptr;
extern IMP_ISP_Tuning_GetAeTargetList_t IMP_ISP_Tuning_GetAeTargetList_ptr;
extern IMP_ISP_Tuning_SetModuleControl_t IMP_ISP_Tuning_SetModuleControl_ptr;
extern IMP_ISP_Tuning_GetModuleControl_t IMP_ISP_Tuning_GetModuleControl_ptr;
extern IMP_ISP_Tuning_SetFrontCrop_t IMP_ISP_Tuning_SetFrontCrop_ptr;
extern IMP_ISP_Tuning_GetFrontCrop_t IMP_ISP_Tuning_GetFrontCrop_ptr;
extern IMP_ISP_WDR_ENABLE_t IMP_ISP_WDR_ENABLE_ptr;
extern IMP_ISP_WDR_ENABLE_Get_t IMP_ISP_WDR_ENABLE_Get_ptr;
extern IMP_ISP_Tuning_SetDPC_Strength_t IMP_ISP_Tuning_SetDPC_Strength_ptr;
extern IMP_ISP_Tuning_GetDPC_Strength_t IMP_ISP_Tuning_GetDPC_Strength_ptr;
extern IMP_ISP_Tuning_SetDRC_Strength_t IMP_ISP_Tuning_SetDRC_Strength_ptr;
extern IMP_ISP_Tuning_GetDRC_Strength_t IMP_ISP_Tuning_GetDRC_Strength_ptr;
extern IMP_ISP_Tuning_SetHVFLIP_t IMP_ISP_Tuning_SetHVFLIP_ptr;
extern IMP_ISP_Tuning_GetHVFlip_t IMP_ISP_Tuning_GetHVFlip_ptr;
extern IMP_ISP_Tuning_SetMask_t IMP_ISP_Tuning_SetMask_ptr;
extern IMP_ISP_Tuning_GetMask_t IMP_ISP_Tuning_GetMask_ptr;
extern IMP_ISP_Tuning_GetSensorAttr_t IMP_ISP_Tuning_GetSensorAttr_ptr;
extern IMP_ISP_Tuning_EnableDRC_t IMP_ISP_Tuning_EnableDRC_ptr;
extern IMP_ISP_Tuning_EnableDefog_t IMP_ISP_Tuning_EnableDefog_ptr;
extern IMP_ISP_Tuning_SetAwbCt_t IMP_ISP_Tuning_SetAwbCt_ptr;
extern IMP_ISP_Tuning_GetAWBCt_t IMP_ISP_Tuning_GetAWBCt_ptr;
extern IMP_ISP_Tuning_SetCCMAttr_t IMP_ISP_Tuning_SetCCMAttr_ptr;
extern IMP_ISP_Tuning_GetCCMAttr_t IMP_ISP_Tuning_GetCCMAttr_ptr;
extern IMP_ISP_Tuning_SetAeAttr_t IMP_ISP_Tuning_SetAeAttr_ptr;
extern IMP_ISP_Tuning_GetAeAttr_t IMP_ISP_Tuning_GetAeAttr_ptr;
extern IMP_ISP_Tuning_GetAeState_t IMP_ISP_Tuning_GetAeState_ptr;
extern IMP_ISP_Tuning_SetScalerLv_t IMP_ISP_Tuning_SetScalerLv_ptr;
extern IMP_ISP_SetAeAlgoFunc_t IMP_ISP_SetAeAlgoFunc_ptr;
extern IMP_ISP_SetAwbAlgoFunc_t IMP_ISP_SetAwbAlgoFunc_ptr;
extern IMP_ISP_Tuning_GetBlcAttr_t IMP_ISP_Tuning_GetBlcAttr_ptr;
extern IMP_ISP_Tuning_SetDefog_Strength_t IMP_ISP_Tuning_SetDefog_Strength_ptr;
extern IMP_ISP_Tuning_GetDefog_Strength_t IMP_ISP_Tuning_GetDefog_Strength_ptr;
extern IMP_ISP_Tuning_SetCsc_Attr_t IMP_ISP_Tuning_SetCsc_Attr_ptr;
extern IMP_ISP_Tuning_GetCsc_Attr_t IMP_ISP_Tuning_GetCsc_Attr_ptr;
extern IMP_ISP_Tuning_SetWdr_OutputMode_t IMP_ISP_Tuning_SetWdr_OutputMode_ptr;
extern IMP_ISP_Tuning_GetWdr_OutputMode_t IMP_ISP_Tuning_GetWdr_OutputMode_ptr;
extern IMP_ISP_SetFrameDrop_t IMP_ISP_SetFrameDrop_ptr;
extern IMP_ISP_GetFrameDrop_t IMP_ISP_GetFrameDrop_ptr;
extern IMP_ISP_SetFixedContraster_t IMP_ISP_SetFixedContraster_ptr;
extern IMP_ISP_SET_GPIO_INIT_OR_FREE_t IMP_ISP_SET_GPIO_INIT_OR_FREE_ptr;
extern IMP_ISP_SET_GPIO_STA_t IMP_ISP_SET_GPIO_STA_ptr;
extern IMP_System_Init_t IMP_System_Init_ptr;
extern IMP_System_Exit_t IMP_System_Exit_ptr;
extern IMP_System_GetTimeStamp_t IMP_System_GetTimeStamp_ptr;
extern IMP_System_RebaseTimeStamp_t IMP_System_RebaseTimeStamp_ptr;
extern IMP_System_ReadReg32_t IMP_System_ReadReg32_ptr;
extern IMP_System_WriteReg32_t IMP_System_WriteReg32_ptr;
extern IMP_System_GetVersion_t IMP_System_GetVersion_ptr;
extern IMP_System_Bind_t IMP_System_Bind_ptr;
extern IMP_System_UnBind_t IMP_System_UnBind_ptr;
extern IMP_System_GetBindbyDest_t IMP_System_GetBindbyDest_ptr;
extern IMP_System_MemPoolRequest_t IMP_System_MemPoolRequest_ptr;
extern IMP_System_MemPoolFree_t IMP_System_MemPoolFree_ptr;
extern IMP_Encoder_CreateGroup_t IMP_Encoder_CreateGroup_ptr;
extern IMP_Encoder_DestroyGroup_t IMP_Encoder_DestroyGroup_ptr;
extern IMP_Encoder_SetDefaultParam_t IMP_Encoder_SetDefaultParam_ptr;
extern IMP_Encoder_CreateChn_t IMP_Encoder_CreateChn_ptr;
extern IMP_Encoder_DestroyChn_t IMP_Encoder_DestroyChn_ptr;
extern IMP_Encoder_GetChnAttr_t IMP_Encoder_GetChnAttr_ptr;
extern IMP_Encoder_RegisterChn_t IMP_Encoder_RegisterChn_ptr;
extern IMP_Encoder_UnRegisterChn_t IMP_Encoder_UnRegisterChn_ptr;
extern IMP_Encoder_StartRecvPic_t IMP_Encoder_StartRecvPic_ptr;
extern IMP_Encoder_StopRecvPic_t IMP_Encoder_StopRecvPic_ptr;
extern IMP_Encoder_Query_t IMP_Encoder_Query_ptr;
extern IMP_Encoder_GetStream_t IMP_Encoder_GetStream_ptr;
extern IMP_Encoder_ReleaseStream_t IMP_Encoder_ReleaseStream_ptr;
extern IMP_Encoder_PollingStream_t IMP_Encoder_PollingStream_ptr;
extern IMP_Encoder_GetFd_t IMP_Encoder_GetFd_ptr;
extern IMP_Encoder_SetbufshareChn_t IMP_Encoder_SetbufshareChn_ptr;
extern IMP_Encoder_SetChnResizeMode_t IMP_Encoder_SetChnResizeMode_ptr;
extern IMP_Encoder_SetMaxStreamCnt_t IMP_Encoder_SetMaxStreamCnt_ptr;
extern IMP_Encoder_GetMaxStreamCnt_t IMP_Encoder_GetMaxStreamCnt_ptr;
extern IMP_Encoder_RequestIDR_t IMP_Encoder_RequestIDR_ptr;
extern IMP_Encoder_FlushStream_t IMP_Encoder_FlushStream_ptr;
extern IMP_Encoder_GetChnFrmRate_t IMP_Encoder_GetChnFrmRate_ptr;
extern IMP_Encoder_SetChnFrmRate_t IMP_Encoder_SetChnFrmRate_ptr;
extern IMP_Encoder_SetChnBitRate_t IMP_Encoder_SetChnBitRate_ptr;
extern IMP_Encoder_SetChnGopLength_t IMP_Encoder_SetChnGopLength_ptr;
extern IMP_Encoder_GetChnAttrRcMode_t IMP_Encoder_GetChnAttrRcMode_ptr;
extern IMP_Encoder_SetChnAttrRcMode_t IMP_Encoder_SetChnAttrRcMode_ptr;;
extern IMP_Encoder_GetChnGopAttr_t IMP_Encoder_GetChnGopAttr_ptr;
extern IMP_Encoder_SetChnGopAttr_t IMP_Encoder_SetChnGopAttr_ptr;
extern IMP_Encoder_SetChnQp_t IMP_Encoder_SetChnQp_ptr;
extern IMP_Encoder_SetChnQpBounds_t IMP_Encoder_SetChnQpBounds_ptr;
extern IMP_Encoder_SetChnQpIPDelta_t IMP_Encoder_SetChnQpIPDelta_ptr;
extern IMP_Encoder_SetFisheyeEnableStatus_t IMP_Encoder_SetFisheyeEnableStatus_ptr;
extern IMP_Encoder_GetFisheyeEnableStatus_t IMP_Encoder_GetFisheyeEnableStatus_ptr;
extern IMP_Encoder_GetChnEncType_t IMP_Encoder_GetChnEncType_ptr;
extern IMP_Encoder_SetPool_t IMP_Encoder_SetPool_ptr;
extern IMP_Encoder_GetPool_t IMP_Encoder_GetPool_ptr;
extern IMP_Encoder_SetStreamBufSize_t IMP_Encoder_SetStreamBufSize_ptr;
extern IMP_Encoder_GetStreamBufSize_t IMP_Encoder_GetStreamBufSize_ptr;
extern IMP_Encoder_GetChnAveBitrate_t IMP_Encoder_GetChnAveBitrate_ptr;
extern IMP_Encoder_GetChnEvalInfo_t IMP_Encoder_GetChnEvalInfo_ptr;
extern IMP_Encoder_SetChnEntropyMode_t IMP_Encoder_SetChnEntropyMode_ptr;
extern IMP_Encoder_SetFrameRelease_t IMP_Encoder_SetFrameRelease_ptr;
extern IMP_IVS_CreateMoveInterface_t *IMP_IVS_CreateMoveInterface_ptr;
extern IMP_IVS_DestroyMoveInterface_t IMP_IVS_DestroyMoveInterface_ptr;
/*extern imp_log_fun_t imp_log_fun_ptr;
extern IMP_Log_Set_Option_t IMP_Log_Set_Option_ptr;
extern IMP_Log_Get_Option_t IMP_Log_Get_Option_ptr;*/
extern IMP_FrameSource_CreateChn_t IMP_FrameSource_CreateChn_ptr;
extern IMP_FrameSource_DestroyChn_t IMP_FrameSource_DestroyChn_ptr;
extern IMP_FrameSource_EnableChn_t IMP_FrameSource_EnableChn_ptr;
extern IMP_FrameSource_DisableChn_t IMP_FrameSource_DisableChn_ptr;
extern IMP_FrameSource_SetSource_t IMP_FrameSource_SetSource_ptr;
extern IMP_FrameSource_GetChnAttr_t IMP_FrameSource_GetChnAttr_ptr;
extern IMP_FrameSource_SetChnAttr_t IMP_FrameSource_SetChnAttr_ptr;
extern IMP_FrameSource_SetFrameDepth_t IMP_FrameSource_SetFrameDepth_ptr;
extern IMP_FrameSource_GetFrameDepth_t IMP_FrameSource_GetFrameDepth_ptr;
extern IMP_FrameSource_GetFrame_t IMP_FrameSource_GetFrame_ptr;
extern IMP_FrameSource_GetTimedFrame_t IMP_FrameSource_GetTimedFrame_ptr;
extern IMP_FrameSource_ReleaseFrame_t IMP_FrameSource_ReleaseFrame_ptr;
extern IMP_FrameSource_SnapFrame_t IMP_FrameSource_SnapFrame_ptr;
extern IMP_FrameSource_SetMaxDelay_t IMP_FrameSource_SetMaxDelay_ptr;
extern IMP_FrameSource_GetMaxDelay_t IMP_FrameSource_GetMaxDelay_ptr;
extern IMP_FrameSource_SetDelay_t IMP_FrameSource_SetDelay_ptr;
extern IMP_FrameSource_GetDelay_t IMP_FrameSource_GetDelay_ptr;
extern IMP_FrameSource_SetChnFifoAttr_t IMP_FrameSource_SetChnFifoAttr_ptr;
extern IMP_FrameSource_GetChnFifoAttr_t IMP_FrameSource_GetChnFifoAttr_ptr;
extern IMP_FrameSource_SetPool_t IMP_FrameSource_SetPool_ptr;
extern IMP_FrameSource_GetPool_t IMP_FrameSource_GetPool_ptr;
extern IMP_FrameSource_ChnStatQuery_t IMP_FrameSource_ChnStatQuery_ptr;
extern IMP_FrameSource_SetChnRotate_t IMP_FrameSource_SetChnRotate_ptr;

#define IMP_System_GetCPUInfo (*IMP_System_GetCPUInfo_ptr)
#define IMP_IVS_CreateGroup (*IMP_IVS_CreateGroup_ptr)
#define IMP_IVS_DestroyGroup (*IMP_IVS_DestroyGroup_ptr)
#define IMP_IVS_CreateChn (*IMP_IVS_CreateChn_ptr)
#define IMP_IVS_DestroyChn (*IMP_IVS_DestroyChn_ptr)
#define IMP_IVS_RegisterChn (*IMP_IVS_RegisterChn_ptr)
#define IMP_IVS_UnRegisterChn (*IMP_IVS_UnRegisterChn_ptr)
#define IMP_IVS_StartRecvPic (*IMP_IVS_StartRecvPic_ptr)
#define IMP_IVS_StopRecvPic (*IMP_IVS_StopRecvPic_ptr)
#define IMP_IVS_PollingResult (*IMP_IVS_PollingResult_ptr)
#define IMP_IVS_GetResult (*IMP_IVS_GetResult_ptr)
#define IMP_IVS_ReleaseResult (*IMP_IVS_ReleaseResult_ptr)
#define IMP_IVS_ReleaseData (*IMP_IVS_ReleaseData_ptr)
#define IMP_IVS_GetParam (*IMP_IVS_GetParam_ptr)
#define IMP_IVS_SetParam (*IMP_IVS_SetParam_ptr)
#define IMP_DMIC_SetUserInfo (*IMP_DMIC_SetUserInfo_ptr)
#define IMP_DMIC_SetPubAttr (*IMP_DMIC_SetPubAttr_ptr)
#define IMP_DMIC_GetPubAttr (*IMP_DMIC_GetPubAttr_ptr)
#define IMP_DMIC_Enable (*IMP_DMIC_Enable_ptr)
#define IMP_DMIC_Disable (*IMP_DMIC_Disable_ptr)
#define IMP_DMIC_EnableChn (*IMP_DMIC_EnableChn_ptr)
#define IMP_DMIC_DisableChn (*IMP_DMIC_DisableChn_ptr)
#define IMP_DMIC_SetChnParam (*IMP_DMIC_SetChnParam_ptr)
#define IMP_DMIC_GetChnParam (*IMP_DMIC_GetChnParam_ptr)
#define IMP_DMIC_GetFrame (*IMP_DMIC_GetFrame_ptr)
#define IMP_DMIC_ReleaseFrame (*IMP_DMIC_ReleaseFrame_ptr)
#define IMP_DMIC_EnableAecRefFrame (*IMP_DMIC_EnableAecRefFrame_ptr)
#define IMP_DMIC_GetFrameAndRef (*IMP_DMIC_GetFrameAndRef_ptr)
#define IMP_DMIC_EnableAec (*IMP_DMIC_EnableAec_ptr)
#define IMP_DMIC_DisableAec (*IMP_DMIC_DisableAec_ptr)
#define IMP_DMIC_PollingFrame (*IMP_DMIC_PollingFrame_ptr)
#define IMP_DMIC_SetVol (*IMP_DMIC_SetVol_ptr)
#define IMP_DMIC_GetVol (*IMP_DMIC_GetVol_ptr)
#define IMP_DMIC_SetGain (*IMP_DMIC_SetGain_ptr)
#define IMP_DMIC_GetGain (*IMP_DMIC_GetGain_ptr)
#define IMP_AI_SetPubAttr (*IMP_AI_SetPubAttr_ptr)
#define IMP_AI_GetPubAttr (*IMP_AI_GetPubAttr_ptr)
#define IMP_AI_Enable (*IMP_AI_Enable_ptr)
#define IMP_AI_Disable (*IMP_AI_Disable_ptr)
#define IMP_AI_EnableChn (*IMP_AI_EnableChn_ptr)
#define IMP_AI_DisableChn (*IMP_AI_DisableChn_ptr)
#define IMP_AI_PollingFrame (*IMP_AI_PollingFrame_ptr)
#define IMP_AI_GetFrame (*IMP_AI_GetFrame_ptr)
#define IMP_AI_ReleaseFrame (*IMP_AI_ReleaseFrame_ptr)
#define IMP_AI_SetChnParam (*IMP_AI_SetChnParam_ptr)
#define IMP_AI_GetChnParam (*IMP_AI_GetChnParam_ptr)
#define IMP_AI_EnableAec (*IMP_AI_EnableAec_ptr)
#define IMP_AI_Set_WebrtcProfileIni_Path (*IMP_AI_Set_WebrtcProfileIni_Path_ptr)
#define IMP_AI_DisableAec (*IMP_AI_DisableAec_ptr)
#define IMP_AI_EnableNs (*IMP_AI_EnableNs_ptr)
#define IMP_AI_DisableNs (*IMP_AI_DisableNs_ptr)
#define IMP_AI_SetAgcMode (*IMP_AI_SetAgcMode_ptr)
#define IMP_AI_EnableAgc (*IMP_AI_EnableAgc_ptr)
#define IMP_AI_DisableAgc (*IMP_AI_DisableAgc_ptr)
#define IMP_AO_EnableAgc (*IMP_AO_EnableAgc_ptr)
#define IMP_AO_DisableAgc (*IMP_AO_DisableAgc_ptr)
#define IMP_AI_EnableHpf (*IMP_AI_EnableHpf_ptr)
#define IMP_AI_SetHpfCoFrequency (*IMP_AI_SetHpfCoFrequency_ptr)
#define IMP_AI_DisableHpf (*IMP_AI_DisableHpf_ptr)
#define IMP_AO_EnableHpf (*IMP_AO_EnableHpf_ptr)
#define IMP_AO_SetHpfCoFrequency (*IMP_AO_SetHpfCoFrequency_ptr)
#define IMP_AO_DisableHpf (*IMP_AO_DisableHpf_ptr)
#define IMP_AO_SetPubAttr (*IMP_AO_SetPubAttr_ptr)
#define IMP_AO_GetPubAttr (*IMP_AO_GetPubAttr_ptr)
#define IMP_AO_Enable (*IMP_AO_Enable_ptr)
#define IMP_AO_Disable (*IMP_AO_Disable_ptr)
#define IMP_AO_EnableChn (*IMP_AO_EnableChn_ptr)
#define IMP_AO_DisableChn (*IMP_AO_DisableChn_ptr)
#define IMP_AO_SendFrame (*IMP_AO_SendFrame_ptr)
#define IMP_AO_PauseChn (*IMP_AO_PauseChn_ptr)
#define IMP_AO_ResumeChn (*IMP_AO_ResumeChn_ptr)
#define IMP_AO_ClearChnBuf (*IMP_AO_ClearChnBuf_ptr)
#define IMP_AO_QueryChnStat (*IMP_AO_QueryChnStat_ptr)
#define IMP_AENC_CreateChn (*IMP_AENC_CreateChn_ptr)
#define IMP_AENC_DestroyChn (*IMP_AENC_DestroyChn_ptr)
#define IMP_AENC_SendFrame (*IMP_AENC_SendFrame_ptr)
#define IMP_AENC_PollingStream (*IMP_AENC_PollingStream_ptr)
#define IMP_AENC_GetStream (*IMP_AENC_GetStream_ptr)
#define IMP_AENC_ReleaseStream (*IMP_AENC_ReleaseStream_ptr)
#define IMP_AENC_RegisterEncoder (*IMP_AENC_RegisterEncoder_ptr)
#define IMP_AENC_UnRegisterEncoder (*IMP_AENC_UnRegisterEncoder_ptr)
#define IMP_ADEC_CreateChn (*IMP_ADEC_CreateChn_ptr)
#define IMP_ADEC_DestroyChn (*IMP_ADEC_DestroyChn_ptr)
#define IMP_ADEC_SendStream (*IMP_ADEC_SendStream_ptr)
#define IMP_ADEC_PollingStream (*IMP_ADEC_PollingStream_ptr)
#define IMP_ADEC_GetStream (*IMP_ADEC_GetStream_ptr)
#define IMP_ADEC_ReleaseStream (*IMP_ADEC_ReleaseStream_ptr)
#define IMP_ADEC_ClearChnBuf (*IMP_ADEC_ClearChnBuf_ptr)
#define IMP_ADEC_RegisterDecoder (*IMP_ADEC_RegisterDecoder_ptr)
#define IMP_ADEC_UnRegisterDecoder (*IMP_ADEC_UnRegisterDecoder_ptr)
#define IMP_AI_SetVol (*IMP_AI_SetVol_ptr)
#define IMP_AI_GetVol (*IMP_AI_GetVol_ptr)
#define IMP_AI_SetVolMute (*IMP_AI_SetVolMute_ptr)
#define IMP_AO_SetVol (*IMP_AO_SetVol_ptr)
#define IMP_AO_GetVol (*IMP_AO_GetVol_ptr)
#define IMP_AO_SetVolMute (*IMP_AO_SetVolMute_ptr)
#define IMP_AI_SetGain (*IMP_AI_SetGain_ptr)
#define IMP_AI_GetGain (*IMP_AI_GetGain_ptr)
#define IMP_AI_SetAlcGain (*IMP_AI_SetAlcGain_ptr)
#define IMP_AI_GetAlcGain (*IMP_AI_GetAlcGain_ptr)
#define IMP_AO_SetGain (*IMP_AO_SetGain_ptr)
#define IMP_AO_GetGain (*IMP_AO_GetGain_ptr)
#define IMP_AO_Soft_Mute (*IMP_AO_Soft_Mute_ptr)
#define IMP_AO_Soft_UNMute (*IMP_AO_Soft_UNMute_ptr)
#define IMP_AI_GetFrameAndRef (*IMP_AI_GetFrameAndRef_ptr)
#define IMP_AI_EnableAecRefFrame (*IMP_AI_EnableAecRefFrame_ptr)
#define IMP_AI_DisableAecRefFrame (*IMP_AI_DisableAecRefFrame_ptr)
#define IMP_AO_CacheSwitch (*IMP_AO_CacheSwitch_ptr)
#define IMP_AO_FlushChnBuf (*IMP_AO_FlushChnBuf_ptr)
#define IMP_IVS_CreateBaseMoveInterface (*IMP_IVS_CreateBaseMoveInterface_ptr)
#define IMP_IVS_DestroyBaseMoveInterface (*IMP_IVS_DestroyBaseMoveInterface_ptr)
#define IMPPixfmtToString (*IMPPixfmtToString_ptr)
#define IMP_OSD_CreateGroup (*IMP_OSD_CreateGroup_ptr)
#define IMP_OSD_DestroyGroup (*IMP_OSD_DestroyGroup_ptr)
#define IMP_OSD_AttachToGroup (*IMP_OSD_AttachToGroup_ptr)
#define IMP_OSD_CreateRgn (*IMP_OSD_CreateRgn_ptr)
#define IMP_OSD_DestroyRgn (*IMP_OSD_DestroyRgn_ptr)
#define IMP_OSD_RegisterRgn (*IMP_OSD_RegisterRgn_ptr)
#define IMP_OSD_UnRegisterRgn (*IMP_OSD_UnRegisterRgn_ptr)
#define IMP_OSD_SetRgnAttr (*IMP_OSD_SetRgnAttr_ptr)
#define IMP_OSD_SetRgnAttrWithTimestamp (*IMP_OSD_SetRgnAttrWithTimestamp_ptr)
#define IMP_OSD_GetRgnAttr (*IMP_OSD_GetRgnAttr_ptr)
#define IMP_OSD_UpdateRgnAttrData (*IMP_OSD_UpdateRgnAttrData_ptr)
#define IMP_OSD_SetGrpRgnAttr (*IMP_OSD_SetGrpRgnAttr_ptr)
#define IMP_OSD_GetGrpRgnAttr (*IMP_OSD_GetGrpRgnAttr_ptr)
#define IMP_OSD_ShowRgn (*IMP_OSD_ShowRgn_ptr)
#define IMP_OSD_Start (*IMP_OSD_Start_ptr)
#define IMP_OSD_Stop (*IMP_OSD_Stop_ptr)
#define IMP_ISP_Open (*IMP_ISP_Open_ptr)
#define IMP_ISP_Close (*IMP_ISP_Close_ptr)
#define IMP_ISP_SetDefaultBinPath (*IMP_ISP_SetDefaultBinPath_ptr)
#define IMP_ISP_GetDefaultBinPath (*IMP_ISP_GetDefaultBinPath_ptr)
#define IMP_ISP_AddSensor (*IMP_ISP_AddSensor_ptr)
#define IMP_ISP_DelSensor (*IMP_ISP_DelSensor_ptr)
#define IMP_ISP_EnableSensor (*IMP_ISP_EnableSensor_ptr)
#define IMP_ISP_DisableSensor (*IMP_ISP_DisableSensor_ptr)
#define IMP_ISP_SetSensorRegister (*IMP_ISP_SetSensorRegister_ptr)
#define IMP_ISP_GetSensorRegister (*IMP_ISP_GetSensorRegister_ptr)
#define IMP_ISP_Tuning_SetAutoZoom (*IMP_ISP_Tuning_SetAutoZoom_ptr)
#define IMP_ISP_EnableTuning (*IMP_ISP_EnableTuning_ptr)
#define IMP_ISP_DisableTuning (*IMP_ISP_DisableTuning_ptr)
#define IMP_ISP_Tuning_SetSensorFPS (*IMP_ISP_Tuning_SetSensorFPS_ptr)
#define IMP_ISP_Tuning_GetSensorFPS (*IMP_ISP_Tuning_GetSensorFPS_ptr)
#define IMP_ISP_Tuning_SetAntiFlickerAttr (*IMP_ISP_Tuning_SetAntiFlickerAttr_ptr)
#define IMP_ISP_Tuning_GetAntiFlickerAttr (*IMP_ISP_Tuning_GetAntiFlickerAttr_ptr)
#define IMP_ISP_Tuning_SetBrightness (*IMP_ISP_Tuning_SetBrightness_ptr)
#define IMP_ISP_Tuning_GetBrightness (*IMP_ISP_Tuning_GetBrightness_ptr)
#define IMP_ISP_Tuning_SetContrast (*IMP_ISP_Tuning_SetContrast_ptr)
#define IMP_ISP_Tuning_GetContrast (*IMP_ISP_Tuning_GetContrast_ptr)
#define IMP_ISP_Tuning_SetSharpness (*IMP_ISP_Tuning_SetSharpness_ptr)
#define IMP_ISP_Tuning_GetSharpness (*IMP_ISP_Tuning_GetSharpness_ptr)
#define IMP_ISP_Tuning_SetBcshHue (*IMP_ISP_Tuning_SetBcshHue_ptr)
#define IMP_ISP_Tuning_GetBcshHue (*IMP_ISP_Tuning_GetBcshHue_ptr)
#define IMP_ISP_Tuning_SetSaturation (*IMP_ISP_Tuning_SetSaturation_ptr)
#define IMP_ISP_Tuning_GetSaturation (*IMP_ISP_Tuning_GetSaturation_ptr)
#define IMP_ISP_Tuning_SetISPBypass (*IMP_ISP_Tuning_SetISPBypass_ptr)
#define IMP_ISP_Tuning_GetTotalGain (*IMP_ISP_Tuning_GetTotalGain_ptr)
#define IMP_ISP_Tuning_SetISPHflip (*IMP_ISP_Tuning_SetISPHflip_ptr)
#define IMP_ISP_Tuning_GetISPHflip (*IMP_ISP_Tuning_GetISPHflip_ptr)
#define IMP_ISP_Tuning_SetISPVflip (*IMP_ISP_Tuning_SetISPVflip_ptr)
#define IMP_ISP_Tuning_GetISPVflip (*IMP_ISP_Tuning_GetISPVflip_ptr)
#define IMP_ISP_Tuning_SetISPRunningMode (*IMP_ISP_Tuning_SetISPRunningMode_ptr)
#define IMP_ISP_Tuning_GetISPRunningMode (*IMP_ISP_Tuning_GetISPRunningMode_ptr)
#define IMP_ISP_Tuning_SetISPCustomMode (*IMP_ISP_Tuning_SetISPCustomMode_ptr)
#define IMP_ISP_Tuning_GetISPCustomMode (*IMP_ISP_Tuning_GetISPCustomMode_ptr)
#define IMP_ISP_Tuning_SetGamma (*IMP_ISP_Tuning_SetGamma_ptr)
#define IMP_ISP_Tuning_GetGamma (*IMP_ISP_Tuning_GetGamma_ptr)
#define IMP_ISP_Tuning_SetAeComp (*IMP_ISP_Tuning_SetAeComp_ptr)
#define IMP_ISP_Tuning_GetAeComp (*IMP_ISP_Tuning_GetAeComp_ptr)
#define IMP_ISP_Tuning_GetAeLuma (*IMP_ISP_Tuning_GetAeLuma_ptr)
#define IMP_ISP_Tuning_SetAeFreeze (*IMP_ISP_Tuning_SetAeFreeze_ptr)
#define IMP_ISP_Tuning_SetExpr (*IMP_ISP_Tuning_SetExpr_ptr)
#define IMP_ISP_Tuning_GetExpr (*IMP_ISP_Tuning_GetExpr_ptr)
#define IMP_ISP_Tuning_SetWB (*IMP_ISP_Tuning_SetWB_ptr)
#define IMP_ISP_Tuning_GetWB (*IMP_ISP_Tuning_GetWB_ptr)
#define IMP_ISP_Tuning_GetWB_Statis (*IMP_ISP_Tuning_GetWB_Statis_ptr)
#define IMP_ISP_Tuning_GetWB_GOL_Statis (*IMP_ISP_Tuning_GetWB_GOL_Statis_ptr)
#define IMP_ISP_Tuning_SetAwbClust (*IMP_ISP_Tuning_SetAwbClust_ptr)
#define IMP_ISP_Tuning_GetAwbClust (*IMP_ISP_Tuning_GetAwbClust_ptr)
#define IMP_ISP_Tuning_SetAwbCtTrend (*IMP_ISP_Tuning_SetAwbCtTrend_ptr)
#define IMP_ISP_Tuning_GetAwbCtTrend (*IMP_ISP_Tuning_GetAwbCtTrend_ptr)
#define IMP_ISP_Tuning_Awb_GetRgbCoefft (*IMP_ISP_Tuning_Awb_GetRgbCoefft_ptr)
#define IMP_ISP_Tuning_Awb_SetRgbCoefft (*IMP_ISP_Tuning_Awb_SetRgbCoefft_ptr)
#define IMP_ISP_Tuning_SetMaxAgain (*IMP_ISP_Tuning_SetMaxAgain_ptr)
#define IMP_ISP_Tuning_GetMaxAgain (*IMP_ISP_Tuning_GetMaxAgain_ptr)
#define IMP_ISP_Tuning_SetMaxDgain (*IMP_ISP_Tuning_SetMaxDgain_ptr)
#define IMP_ISP_Tuning_GetMaxDgain (*IMP_ISP_Tuning_GetMaxDgain_ptr)
#define IMP_ISP_Tuning_SetHiLightDepress (*IMP_ISP_Tuning_SetHiLightDepress_ptr)
#define IMP_ISP_Tuning_GetHiLightDepress (*IMP_ISP_Tuning_GetHiLightDepress_ptr)
#define IMP_ISP_Tuning_SetBacklightComp (*IMP_ISP_Tuning_SetBacklightComp_ptr)
#define IMP_ISP_Tuning_GetBacklightComp (*IMP_ISP_Tuning_GetBacklightComp_ptr)
#define IMP_ISP_Tuning_SetTemperStrength (*IMP_ISP_Tuning_SetTemperStrength_ptr)
#define IMP_ISP_Tuning_SetSinterStrength (*IMP_ISP_Tuning_SetSinterStrength_ptr)
#define IMP_ISP_Tuning_GetEVAttr (*IMP_ISP_Tuning_GetEVAttr_ptr)
#define IMP_ISP_Tuning_EnableMovestate (*IMP_ISP_Tuning_EnableMovestate_ptr)
#define IMP_ISP_Tuning_DisableMovestate (*IMP_ISP_Tuning_DisableMovestate_ptr)
#define IMP_ISP_Tuning_SetAeWeight (*IMP_ISP_Tuning_SetAeWeight_ptr)
#define IMP_ISP_Tuning_GetAeWeight (*IMP_ISP_Tuning_GetAeWeight_ptr)
#define IMP_ISP_Tuning_AE_GetROI (*IMP_ISP_Tuning_AE_GetROI_ptr)
#define IMP_ISP_Tuning_AE_SetROI (*IMP_ISP_Tuning_AE_SetROI_ptr)
#define IMP_ISP_Tuning_SetAwbWeight (*IMP_ISP_Tuning_SetAwbWeight_ptr)
#define IMP_ISP_Tuning_GetAwbWeight (*IMP_ISP_Tuning_GetAwbWeight_ptr)
#define IMP_ISP_Tuning_GetAwbZone (*IMP_ISP_Tuning_GetAwbZone_ptr)
#define IMP_ISP_Tuning_SetWB_ALGO (*IMP_ISP_Tuning_SetWB_ALGO_ptr)
#define IMP_ISP_Tuning_SetAeHist (*IMP_ISP_Tuning_SetAeHist_ptr)
#define IMP_ISP_Tuning_GetAeHist (*IMP_ISP_Tuning_GetAeHist_ptr)
#define IMP_ISP_Tuning_GetAeHist_Origin (*IMP_ISP_Tuning_GetAeHist_Origin_ptr)
#define IMP_ISP_Tuning_GetAwbHist (*IMP_ISP_Tuning_GetAwbHist_ptr)
#define IMP_ISP_Tuning_SetAwbHist (*IMP_ISP_Tuning_SetAwbHist_ptr)
#define IMP_ISP_Tuning_GetAFMetrices (*IMP_ISP_Tuning_GetAFMetrices_ptr)
#define IMP_ISP_Tuning_GetAfHist (*IMP_ISP_Tuning_GetAfHist_ptr)
#define IMP_ISP_Tuning_SetAfHist (*IMP_ISP_Tuning_SetAfHist_ptr)
#define IMP_ISP_Tuning_SetAfWeight (*IMP_ISP_Tuning_SetAfWeight_ptr)
#define IMP_ISP_Tuning_GetAfWeight (*IMP_ISP_Tuning_GetAfWeight_ptr)
#define IMP_ISP_Tuning_GetAfZone (*IMP_ISP_Tuning_GetAfZone_ptr)
#define IMP_ISP_Tuning_WaitFrame (*IMP_ISP_Tuning_WaitFrame_ptr)
#define IMP_ISP_Tuning_SetAeMin (*IMP_ISP_Tuning_SetAeMin_ptr)
#define IMP_ISP_Tuning_GetAeMin (*IMP_ISP_Tuning_GetAeMin_ptr)
#define IMP_ISP_Tuning_SetAe_IT_MAX (*IMP_ISP_Tuning_SetAe_IT_MAX_ptr)
#define IMP_ISP_Tuning_GetAE_IT_MAX (*IMP_ISP_Tuning_GetAE_IT_MAX_ptr)
#define IMP_ISP_Tuning_GetAeZone (*IMP_ISP_Tuning_GetAeZone_ptr)
#define IMP_ISP_Tuning_SetAeTargetList (*IMP_ISP_Tuning_SetAeTargetList_ptr)
#define IMP_ISP_Tuning_GetAeTargetList (*IMP_ISP_Tuning_GetAeTargetList_ptr)
#define IMP_ISP_Tuning_SetModuleControl (*IMP_ISP_Tuning_SetModuleControl_ptr)
#define IMP_ISP_Tuning_GetModuleControl (*IMP_ISP_Tuning_GetModuleControl_ptr)
#define IMP_ISP_Tuning_SetFrontCrop (*IMP_ISP_Tuning_SetFrontCrop_ptr)
#define IMP_ISP_Tuning_GetFrontCrop (*IMP_ISP_Tuning_GetFrontCrop_ptr)
#define IMP_ISP_WDR_ENABLE (*IMP_ISP_WDR_ENABLE_ptr)
#define IMP_ISP_WDR_ENABLE_Get (*IMP_ISP_WDR_ENABLE_Get_ptr)
#define IMP_ISP_Tuning_SetDPC_Strength (*IMP_ISP_Tuning_SetDPC_Strength_ptr)
#define IMP_ISP_Tuning_GetDPC_Strength (*IMP_ISP_Tuning_GetDPC_Strength_ptr)
#define IMP_ISP_Tuning_SetDRC_Strength (*IMP_ISP_Tuning_SetDRC_Strength_ptr)
#define IMP_ISP_Tuning_GetDRC_Strength (*IMP_ISP_Tuning_GetDRC_Strength_ptr)
#define IMP_ISP_Tuning_SetHVFLIP (*IMP_ISP_Tuning_SetHVFLIP_ptr)
#define IMP_ISP_Tuning_GetHVFlip (*IMP_ISP_Tuning_GetHVFlip_ptr)
#define IMP_ISP_Tuning_SetMask (*IMP_ISP_Tuning_SetMask_ptr)
#define IMP_ISP_Tuning_GetMask (*IMP_ISP_Tuning_GetMask_ptr)
#define IMP_ISP_Tuning_GetSensorAttr (*IMP_ISP_Tuning_GetSensorAttr_ptr)
#define IMP_ISP_Tuning_EnableDRC (*IMP_ISP_Tuning_EnableDRC_ptr)
#define IMP_ISP_Tuning_EnableDefog (*IMP_ISP_Tuning_EnableDefog_ptr)
#define IMP_ISP_Tuning_SetAwbCt (*IMP_ISP_Tuning_SetAwbCt_ptr)
#define IMP_ISP_Tuning_GetAWBCt (*IMP_ISP_Tuning_GetAWBCt_ptr)
#define IMP_ISP_Tuning_SetCCMAttr (*IMP_ISP_Tuning_SetCCMAttr_ptr)
#define IMP_ISP_Tuning_GetCCMAttr (*IMP_ISP_Tuning_GetCCMAttr_ptr)
#define IMP_ISP_Tuning_SetAeAttr (*IMP_ISP_Tuning_SetAeAttr_ptr)
#define IMP_ISP_Tuning_GetAeAttr (*IMP_ISP_Tuning_GetAeAttr_ptr)
#define IMP_ISP_Tuning_GetAeState (*IMP_ISP_Tuning_GetAeState_ptr)
#define IMP_ISP_Tuning_SetScalerLv (*IMP_ISP_Tuning_SetScalerLv_ptr)
#define IMP_ISP_SetAeAlgoFunc (*IMP_ISP_SetAeAlgoFunc_ptr)
#define IMP_ISP_SetAwbAlgoFunc (*IMP_ISP_SetAwbAlgoFunc_ptr)
#define IMP_ISP_Tuning_GetBlcAttr (*IMP_ISP_Tuning_GetBlcAttr_ptr)
#define IMP_ISP_Tuning_SetDefog_Strength (*IMP_ISP_Tuning_SetDefog_Strength_ptr)
#define IMP_ISP_Tuning_GetDefog_Strength (*IMP_ISP_Tuning_GetDefog_Strength_ptr)
#define IMP_ISP_Tuning_SetCsc_Attr (*IMP_ISP_Tuning_SetCsc_Attr_ptr)
#define IMP_ISP_Tuning_GetCsc_Attr (*IMP_ISP_Tuning_GetCsc_Attr_ptr)
#define IMP_ISP_Tuning_SetWdr_OutputMode (*IMP_ISP_Tuning_SetWdr_OutputMode_ptr)
#define IMP_ISP_Tuning_GetWdr_OutputMode (*IMP_ISP_Tuning_GetWdr_OutputMode_ptr)
#define IMP_ISP_SetFrameDrop (*IMP_ISP_SetFrameDrop_ptr)
#define IMP_ISP_GetFrameDrop (*IMP_ISP_GetFrameDrop_ptr)
#define IMP_ISP_SetFixedContraster (*IMP_ISP_SetFixedContraster_ptr)
#define IMP_ISP_SET_GPIO_INIT_OR_FREE (*IMP_ISP_SET_GPIO_INIT_OR_FREE_ptr)
#define IMP_ISP_SET_GPIO_STA (*IMP_ISP_SET_GPIO_STA_ptr)
#define IMP_Decoder_CreateChn (*IMP_Decoder_CreateChn_ptr)
#define IMP_Decoder_DestroyChn (*IMP_Decoder_DestroyChn_ptr)
#define IMP_Decoder_StartRecvPic (*IMP_Decoder_StartRecvPic_ptr)
#define IMP_Decoder_StopRecvPic (*IMP_Decoder_StopRecvPic_ptr)
#define IMP_Decoder_SendStreamTimeout (*IMP_Decoder_SendStreamTimeout_ptr)
#define IMP_Decoder_PollingFrame (*IMP_Decoder_PollingFrame_ptr)
#define IMP_Decoder_GetFrame (*IMP_Decoder_GetFrame_ptr)
#define IMP_Decoder_ReleaseFrame (*IMP_Decoder_ReleaseFrame_ptr)
#define IMP_System_Init (*IMP_System_Init_ptr)
#define IMP_System_Exit (*IMP_System_Exit_ptr)
#define IMP_System_GetTimeStamp (*IMP_System_GetTimeStamp_ptr)
#define IMP_System_RebaseTimeStamp (*IMP_System_RebaseTimeStamp_ptr)
#define IMP_System_ReadReg32 (*IMP_System_ReadReg32_ptr)
#define IMP_System_WriteReg32 (*IMP_System_WriteReg32_ptr)
#define IMP_System_GetVersion (*IMP_System_GetVersion_ptr)
#define IMP_System_Bind (*IMP_System_Bind_ptr)
#define IMP_System_UnBind (*IMP_System_UnBind_ptr)
#define IMP_System_GetBindbyDest (*IMP_System_GetBindbyDest_ptr)
#define IMP_System_MemPoolRequest (*IMP_System_MemPoolRequest_ptr)
#define IMP_System_MemPoolFree (*IMP_System_MemPoolFree_ptr)
#define IMP_Encoder_CreateGroup (*IMP_Encoder_CreateGroup_ptr)
#define IMP_Encoder_DestroyGroup (*IMP_Encoder_DestroyGroup_ptr)
#define IMP_Encoder_SetDefaultParam (*IMP_Encoder_SetDefaultParam_ptr)
#define IMP_Encoder_CreateChn (*IMP_Encoder_CreateChn_ptr)
#define IMP_Encoder_DestroyChn (*IMP_Encoder_DestroyChn_ptr)
#define IMP_Encoder_GetChnAttr (*IMP_Encoder_GetChnAttr_ptr)
#define IMP_Encoder_RegisterChn (*IMP_Encoder_RegisterChn_ptr)
#define IMP_Encoder_UnRegisterChn (*IMP_Encoder_UnRegisterChn_ptr)
#define IMP_Encoder_StartRecvPic (*IMP_Encoder_StartRecvPic_ptr)
#define IMP_Encoder_StopRecvPic (*IMP_Encoder_StopRecvPic_ptr)
#define IMP_Encoder_Query (*IMP_Encoder_Query_ptr)
#define IMP_Encoder_GetStream (*IMP_Encoder_GetStream_ptr)
#define IMP_Encoder_ReleaseStream (*IMP_Encoder_ReleaseStream_ptr)
#define IMP_Encoder_PollingStream (*IMP_Encoder_PollingStream_ptr)
#define IMP_Encoder_GetFd (*IMP_Encoder_GetFd_ptr)
#define IMP_Encoder_SetbufshareChn (*IMP_Encoder_SetbufshareChn_ptr)
#define IMP_Encoder_SetChnResizeMode (*IMP_Encoder_SetChnResizeMode_ptr)
#define IMP_Encoder_SetMaxStreamCnt (*IMP_Encoder_SetMaxStreamCnt_ptr)
#define IMP_Encoder_GetMaxStreamCnt (*IMP_Encoder_GetMaxStreamCnt_ptr)
#define IMP_Encoder_RequestIDR (*IMP_Encoder_RequestIDR_ptr)
#define IMP_Encoder_FlushStream (*IMP_Encoder_FlushStream_ptr)
#define IMP_Encoder_GetChnFrmRate (*IMP_Encoder_GetChnFrmRate_ptr)
#define IMP_Encoder_SetChnFrmRate (*IMP_Encoder_SetChnFrmRate_ptr)
#define IMP_Encoder_SetChnBitRate (*IMP_Encoder_SetChnBitRate_ptr)
#define IMP_Encoder_SetChnGopLength (*IMP_Encoder_SetChnGopLength_ptr)
#define IMP_Encoder_GetChnAttrRcMode (*IMP_Encoder_GetChnAttrRcMode_ptr)
#define IMP_Encoder_SetChnAttrRcMode (*IMP_Encoder_SetChnAttrRcMode_ptr)
#define IMP_Encoder_GetChnGopAttr (*IMP_Encoder_GetChnGopAttr_ptr)
#define IMP_Encoder_SetChnGopAttr (*IMP_Encoder_SetChnGopAttr_ptr)
#define IMP_Encoder_SetChnQp (*IMP_Encoder_SetChnQp_ptr)
#define IMP_Encoder_SetChnQpBounds (*IMP_Encoder_SetChnQpBounds_ptr)
#define IMP_Encoder_SetChnQpIPDelta (*IMP_Encoder_SetChnQpIPDelta_ptr)
#define IMP_Encoder_SetFisheyeEnableStatus (*IMP_Encoder_SetFisheyeEnableStatus_ptr)
#define IMP_Encoder_GetFisheyeEnableStatus (*IMP_Encoder_GetFisheyeEnableStatus_ptr)
#define IMP_Encoder_GetChnEncType (*IMP_Encoder_GetChnEncType_ptr)
#define IMP_Encoder_SetPool (*IMP_Encoder_SetPool_ptr)
#define IMP_Encoder_GetPool (*IMP_Encoder_GetPool_ptr)
#define IMP_Encoder_SetStreamBufSize (*IMP_Encoder_SetStreamBufSize_ptr)
#define IMP_Encoder_GetStreamBufSize (*IMP_Encoder_GetStreamBufSize_ptr)
#define IMP_Encoder_GetChnAveBitrate (*IMP_Encoder_GetChnAveBitrate_ptr)
#define IMP_Encoder_GetChnEvalInfo (*IMP_Encoder_GetChnEvalInfo_ptr)
#define IMP_Encoder_SetChnEntropyMode (*IMP_Encoder_SetChnEntropyMode_ptr)
#define IMP_Encoder_SetFrameRelease (*IMP_Encoder_SetFrameRelease_ptr)
#define IMP_IVS_CreateMoveInterface (*IMP_IVS_CreateMoveInterface_ptr)
#define IMP_IVS_DestroyMoveInterface (*IMP_IVS_DestroyMoveInterface_ptr)
/*#define imp_log_fun (*imp_log_fun_ptr)
#define IMP_Log_Set_Option (*IMP_Log_Set_Option_ptr)
#define IMP_Log_Get_Option (*IMP_Log_Get_Option_ptr)*/
#define IMP_FrameSource_CreateChn (*IMP_FrameSource_CreateChn_ptr)
#define IMP_FrameSource_DestroyChn (*IMP_FrameSource_DestroyChn_ptr)
#define IMP_FrameSource_EnableChn (*IMP_FrameSource_EnableChn_ptr)
#define IMP_FrameSource_DisableChn (*IMP_FrameSource_DisableChn_ptr)
#define IMP_FrameSource_SetSource (*IMP_FrameSource_SetSource_ptr)
#define IMP_FrameSource_GetChnAttr (*IMP_FrameSource_GetChnAttr_ptr)
#define IMP_FrameSource_SetChnAttr (*IMP_FrameSource_SetChnAttr_ptr)
#define IMP_FrameSource_SetFrameDepth (*IMP_FrameSource_SetFrameDepth_ptr)
#define IMP_FrameSource_GetFrameDepth (*IMP_FrameSource_GetFrameDepth_ptr)
#define IMP_FrameSource_GetFrame (*IMP_FrameSource_GetFrame_ptr)
#define IMP_FrameSource_GetTimedFrame (*IMP_FrameSource_GetTimedFrame_ptr)
#define IMP_FrameSource_ReleaseFrame (*IMP_FrameSource_ReleaseFrame_ptr)
#define IMP_FrameSource_SnapFrame (*IMP_FrameSource_SnapFrame_ptr)
#define IMP_FrameSource_SetMaxDelay (*IMP_FrameSource_SetMaxDelay_ptr)
#define IMP_FrameSource_GetMaxDelay (*IMP_FrameSource_GetMaxDelay_ptr)
#define IMP_FrameSource_SetDelay (*IMP_FrameSource_SetDelay_ptr)
#define IMP_FrameSource_GetDelay (*IMP_FrameSource_GetDelay_ptr)
#define IMP_FrameSource_SetChnFifoAttr (*IMP_FrameSource_SetChnFifoAttr_ptr)
#define IMP_FrameSource_GetChnFifoAttr (*IMP_FrameSource_GetChnFifoAttr_ptr)
#define IMP_FrameSource_SetPool (*IMP_FrameSource_SetPool_ptr)
#define IMP_FrameSource_GetPool (*IMP_FrameSource_GetPool_ptr)
#define IMP_FrameSource_ChnStatQuery (*IMP_FrameSource_ChnStatQuery_ptr)
#define IMP_FrameSource_SetChnRotate (*IMP_FrameSource_SetChnRotate_ptr)

