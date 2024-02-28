#include <stdio.h>
#include <dlfcn.h>
#include <stdint.h>

#include "libimp_dynamic_t31.h"

IMP_System_GetCPUInfo_t IMP_System_GetCPUInfo_ptr;
IMP_IVS_CreateGroup_t IMP_IVS_CreateGroup_ptr;
IMP_IVS_DestroyGroup_t IMP_IVS_DestroyGroup_ptr;
IMP_IVS_CreateChn_t IMP_IVS_CreateChn_ptr;
IMP_IVS_DestroyChn_t IMP_IVS_DestroyChn_ptr;
IMP_IVS_RegisterChn_t IMP_IVS_RegisterChn_ptr;
IMP_IVS_UnRegisterChn_t IMP_IVS_UnRegisterChn_ptr;
IMP_IVS_StartRecvPic_t IMP_IVS_StartRecvPic_ptr;
IMP_IVS_StopRecvPic_t IMP_IVS_StopRecvPic_ptr;
IMP_IVS_PollingResult_t IMP_IVS_PollingResult_ptr;
IMP_IVS_GetResult_t IMP_IVS_GetResult_ptr;
IMP_IVS_ReleaseResult_t IMP_IVS_ReleaseResult_ptr;
IMP_IVS_ReleaseData_t IMP_IVS_ReleaseData_ptr;
IMP_IVS_GetParam_t IMP_IVS_GetParam_ptr;
IMP_IVS_SetParam_t IMP_IVS_SetParam_ptr;
IMP_DMIC_SetUserInfo_t IMP_DMIC_SetUserInfo_ptr;
IMP_DMIC_SetPubAttr_t IMP_DMIC_SetPubAttr_ptr;
IMP_DMIC_GetPubAttr_t IMP_DMIC_GetPubAttr_ptr;
IMP_DMIC_Enable_t IMP_DMIC_Enable_ptr;
IMP_DMIC_Disable_t IMP_DMIC_Disable_ptr;
IMP_DMIC_EnableChn_t IMP_DMIC_EnableChn_ptr;
IMP_DMIC_DisableChn_t IMP_DMIC_DisableChn_ptr;
IMP_DMIC_SetChnParam_t IMP_DMIC_SetChnParam_ptr;
IMP_DMIC_GetChnParam_t IMP_DMIC_GetChnParam_ptr;
IMP_DMIC_GetFrame_t IMP_DMIC_GetFrame_ptr;
IMP_DMIC_ReleaseFrame_t IMP_DMIC_ReleaseFrame_ptr;
IMP_DMIC_EnableAecRefFrame_t IMP_DMIC_EnableAecRefFrame_ptr;
IMP_DMIC_GetFrameAndRef_t IMP_DMIC_GetFrameAndRef_ptr;
IMP_DMIC_EnableAec_t IMP_DMIC_EnableAec_ptr;
IMP_DMIC_DisableAec_t IMP_DMIC_DisableAec_ptr;
IMP_DMIC_PollingFrame_t IMP_DMIC_PollingFrame_ptr;
IMP_DMIC_SetVol_t IMP_DMIC_SetVol_ptr;
IMP_DMIC_GetVol_t IMP_DMIC_GetVol_ptr;
IMP_DMIC_SetGain_t IMP_DMIC_SetGain_ptr;
IMP_DMIC_GetGain_t IMP_DMIC_GetGain_ptr;
IMP_AI_SetPubAttr_t IMP_AI_SetPubAttr_ptr;
IMP_AI_GetPubAttr_t IMP_AI_GetPubAttr_ptr;
IMP_AI_Enable_t IMP_AI_Enable_ptr;
IMP_AI_Disable_t IMP_AI_Disable_ptr;
IMP_AI_EnableChn_t IMP_AI_EnableChn_ptr;
IMP_AI_DisableChn_t IMP_AI_DisableChn_ptr;
IMP_AI_PollingFrame_t IMP_AI_PollingFrame_ptr;
IMP_AI_GetFrame_t IMP_AI_GetFrame_ptr;
IMP_AI_ReleaseFrame_t IMP_AI_ReleaseFrame_ptr;
IMP_AI_SetChnParam_t IMP_AI_SetChnParam_ptr;
IMP_AI_GetChnParam_t IMP_AI_GetChnParam_ptr;
IMP_AI_EnableAec_t IMP_AI_EnableAec_ptr;
IMP_AI_Set_WebrtcProfileIni_Path_t IMP_AI_Set_WebrtcProfileIni_Path_ptr;
IMP_AI_DisableAec_t IMP_AI_DisableAec_ptr;
IMP_AI_EnableNs_t IMP_AI_EnableNs_ptr;
IMP_AI_DisableNs_t IMP_AI_DisableNs_ptr;
IMP_AI_SetAgcMode_t IMP_AI_SetAgcMode_ptr;
IMP_AI_EnableAgc_t IMP_AI_EnableAgc_ptr;
IMP_AI_DisableAgc_t IMP_AI_DisableAgc_ptr;
IMP_AO_EnableAgc_t IMP_AO_EnableAgc_ptr;
IMP_AO_DisableAgc_t IMP_AO_DisableAgc_ptr;
IMP_AI_EnableHpf_t IMP_AI_EnableHpf_ptr;
IMP_AI_SetHpfCoFrequency_t IMP_AI_SetHpfCoFrequency_ptr;
IMP_AI_DisableHpf_t IMP_AI_DisableHpf_ptr;
IMP_AO_EnableHpf_t IMP_AO_EnableHpf_ptr;
IMP_AO_SetHpfCoFrequency_t IMP_AO_SetHpfCoFrequency_ptr;
IMP_AO_DisableHpf_t IMP_AO_DisableHpf_ptr;
IMP_AO_SetPubAttr_t IMP_AO_SetPubAttr_ptr;
IMP_AO_GetPubAttr_t IMP_AO_GetPubAttr_ptr;
IMP_AO_Enable_t IMP_AO_Enable_ptr;
IMP_AO_Disable_t IMP_AO_Disable_ptr;
IMP_AO_EnableChn_t IMP_AO_EnableChn_ptr;
IMP_AO_DisableChn_t IMP_AO_DisableChn_ptr;
IMP_AO_SendFrame_t IMP_AO_SendFrame_ptr;
IMP_AO_PauseChn_t IMP_AO_PauseChn_ptr;
IMP_AO_ResumeChn_t IMP_AO_ResumeChn_ptr;
IMP_AO_ClearChnBuf_t IMP_AO_ClearChnBuf_ptr;
IMP_AO_QueryChnStat_t IMP_AO_QueryChnStat_ptr;
IMP_AENC_CreateChn_t IMP_AENC_CreateChn_ptr;
IMP_AENC_DestroyChn_t IMP_AENC_DestroyChn_ptr;
IMP_AENC_SendFrame_t IMP_AENC_SendFrame_ptr;
IMP_AENC_PollingStream_t IMP_AENC_PollingStream_ptr;
IMP_AENC_GetStream_t IMP_AENC_GetStream_ptr;
IMP_AENC_ReleaseStream_t IMP_AENC_ReleaseStream_ptr;
IMP_AENC_RegisterEncoder_t IMP_AENC_RegisterEncoder_ptr;
IMP_AENC_UnRegisterEncoder_t IMP_AENC_UnRegisterEncoder_ptr;
IMP_ADEC_CreateChn_t IMP_ADEC_CreateChn_ptr;
IMP_ADEC_DestroyChn_t IMP_ADEC_DestroyChn_ptr;
IMP_ADEC_SendStream_t IMP_ADEC_SendStream_ptr;
IMP_ADEC_PollingStream_t IMP_ADEC_PollingStream_ptr;
IMP_ADEC_GetStream_t IMP_ADEC_GetStream_ptr;
IMP_ADEC_ReleaseStream_t IMP_ADEC_ReleaseStream_ptr;
IMP_ADEC_ClearChnBuf_t IMP_ADEC_ClearChnBuf_ptr;
IMP_ADEC_RegisterDecoder_t IMP_ADEC_RegisterDecoder_ptr;
IMP_ADEC_UnRegisterDecoder_t IMP_ADEC_UnRegisterDecoder_ptr;
IMP_AI_SetVol_t IMP_AI_SetVol_ptr;
IMP_AI_GetVol_t IMP_AI_GetVol_ptr;
IMP_AI_SetVolMute_t IMP_AI_SetVolMute_ptr;
IMP_AO_SetVol_t IMP_AO_SetVol_ptr;
IMP_AO_GetVol_t IMP_AO_GetVol_ptr;
IMP_AO_SetVolMute_t IMP_AO_SetVolMute_ptr;
IMP_AI_SetGain_t IMP_AI_SetGain_ptr;
IMP_AI_GetGain_t IMP_AI_GetGain_ptr;
IMP_AI_SetAlcGain_t IMP_AI_SetAlcGain_ptr;
IMP_AI_GetAlcGain_t IMP_AI_GetAlcGain_ptr;
IMP_AO_SetGain_t IMP_AO_SetGain_ptr;
IMP_AO_GetGain_t IMP_AO_GetGain_ptr;
IMP_AO_Soft_Mute_t IMP_AO_Soft_Mute_ptr;
IMP_AO_Soft_UNMute_t IMP_AO_Soft_UNMute_ptr;
IMP_AI_GetFrameAndRef_t IMP_AI_GetFrameAndRef_ptr;
IMP_AI_EnableAecRefFrame_t IMP_AI_EnableAecRefFrame_ptr;
IMP_AI_DisableAecRefFrame_t IMP_AI_DisableAecRefFrame_ptr;
IMP_AO_CacheSwitch_t IMP_AO_CacheSwitch_ptr;
IMP_AO_FlushChnBuf_t IMP_AO_FlushChnBuf_ptr;
IMP_IVS_CreateBaseMoveInterface_t *IMP_IVS_CreateBaseMoveInterface_ptr;
IMP_IVS_DestroyBaseMoveInterface_t IMP_IVS_DestroyBaseMoveInterface_ptr;
IMPPixfmtToString_t *IMPPixfmtToString_ptr;
IMP_OSD_CreateGroup_t IMP_OSD_CreateGroup_ptr;
IMP_OSD_DestroyGroup_t IMP_OSD_DestroyGroup_ptr;
IMP_OSD_AttachToGroup_t IMP_OSD_AttachToGroup_ptr;
IMP_OSD_CreateRgn_t IMP_OSD_CreateRgn_ptr;
IMP_OSD_DestroyRgn_t IMP_OSD_DestroyRgn_ptr;
IMP_OSD_RegisterRgn_t IMP_OSD_RegisterRgn_ptr;
IMP_OSD_UnRegisterRgn_t IMP_OSD_UnRegisterRgn_ptr;
IMP_OSD_SetRgnAttr_t IMP_OSD_SetRgnAttr_ptr;
IMP_OSD_SetRgnAttrWithTimestamp_t IMP_OSD_SetRgnAttrWithTimestamp_ptr;
IMP_OSD_GetRgnAttr_t IMP_OSD_GetRgnAttr_ptr;
IMP_OSD_UpdateRgnAttrData_t IMP_OSD_UpdateRgnAttrData_ptr;
IMP_OSD_SetGrpRgnAttr_t IMP_OSD_SetGrpRgnAttr_ptr;
IMP_OSD_GetGrpRgnAttr_t IMP_OSD_GetGrpRgnAttr_ptr;
IMP_OSD_ShowRgn_t IMP_OSD_ShowRgn_ptr;
IMP_OSD_Start_t IMP_OSD_Start_ptr;
IMP_OSD_Stop_t IMP_OSD_Stop_ptr;
IMP_ISP_Open_t IMP_ISP_Open_ptr;
IMP_ISP_Close_t IMP_ISP_Close_ptr;
IMP_ISP_SetDefaultBinPath_t IMP_ISP_SetDefaultBinPath_ptr;
IMP_ISP_GetDefaultBinPath_t IMP_ISP_GetDefaultBinPath_ptr;
IMP_ISP_AddSensor_t IMP_ISP_AddSensor_ptr;
IMP_ISP_DelSensor_t IMP_ISP_DelSensor_ptr;
IMP_ISP_EnableSensor_t IMP_ISP_EnableSensor_ptr;
IMP_ISP_DisableSensor_t IMP_ISP_DisableSensor_ptr;
IMP_ISP_SetSensorRegister_t IMP_ISP_SetSensorRegister_ptr;
IMP_ISP_GetSensorRegister_t IMP_ISP_GetSensorRegister_ptr;
IMP_ISP_Tuning_SetAutoZoom_t IMP_ISP_Tuning_SetAutoZoom_ptr;
IMP_ISP_EnableTuning_t IMP_ISP_EnableTuning_ptr;
IMP_ISP_DisableTuning_t IMP_ISP_DisableTuning_ptr;
IMP_ISP_Tuning_SetSensorFPS_t IMP_ISP_Tuning_SetSensorFPS_ptr;
IMP_ISP_Tuning_GetSensorFPS_t IMP_ISP_Tuning_GetSensorFPS_ptr;
IMP_ISP_Tuning_SetAntiFlickerAttr_t IMP_ISP_Tuning_SetAntiFlickerAttr_ptr;
IMP_ISP_Tuning_GetAntiFlickerAttr_t IMP_ISP_Tuning_GetAntiFlickerAttr_ptr;
IMP_ISP_Tuning_SetBrightness_t IMP_ISP_Tuning_SetBrightness_ptr;
IMP_ISP_Tuning_GetBrightness_t IMP_ISP_Tuning_GetBrightness_ptr;
IMP_ISP_Tuning_SetContrast_t IMP_ISP_Tuning_SetContrast_ptr;
IMP_ISP_Tuning_GetContrast_t IMP_ISP_Tuning_GetContrast_ptr;
IMP_ISP_Tuning_SetSharpness_t IMP_ISP_Tuning_SetSharpness_ptr;
IMP_ISP_Tuning_GetSharpness_t IMP_ISP_Tuning_GetSharpness_ptr;
IMP_ISP_Tuning_SetBcshHue_t IMP_ISP_Tuning_SetBcshHue_ptr;
IMP_ISP_Tuning_GetBcshHue_t IMP_ISP_Tuning_GetBcshHue_ptr;
IMP_ISP_Tuning_SetSaturation_t IMP_ISP_Tuning_SetSaturation_ptr;
IMP_ISP_Tuning_GetSaturation_t IMP_ISP_Tuning_GetSaturation_ptr;
IMP_ISP_Tuning_SetISPBypass_t IMP_ISP_Tuning_SetISPBypass_ptr;
IMP_ISP_Tuning_GetTotalGain_t IMP_ISP_Tuning_GetTotalGain_ptr;
IMP_ISP_Tuning_SetISPHflip_t IMP_ISP_Tuning_SetISPHflip_ptr;
IMP_ISP_Tuning_GetISPHflip_t IMP_ISP_Tuning_GetISPHflip_ptr;
IMP_ISP_Tuning_SetISPVflip_t IMP_ISP_Tuning_SetISPVflip_ptr;
IMP_ISP_Tuning_GetISPVflip_t IMP_ISP_Tuning_GetISPVflip_ptr;
IMP_ISP_Tuning_SetISPRunningMode_t IMP_ISP_Tuning_SetISPRunningMode_ptr;
IMP_ISP_Tuning_GetISPRunningMode_t IMP_ISP_Tuning_GetISPRunningMode_ptr;
IMP_ISP_Tuning_SetISPCustomMode_t IMP_ISP_Tuning_SetISPCustomMode_ptr;
IMP_ISP_Tuning_GetISPCustomMode_t IMP_ISP_Tuning_GetISPCustomMode_ptr;
IMP_ISP_Tuning_SetGamma_t IMP_ISP_Tuning_SetGamma_ptr;
IMP_ISP_Tuning_GetGamma_t IMP_ISP_Tuning_GetGamma_ptr;
IMP_ISP_Tuning_SetAeComp_t IMP_ISP_Tuning_SetAeComp_ptr;
IMP_ISP_Tuning_GetAeComp_t IMP_ISP_Tuning_GetAeComp_ptr;
IMP_ISP_Tuning_GetAeLuma_t IMP_ISP_Tuning_GetAeLuma_ptr;
IMP_ISP_Tuning_SetAeFreeze_t IMP_ISP_Tuning_SetAeFreeze_ptr;
IMP_ISP_Tuning_SetExpr_t IMP_ISP_Tuning_SetExpr_ptr;
IMP_ISP_Tuning_GetExpr_t IMP_ISP_Tuning_GetExpr_ptr;
IMP_ISP_Tuning_SetWB_t IMP_ISP_Tuning_SetWB_ptr;
IMP_ISP_Tuning_GetWB_t IMP_ISP_Tuning_GetWB_ptr;
IMP_ISP_Tuning_GetWB_Statis_t IMP_ISP_Tuning_GetWB_Statis_ptr;
IMP_ISP_Tuning_GetWB_GOL_Statis_t IMP_ISP_Tuning_GetWB_GOL_Statis_ptr;
IMP_ISP_Tuning_SetAwbClust_t IMP_ISP_Tuning_SetAwbClust_ptr;
IMP_ISP_Tuning_GetAwbClust_t IMP_ISP_Tuning_GetAwbClust_ptr;
IMP_ISP_Tuning_SetAwbCtTrend_t IMP_ISP_Tuning_SetAwbCtTrend_ptr;
IMP_ISP_Tuning_GetAwbCtTrend_t IMP_ISP_Tuning_GetAwbCtTrend_ptr;
IMP_ISP_Tuning_Awb_GetRgbCoefft_t IMP_ISP_Tuning_Awb_GetRgbCoefft_ptr;
IMP_ISP_Tuning_Awb_SetRgbCoefft_t IMP_ISP_Tuning_Awb_SetRgbCoefft_ptr;
IMP_ISP_Tuning_SetMaxAgain_t IMP_ISP_Tuning_SetMaxAgain_ptr;
IMP_ISP_Tuning_GetMaxAgain_t IMP_ISP_Tuning_GetMaxAgain_ptr;
IMP_ISP_Tuning_SetMaxDgain_t IMP_ISP_Tuning_SetMaxDgain_ptr;
IMP_ISP_Tuning_GetMaxDgain_t IMP_ISP_Tuning_GetMaxDgain_ptr;
IMP_ISP_Tuning_SetHiLightDepress_t IMP_ISP_Tuning_SetHiLightDepress_ptr;
IMP_ISP_Tuning_GetHiLightDepress_t IMP_ISP_Tuning_GetHiLightDepress_ptr;
IMP_ISP_Tuning_SetBacklightComp_t IMP_ISP_Tuning_SetBacklightComp_ptr;
IMP_ISP_Tuning_GetBacklightComp_t IMP_ISP_Tuning_GetBacklightComp_ptr;
IMP_ISP_Tuning_SetTemperStrength_t IMP_ISP_Tuning_SetTemperStrength_ptr;
IMP_ISP_Tuning_SetSinterStrength_t IMP_ISP_Tuning_SetSinterStrength_ptr;
IMP_ISP_Tuning_GetEVAttr_t IMP_ISP_Tuning_GetEVAttr_ptr;
IMP_ISP_Tuning_EnableMovestate_t IMP_ISP_Tuning_EnableMovestate_ptr;
IMP_ISP_Tuning_DisableMovestate_t IMP_ISP_Tuning_DisableMovestate_ptr;
IMP_ISP_Tuning_SetAeWeight_t IMP_ISP_Tuning_SetAeWeight_ptr;
IMP_ISP_Tuning_GetAeWeight_t IMP_ISP_Tuning_GetAeWeight_ptr;
IMP_ISP_Tuning_AE_GetROI_t IMP_ISP_Tuning_AE_GetROI_ptr;
IMP_ISP_Tuning_AE_SetROI_t IMP_ISP_Tuning_AE_SetROI_ptr;
IMP_ISP_Tuning_SetAwbWeight_t IMP_ISP_Tuning_SetAwbWeight_ptr;
IMP_ISP_Tuning_GetAwbWeight_t IMP_ISP_Tuning_GetAwbWeight_ptr;
IMP_ISP_Tuning_GetAwbZone_t IMP_ISP_Tuning_GetAwbZone_ptr;
IMP_ISP_Tuning_SetWB_ALGO_t IMP_ISP_Tuning_SetWB_ALGO_ptr;
IMP_ISP_Tuning_SetAeHist_t IMP_ISP_Tuning_SetAeHist_ptr;
IMP_ISP_Tuning_GetAeHist_t IMP_ISP_Tuning_GetAeHist_ptr;
IMP_ISP_Tuning_GetAeHist_Origin_t IMP_ISP_Tuning_GetAeHist_Origin_ptr;
IMP_ISP_Tuning_GetAwbHist_t IMP_ISP_Tuning_GetAwbHist_ptr;
IMP_ISP_Tuning_SetAwbHist_t IMP_ISP_Tuning_SetAwbHist_ptr;
IMP_ISP_Tuning_GetAFMetrices_t IMP_ISP_Tuning_GetAFMetrices_ptr;
IMP_ISP_Tuning_GetAfHist_t IMP_ISP_Tuning_GetAfHist_ptr;
IMP_ISP_Tuning_SetAfHist_t IMP_ISP_Tuning_SetAfHist_ptr;
IMP_ISP_Tuning_SetAfWeight_t IMP_ISP_Tuning_SetAfWeight_ptr;
IMP_ISP_Tuning_GetAfWeight_t IMP_ISP_Tuning_GetAfWeight_ptr;
IMP_ISP_Tuning_GetAfZone_t IMP_ISP_Tuning_GetAfZone_ptr;
IMP_ISP_Tuning_WaitFrame_t IMP_ISP_Tuning_WaitFrame_ptr;
IMP_ISP_Tuning_SetAeMin_t IMP_ISP_Tuning_SetAeMin_ptr;
IMP_ISP_Tuning_GetAeMin_t IMP_ISP_Tuning_GetAeMin_ptr;
IMP_ISP_Tuning_SetAe_IT_MAX_t IMP_ISP_Tuning_SetAe_IT_MAX_ptr;
IMP_ISP_Tuning_GetAE_IT_MAX_t IMP_ISP_Tuning_GetAE_IT_MAX_ptr;
IMP_ISP_Tuning_GetAeZone_t IMP_ISP_Tuning_GetAeZone_ptr;
IMP_ISP_Tuning_SetAeTargetList_t IMP_ISP_Tuning_SetAeTargetList_ptr;
IMP_ISP_Tuning_GetAeTargetList_t IMP_ISP_Tuning_GetAeTargetList_ptr;
IMP_ISP_Tuning_SetModuleControl_t IMP_ISP_Tuning_SetModuleControl_ptr;
IMP_ISP_Tuning_GetModuleControl_t IMP_ISP_Tuning_GetModuleControl_ptr;
IMP_ISP_Tuning_SetFrontCrop_t IMP_ISP_Tuning_SetFrontCrop_ptr;
IMP_ISP_Tuning_GetFrontCrop_t IMP_ISP_Tuning_GetFrontCrop_ptr;
IMP_ISP_WDR_ENABLE_t IMP_ISP_WDR_ENABLE_ptr;
IMP_ISP_WDR_ENABLE_Get_t IMP_ISP_WDR_ENABLE_Get_ptr;
IMP_ISP_Tuning_SetDPC_Strength_t IMP_ISP_Tuning_SetDPC_Strength_ptr;
IMP_ISP_Tuning_GetDPC_Strength_t IMP_ISP_Tuning_GetDPC_Strength_ptr;
IMP_ISP_Tuning_SetDRC_Strength_t IMP_ISP_Tuning_SetDRC_Strength_ptr;
IMP_ISP_Tuning_GetDRC_Strength_t IMP_ISP_Tuning_GetDRC_Strength_ptr;
IMP_ISP_Tuning_SetHVFLIP_t IMP_ISP_Tuning_SetHVFLIP_ptr;
IMP_ISP_Tuning_GetHVFlip_t IMP_ISP_Tuning_GetHVFlip_ptr;
IMP_ISP_Tuning_SetMask_t IMP_ISP_Tuning_SetMask_ptr;
IMP_ISP_Tuning_GetMask_t IMP_ISP_Tuning_GetMask_ptr;
IMP_ISP_Tuning_GetSensorAttr_t IMP_ISP_Tuning_GetSensorAttr_ptr;
IMP_ISP_Tuning_EnableDRC_t IMP_ISP_Tuning_EnableDRC_ptr;
IMP_ISP_Tuning_EnableDefog_t IMP_ISP_Tuning_EnableDefog_ptr;
IMP_ISP_Tuning_SetAwbCt_t IMP_ISP_Tuning_SetAwbCt_ptr;
IMP_ISP_Tuning_GetAWBCt_t IMP_ISP_Tuning_GetAWBCt_ptr;
IMP_ISP_Tuning_SetCCMAttr_t IMP_ISP_Tuning_SetCCMAttr_ptr;
IMP_ISP_Tuning_GetCCMAttr_t IMP_ISP_Tuning_GetCCMAttr_ptr;
IMP_ISP_Tuning_SetAeAttr_t IMP_ISP_Tuning_SetAeAttr_ptr;
IMP_ISP_Tuning_GetAeAttr_t IMP_ISP_Tuning_GetAeAttr_ptr;
IMP_ISP_Tuning_GetAeState_t IMP_ISP_Tuning_GetAeState_ptr;
IMP_ISP_Tuning_SetScalerLv_t IMP_ISP_Tuning_SetScalerLv_ptr;
IMP_ISP_SetAeAlgoFunc_t IMP_ISP_SetAeAlgoFunc_ptr;
IMP_ISP_SetAwbAlgoFunc_t IMP_ISP_SetAwbAlgoFunc_ptr;
IMP_ISP_Tuning_GetBlcAttr_t IMP_ISP_Tuning_GetBlcAttr_ptr;
IMP_ISP_Tuning_SetDefog_Strength_t IMP_ISP_Tuning_SetDefog_Strength_ptr;
IMP_ISP_Tuning_GetDefog_Strength_t IMP_ISP_Tuning_GetDefog_Strength_ptr;
IMP_ISP_Tuning_SetCsc_Attr_t IMP_ISP_Tuning_SetCsc_Attr_ptr;
IMP_ISP_Tuning_GetCsc_Attr_t IMP_ISP_Tuning_GetCsc_Attr_ptr;
IMP_ISP_Tuning_SetWdr_OutputMode_t IMP_ISP_Tuning_SetWdr_OutputMode_ptr;
IMP_ISP_Tuning_GetWdr_OutputMode_t IMP_ISP_Tuning_GetWdr_OutputMode_ptr;
IMP_ISP_SetFrameDrop_t IMP_ISP_SetFrameDrop_ptr;
IMP_ISP_GetFrameDrop_t IMP_ISP_GetFrameDrop_ptr;
IMP_ISP_SetFixedContraster_t IMP_ISP_SetFixedContraster_ptr;
IMP_ISP_SET_GPIO_INIT_OR_FREE_t IMP_ISP_SET_GPIO_INIT_OR_FREE_ptr;
IMP_ISP_SET_GPIO_STA_t IMP_ISP_SET_GPIO_STA_ptr;
IMP_System_Init_t IMP_System_Init_ptr;
IMP_System_Exit_t IMP_System_Exit_ptr;
IMP_System_GetTimeStamp_t IMP_System_GetTimeStamp_ptr;
IMP_System_RebaseTimeStamp_t IMP_System_RebaseTimeStamp_ptr;
IMP_System_ReadReg32_t IMP_System_ReadReg32_ptr;
IMP_System_WriteReg32_t IMP_System_WriteReg32_ptr;
IMP_System_GetVersion_t IMP_System_GetVersion_ptr;
IMP_System_Bind_t IMP_System_Bind_ptr;
IMP_System_UnBind_t IMP_System_UnBind_ptr;
IMP_System_GetBindbyDest_t IMP_System_GetBindbyDest_ptr;
IMP_System_MemPoolRequest_t IMP_System_MemPoolRequest_ptr;
IMP_System_MemPoolFree_t IMP_System_MemPoolFree_ptr;
IMP_Encoder_CreateGroup_t IMP_Encoder_CreateGroup_ptr;
IMP_Encoder_DestroyGroup_t IMP_Encoder_DestroyGroup_ptr;
IMP_Encoder_SetDefaultParam_t IMP_Encoder_SetDefaultParam_ptr;
IMP_Encoder_CreateChn_t IMP_Encoder_CreateChn_ptr;
IMP_Encoder_DestroyChn_t IMP_Encoder_DestroyChn_ptr;
IMP_Encoder_GetChnAttr_t IMP_Encoder_GetChnAttr_ptr;
IMP_Encoder_RegisterChn_t IMP_Encoder_RegisterChn_ptr;
IMP_Encoder_UnRegisterChn_t IMP_Encoder_UnRegisterChn_ptr;
IMP_Encoder_StartRecvPic_t IMP_Encoder_StartRecvPic_ptr;
IMP_Encoder_StopRecvPic_t IMP_Encoder_StopRecvPic_ptr;
IMP_Encoder_Query_t IMP_Encoder_Query_ptr;
IMP_Encoder_GetStream_t IMP_Encoder_GetStream_ptr;
IMP_Encoder_ReleaseStream_t IMP_Encoder_ReleaseStream_ptr;
IMP_Encoder_PollingStream_t IMP_Encoder_PollingStream_ptr;
IMP_Encoder_GetFd_t IMP_Encoder_GetFd_ptr;
IMP_Encoder_SetbufshareChn_t IMP_Encoder_SetbufshareChn_ptr;
IMP_Encoder_SetChnResizeMode_t IMP_Encoder_SetChnResizeMode_ptr;
IMP_Encoder_SetMaxStreamCnt_t IMP_Encoder_SetMaxStreamCnt_ptr;
IMP_Encoder_GetMaxStreamCnt_t IMP_Encoder_GetMaxStreamCnt_ptr;
IMP_Encoder_RequestIDR_t IMP_Encoder_RequestIDR_ptr;
IMP_Encoder_FlushStream_t IMP_Encoder_FlushStream_ptr;
IMP_Encoder_GetChnFrmRate_t IMP_Encoder_GetChnFrmRate_ptr;
IMP_Encoder_SetChnFrmRate_t IMP_Encoder_SetChnFrmRate_ptr;
IMP_Encoder_SetChnBitRate_t IMP_Encoder_SetChnBitRate_ptr;
IMP_Encoder_SetChnGopLength_t IMP_Encoder_SetChnGopLength_ptr;
IMP_Encoder_GetChnAttrRcMode_t IMP_Encoder_GetChnAttrRcMode_ptr;
IMP_Encoder_SetChnAttrRcMode_t IMP_Encoder_SetChnAttrRcMode_ptr;;
IMP_Encoder_GetChnGopAttr_t IMP_Encoder_GetChnGopAttr_ptr;
IMP_Encoder_SetChnGopAttr_t IMP_Encoder_SetChnGopAttr_ptr;
IMP_Encoder_SetChnQp_t IMP_Encoder_SetChnQp_ptr;
IMP_Encoder_SetChnQpBounds_t IMP_Encoder_SetChnQpBounds_ptr;
IMP_Encoder_SetChnQpIPDelta_t IMP_Encoder_SetChnQpIPDelta_ptr;
IMP_Encoder_SetFisheyeEnableStatus_t IMP_Encoder_SetFisheyeEnableStatus_ptr;
IMP_Encoder_GetFisheyeEnableStatus_t IMP_Encoder_GetFisheyeEnableStatus_ptr;
IMP_Encoder_GetChnEncType_t IMP_Encoder_GetChnEncType_ptr;
IMP_Encoder_SetPool_t IMP_Encoder_SetPool_ptr;
IMP_Encoder_GetPool_t IMP_Encoder_GetPool_ptr;
IMP_Encoder_SetStreamBufSize_t IMP_Encoder_SetStreamBufSize_ptr;
IMP_Encoder_GetStreamBufSize_t IMP_Encoder_GetStreamBufSize_ptr;
IMP_Encoder_GetChnAveBitrate_t IMP_Encoder_GetChnAveBitrate_ptr;
IMP_Encoder_GetChnEvalInfo_t IMP_Encoder_GetChnEvalInfo_ptr;
IMP_Encoder_SetChnEntropyMode_t IMP_Encoder_SetChnEntropyMode_ptr;
IMP_Encoder_SetFrameRelease_t IMP_Encoder_SetFrameRelease_ptr;
IMP_IVS_CreateMoveInterface_t *IMP_IVS_CreateMoveInterface_ptr;
IMP_IVS_DestroyMoveInterface_t IMP_IVS_DestroyMoveInterface_ptr;
/*imp_log_fun_t imp_log_fun_ptr;
IMP_Log_Set_Option_t IMP_Log_Set_Option_ptr;
IMP_Log_Get_Option_t IMP_Log_Get_Option_ptr;*/
IMP_FrameSource_CreateChn_t IMP_FrameSource_CreateChn_ptr;
IMP_FrameSource_DestroyChn_t IMP_FrameSource_DestroyChn_ptr;
IMP_FrameSource_EnableChn_t IMP_FrameSource_EnableChn_ptr;
IMP_FrameSource_DisableChn_t IMP_FrameSource_DisableChn_ptr;
IMP_FrameSource_SetSource_t IMP_FrameSource_SetSource_ptr;
IMP_FrameSource_GetChnAttr_t IMP_FrameSource_GetChnAttr_ptr;
IMP_FrameSource_SetChnAttr_t IMP_FrameSource_SetChnAttr_ptr;
IMP_FrameSource_SetFrameDepth_t IMP_FrameSource_SetFrameDepth_ptr;
IMP_FrameSource_GetFrameDepth_t IMP_FrameSource_GetFrameDepth_ptr;
IMP_FrameSource_GetFrame_t IMP_FrameSource_GetFrame_ptr;
IMP_FrameSource_GetTimedFrame_t IMP_FrameSource_GetTimedFrame_ptr;
IMP_FrameSource_ReleaseFrame_t IMP_FrameSource_ReleaseFrame_ptr;
IMP_FrameSource_SnapFrame_t IMP_FrameSource_SnapFrame_ptr;
IMP_FrameSource_SetMaxDelay_t IMP_FrameSource_SetMaxDelay_ptr;
IMP_FrameSource_GetMaxDelay_t IMP_FrameSource_GetMaxDelay_ptr;
IMP_FrameSource_SetDelay_t IMP_FrameSource_SetDelay_ptr;
IMP_FrameSource_GetDelay_t IMP_FrameSource_GetDelay_ptr;
IMP_FrameSource_SetChnFifoAttr_t IMP_FrameSource_SetChnFifoAttr_ptr;
IMP_FrameSource_GetChnFifoAttr_t IMP_FrameSource_GetChnFifoAttr_ptr;
IMP_FrameSource_SetPool_t IMP_FrameSource_SetPool_ptr;
IMP_FrameSource_GetPool_t IMP_FrameSource_GetPool_ptr;
IMP_FrameSource_ChnStatQuery_t IMP_FrameSource_ChnStatQuery_ptr;
IMP_FrameSource_SetChnRotate_t IMP_FrameSource_SetChnRotate_ptr;

void *handle;
void init_libimp_v1() {
	handle = dlopen("/usr/lib/libimp.so", RTLD_LAZY);
	if (!handle) {
		fprintf(stderr, "Error loading libimp.so: %s\n", dlerror());
		return;
	}

	IMP_System_GetCPUInfo_ptr = (IMP_System_GetCPUInfo_t)dlsym(handle, "IMP_System_GetCPUInfo");
	if (!IMP_System_GetCPUInfo_ptr) fprintf(stderr, "Error loading symbol: %s\n", dlerror());

	IMP_IVS_CreateGroup_ptr = (IMP_IVS_CreateGroup_t)dlsym(handle, "IMP_IVS_CreateGroup");
	if (!IMP_IVS_CreateGroup_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_DestroyGroup_ptr = (IMP_IVS_DestroyGroup_t)dlsym(handle, "IMP_IVS_DestroyGroup");
	if (!IMP_IVS_DestroyGroup_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_CreateChn_ptr = (IMP_IVS_CreateChn_t)dlsym(handle, "IMP_IVS_CreateChn");
	if (!IMP_IVS_CreateChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_DestroyChn_ptr = (IMP_IVS_DestroyChn_t)dlsym(handle, "IMP_IVS_DestroyChn");
	if (!IMP_IVS_DestroyChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_RegisterChn_ptr = (IMP_IVS_RegisterChn_t)dlsym(handle, "IMP_IVS_RegisterChn");
	if (!IMP_IVS_RegisterChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_UnRegisterChn_ptr = (IMP_IVS_UnRegisterChn_t)dlsym(handle, "IMP_IVS_UnRegisterChn");
	if (!IMP_IVS_UnRegisterChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_StartRecvPic_ptr = (IMP_IVS_StartRecvPic_t)dlsym(handle, "IMP_IVS_StartRecvPic");
	if (!IMP_IVS_StartRecvPic_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_StopRecvPic_ptr = (IMP_IVS_StopRecvPic_t)dlsym(handle, "IMP_IVS_StopRecvPic");
	if (!IMP_IVS_StopRecvPic_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_PollingResult_ptr = (IMP_IVS_PollingResult_t)dlsym(handle, "IMP_IVS_PollingResult");
	if (!IMP_IVS_PollingResult_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_GetResult_ptr = (IMP_IVS_GetResult_t)dlsym(handle, "IMP_IVS_GetResult");
	if (!IMP_IVS_GetResult_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_ReleaseResult_ptr = (IMP_IVS_ReleaseResult_t)dlsym(handle, "IMP_IVS_ReleaseResult");
	if (!IMP_IVS_ReleaseResult_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_ReleaseData_ptr = (IMP_IVS_ReleaseData_t)dlsym(handle, "IMP_IVS_ReleaseData");
	if (!IMP_IVS_ReleaseData_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_GetParam_ptr = (IMP_IVS_GetParam_t)dlsym(handle, "IMP_IVS_GetParam");
	if (!IMP_IVS_GetParam_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_SetParam_ptr = (IMP_IVS_SetParam_t)dlsym(handle, "IMP_IVS_SetParam");
	if (!IMP_IVS_SetParam_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_SetUserInfo_ptr = (IMP_DMIC_SetUserInfo_t)dlsym(handle, "IMP_DMIC_SetUserInfo");
	if (!IMP_DMIC_SetUserInfo_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_SetPubAttr_ptr = (IMP_DMIC_SetPubAttr_t)dlsym(handle, "IMP_DMIC_SetPubAttr");
	if (!IMP_DMIC_SetPubAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_GetPubAttr_ptr = (IMP_DMIC_GetPubAttr_t)dlsym(handle, "IMP_DMIC_GetPubAttr");
	if (!IMP_DMIC_GetPubAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_Enable_ptr = (IMP_DMIC_Enable_t)dlsym(handle, "IMP_DMIC_Enable");
	if (!IMP_DMIC_Enable_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_Disable_ptr = (IMP_DMIC_Disable_t)dlsym(handle, "IMP_DMIC_Disable");
	if (!IMP_DMIC_Disable_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_EnableChn_ptr = (IMP_DMIC_EnableChn_t)dlsym(handle, "IMP_DMIC_EnableChn");
	if (!IMP_DMIC_EnableChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_DisableChn_ptr = (IMP_DMIC_DisableChn_t)dlsym(handle, "IMP_DMIC_DisableChn");
	if (!IMP_DMIC_DisableChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_SetChnParam_ptr = (IMP_DMIC_SetChnParam_t)dlsym(handle, "IMP_DMIC_SetChnParam");
	if (!IMP_DMIC_SetChnParam_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_GetChnParam_ptr = (IMP_DMIC_GetChnParam_t)dlsym(handle, "IMP_DMIC_GetChnParam");
	if (!IMP_DMIC_GetChnParam_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_GetFrame_ptr = (IMP_DMIC_GetFrame_t)dlsym(handle, "IMP_DMIC_GetFrame");
	if (!IMP_DMIC_GetFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_ReleaseFrame_ptr = (IMP_DMIC_ReleaseFrame_t)dlsym(handle, "IMP_DMIC_ReleaseFrame");
	if (!IMP_DMIC_ReleaseFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_EnableAecRefFrame_ptr = (IMP_DMIC_EnableAecRefFrame_t)dlsym(handle, "IMP_DMIC_EnableAecRefFrame");
	if (!IMP_DMIC_EnableAecRefFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_GetFrameAndRef_ptr = (IMP_DMIC_GetFrameAndRef_t)dlsym(handle, "IMP_DMIC_GetFrameAndRef");
	if (!IMP_DMIC_GetFrameAndRef_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_EnableAec_ptr = (IMP_DMIC_EnableAec_t)dlsym(handle, "IMP_DMIC_EnableAec");
	if (!IMP_DMIC_EnableAec_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_DisableAec_ptr = (IMP_DMIC_DisableAec_t)dlsym(handle, "IMP_DMIC_DisableAec");
	if (!IMP_DMIC_DisableAec_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_PollingFrame_ptr = (IMP_DMIC_PollingFrame_t)dlsym(handle, "IMP_DMIC_PollingFrame");
	if (!IMP_DMIC_PollingFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_SetVol_ptr = (IMP_DMIC_SetVol_t)dlsym(handle, "IMP_DMIC_SetVol");
	if (!IMP_DMIC_SetVol_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_GetVol_ptr = (IMP_DMIC_GetVol_t)dlsym(handle, "IMP_DMIC_GetVol");
	if (!IMP_DMIC_GetVol_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_SetGain_ptr = (IMP_DMIC_SetGain_t)dlsym(handle, "IMP_DMIC_SetGain");
	if (!IMP_DMIC_SetGain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_DMIC_GetGain_ptr = (IMP_DMIC_GetGain_t)dlsym(handle, "IMP_DMIC_GetGain");
	if (!IMP_DMIC_GetGain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_SetPubAttr_ptr = (IMP_AI_SetPubAttr_t)dlsym(handle, "IMP_AI_SetPubAttr");
	if (!IMP_AI_SetPubAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_GetPubAttr_ptr = (IMP_AI_GetPubAttr_t)dlsym(handle, "IMP_AI_GetPubAttr");
	if (!IMP_AI_GetPubAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_Enable_ptr = (IMP_AI_Enable_t)dlsym(handle, "IMP_AI_Enable");
	if (!IMP_AI_Enable_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_Disable_ptr = (IMP_AI_Disable_t)dlsym(handle, "IMP_AI_Disable");
	if (!IMP_AI_Disable_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_EnableChn_ptr = (IMP_AI_EnableChn_t)dlsym(handle, "IMP_AI_EnableChn");
	if (!IMP_AI_EnableChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_DisableChn_ptr = (IMP_AI_DisableChn_t)dlsym(handle, "IMP_AI_DisableChn");
	if (!IMP_AI_DisableChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_PollingFrame_ptr = (IMP_AI_PollingFrame_t)dlsym(handle, "IMP_AI_PollingFrame");
	if (!IMP_AI_PollingFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_GetFrame_ptr = (IMP_AI_GetFrame_t)dlsym(handle, "IMP_AI_GetFrame");
	if (!IMP_AI_GetFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_ReleaseFrame_ptr = (IMP_AI_ReleaseFrame_t)dlsym(handle, "IMP_AI_ReleaseFrame");
	if (!IMP_AI_ReleaseFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_SetChnParam_ptr = (IMP_AI_SetChnParam_t)dlsym(handle, "IMP_AI_SetChnParam");
	if (!IMP_AI_SetChnParam_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_GetChnParam_ptr = (IMP_AI_GetChnParam_t)dlsym(handle, "IMP_AI_GetChnParam");
	if (!IMP_AI_GetChnParam_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_EnableAec_ptr = (IMP_AI_EnableAec_t)dlsym(handle, "IMP_AI_EnableAec");
	if (!IMP_AI_EnableAec_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_Set_WebrtcProfileIni_Path_ptr = (IMP_AI_Set_WebrtcProfileIni_Path_t)dlsym(handle, "IMP_AI_Set_WebrtcProfileIni_Path");
	if (!IMP_AI_Set_WebrtcProfileIni_Path_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_DisableAec_ptr = (IMP_AI_DisableAec_t)dlsym(handle, "IMP_AI_DisableAec");
	if (!IMP_AI_DisableAec_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_EnableNs_ptr = (IMP_AI_EnableNs_t)dlsym(handle, "IMP_AI_EnableNs");
	if (!IMP_AI_EnableNs_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_DisableNs_ptr = (IMP_AI_DisableNs_t)dlsym(handle, "IMP_AI_DisableNs");
	if (!IMP_AI_DisableNs_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_SetAgcMode_ptr = (IMP_AI_SetAgcMode_t)dlsym(handle, "IMP_AI_SetAgcMode");
	if (!IMP_AI_SetAgcMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_EnableAgc_ptr = (IMP_AI_EnableAgc_t)dlsym(handle, "IMP_AI_EnableAgc");
	if (!IMP_AI_EnableAgc_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_DisableAgc_ptr = (IMP_AI_DisableAgc_t)dlsym(handle, "IMP_AI_DisableAgc");
	if (!IMP_AI_DisableAgc_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_EnableAgc_ptr = (IMP_AO_EnableAgc_t)dlsym(handle, "IMP_AO_EnableAgc");
	if (!IMP_AO_EnableAgc_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_DisableAgc_ptr = (IMP_AO_DisableAgc_t)dlsym(handle, "IMP_AO_DisableAgc");
	if (!IMP_AO_DisableAgc_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_EnableHpf_ptr = (IMP_AI_EnableHpf_t)dlsym(handle, "IMP_AI_EnableHpf");
	if (!IMP_AI_EnableHpf_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_SetHpfCoFrequency_ptr = (IMP_AI_SetHpfCoFrequency_t)dlsym(handle, "IMP_AI_SetHpfCoFrequency");
	if (!IMP_AI_SetHpfCoFrequency_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_DisableHpf_ptr = (IMP_AI_DisableHpf_t)dlsym(handle, "IMP_AI_DisableHpf");
	if (!IMP_AI_DisableHpf_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_EnableHpf_ptr = (IMP_AO_EnableHpf_t)dlsym(handle, "IMP_AO_EnableHpf");
	if (!IMP_AO_EnableHpf_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_SetHpfCoFrequency_ptr = (IMP_AO_SetHpfCoFrequency_t)dlsym(handle, "IMP_AO_SetHpfCoFrequency");
	if (!IMP_AO_SetHpfCoFrequency_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_DisableHpf_ptr = (IMP_AO_DisableHpf_t)dlsym(handle, "IMP_AO_DisableHpf");
	if (!IMP_AO_DisableHpf_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_SetPubAttr_ptr = (IMP_AO_SetPubAttr_t)dlsym(handle, "IMP_AO_SetPubAttr");
	if (!IMP_AO_SetPubAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_GetPubAttr_ptr = (IMP_AO_GetPubAttr_t)dlsym(handle, "IMP_AO_GetPubAttr");
	if (!IMP_AO_GetPubAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_Enable_ptr = (IMP_AO_Enable_t)dlsym(handle, "IMP_AO_Enable");
	if (!IMP_AO_Enable_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_Disable_ptr = (IMP_AO_Disable_t)dlsym(handle, "IMP_AO_Disable");
	if (!IMP_AO_Disable_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_EnableChn_ptr = (IMP_AO_EnableChn_t)dlsym(handle, "IMP_AO_EnableChn");
	if (!IMP_AO_EnableChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_DisableChn_ptr = (IMP_AO_DisableChn_t)dlsym(handle, "IMP_AO_DisableChn");
	if (!IMP_AO_DisableChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_SendFrame_ptr = (IMP_AO_SendFrame_t)dlsym(handle, "IMP_AO_SendFrame");
	if (!IMP_AO_SendFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_PauseChn_ptr = (IMP_AO_PauseChn_t)dlsym(handle, "IMP_AO_PauseChn");
	if (!IMP_AO_PauseChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_ResumeChn_ptr = (IMP_AO_ResumeChn_t)dlsym(handle, "IMP_AO_ResumeChn");
	if (!IMP_AO_ResumeChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_ClearChnBuf_ptr = (IMP_AO_ClearChnBuf_t)dlsym(handle, "IMP_AO_ClearChnBuf");
	if (!IMP_AO_ClearChnBuf_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_QueryChnStat_ptr = (IMP_AO_QueryChnStat_t)dlsym(handle, "IMP_AO_QueryChnStat");
	if (!IMP_AO_QueryChnStat_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AENC_CreateChn_ptr = (IMP_AENC_CreateChn_t)dlsym(handle, "IMP_AENC_CreateChn");
	if (!IMP_AENC_CreateChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AENC_DestroyChn_ptr = (IMP_AENC_DestroyChn_t)dlsym(handle, "IMP_AENC_DestroyChn");
	if (!IMP_AENC_DestroyChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AENC_SendFrame_ptr = (IMP_AENC_SendFrame_t)dlsym(handle, "IMP_AENC_SendFrame");
	if (!IMP_AENC_SendFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AENC_PollingStream_ptr = (IMP_AENC_PollingStream_t)dlsym(handle, "IMP_AENC_PollingStream");
	if (!IMP_AENC_PollingStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AENC_GetStream_ptr = (IMP_AENC_GetStream_t)dlsym(handle, "IMP_AENC_GetStream");
	if (!IMP_AENC_GetStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AENC_ReleaseStream_ptr = (IMP_AENC_ReleaseStream_t)dlsym(handle, "IMP_AENC_ReleaseStream");
	if (!IMP_AENC_ReleaseStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AENC_RegisterEncoder_ptr = (IMP_AENC_RegisterEncoder_t)dlsym(handle, "IMP_AENC_RegisterEncoder");
	if (!IMP_AENC_RegisterEncoder_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AENC_UnRegisterEncoder_ptr = (IMP_AENC_UnRegisterEncoder_t)dlsym(handle, "IMP_AENC_UnRegisterEncoder");
	if (!IMP_AENC_UnRegisterEncoder_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ADEC_CreateChn_ptr = (IMP_ADEC_CreateChn_t)dlsym(handle, "IMP_ADEC_CreateChn");
	if (!IMP_ADEC_CreateChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ADEC_DestroyChn_ptr = (IMP_ADEC_DestroyChn_t)dlsym(handle, "IMP_ADEC_DestroyChn");
	if (!IMP_ADEC_DestroyChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ADEC_SendStream_ptr = (IMP_ADEC_SendStream_t)dlsym(handle, "IMP_ADEC_SendStream");
	if (!IMP_ADEC_SendStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ADEC_PollingStream_ptr = (IMP_ADEC_PollingStream_t)dlsym(handle, "IMP_ADEC_PollingStream");
	if (!IMP_ADEC_PollingStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ADEC_GetStream_ptr = (IMP_ADEC_GetStream_t)dlsym(handle, "IMP_ADEC_GetStream");
	if (!IMP_ADEC_GetStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ADEC_ReleaseStream_ptr = (IMP_ADEC_ReleaseStream_t)dlsym(handle, "IMP_ADEC_ReleaseStream");
	if (!IMP_ADEC_ReleaseStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ADEC_ClearChnBuf_ptr = (IMP_ADEC_ClearChnBuf_t)dlsym(handle, "IMP_ADEC_ClearChnBuf");
	if (!IMP_ADEC_ClearChnBuf_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ADEC_RegisterDecoder_ptr = (IMP_ADEC_RegisterDecoder_t)dlsym(handle, "IMP_ADEC_RegisterDecoder");
	if (!IMP_ADEC_RegisterDecoder_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ADEC_UnRegisterDecoder_ptr = (IMP_ADEC_UnRegisterDecoder_t)dlsym(handle, "IMP_ADEC_UnRegisterDecoder");
	if (!IMP_ADEC_UnRegisterDecoder_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_SetVol_ptr = (IMP_AI_SetVol_t)dlsym(handle, "IMP_AI_SetVol");
	if (!IMP_AI_SetVol_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_GetVol_ptr = (IMP_AI_GetVol_t)dlsym(handle, "IMP_AI_GetVol");
	if (!IMP_AI_GetVol_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_SetVolMute_ptr = (IMP_AI_SetVolMute_t)dlsym(handle, "IMP_AI_SetVolMute");
	if (!IMP_AI_SetVolMute_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_SetVol_ptr = (IMP_AO_SetVol_t)dlsym(handle, "IMP_AO_SetVol");
	if (!IMP_AO_SetVol_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_GetVol_ptr = (IMP_AO_GetVol_t)dlsym(handle, "IMP_AO_GetVol");
	if (!IMP_AO_GetVol_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_SetVolMute_ptr = (IMP_AO_SetVolMute_t)dlsym(handle, "IMP_AO_SetVolMute");
	if (!IMP_AO_SetVolMute_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_SetGain_ptr = (IMP_AI_SetGain_t)dlsym(handle, "IMP_AI_SetGain");
	if (!IMP_AI_SetGain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_GetGain_ptr = (IMP_AI_GetGain_t)dlsym(handle, "IMP_AI_GetGain");
	if (!IMP_AI_GetGain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_SetAlcGain_ptr = (IMP_AI_SetAlcGain_t)dlsym(handle, "IMP_AI_SetAlcGain");
	if (!IMP_AI_SetAlcGain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_GetAlcGain_ptr = (IMP_AI_GetAlcGain_t)dlsym(handle, "IMP_AI_GetAlcGain");
	if (!IMP_AI_GetAlcGain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_SetGain_ptr = (IMP_AO_SetGain_t)dlsym(handle, "IMP_AO_SetGain");
	if (!IMP_AO_SetGain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_GetGain_ptr = (IMP_AO_GetGain_t)dlsym(handle, "IMP_AO_GetGain");
	if (!IMP_AO_GetGain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_Soft_Mute_ptr = (IMP_AO_Soft_Mute_t)dlsym(handle, "IMP_AO_Soft_Mute");
	if (!IMP_AO_Soft_Mute_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_Soft_UNMute_ptr = (IMP_AO_Soft_UNMute_t)dlsym(handle, "IMP_AO_Soft_UNMute");
	if (!IMP_AO_Soft_UNMute_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_GetFrameAndRef_ptr = (IMP_AI_GetFrameAndRef_t)dlsym(handle, "IMP_AI_GetFrameAndRef");
	if (!IMP_AI_GetFrameAndRef_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_EnableAecRefFrame_ptr = (IMP_AI_EnableAecRefFrame_t)dlsym(handle, "IMP_AI_EnableAecRefFrame");
	if (!IMP_AI_EnableAecRefFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AI_DisableAecRefFrame_ptr = (IMP_AI_DisableAecRefFrame_t)dlsym(handle, "IMP_AI_DisableAecRefFrame");
	if (!IMP_AI_DisableAecRefFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_CacheSwitch_ptr = (IMP_AO_CacheSwitch_t)dlsym(handle, "IMP_AO_CacheSwitch");
	if (!IMP_AO_CacheSwitch_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_AO_FlushChnBuf_ptr = (IMP_AO_FlushChnBuf_t)dlsym(handle, "IMP_AO_FlushChnBuf");
	if (!IMP_AO_FlushChnBuf_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	/**IMP_IVS_CreateBaseMoveInterface_ptr = (IMP_IVS_CreateBaseMoveInterface_t)dlsym(handle, "*IMP_IVS_CreateBaseMoveInterface");
	if (!*IMP_IVS_CreateBaseMoveInterface_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_IVS_DestroyBaseMoveInterface_ptr = (IMP_IVS_DestroyBaseMoveInterface_t)dlsym(handle, "IMP_IVS_DestroyBaseMoveInterface");
	if (!IMP_IVS_DestroyBaseMoveInterface_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMPPixfmtToString_ptr = (IMPPixfmtToString_t)dlsym(handle, "*IMPPixfmtToString");
	if (!*IMPPixfmtToString_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}*/
	IMP_OSD_CreateGroup_ptr = (IMP_OSD_CreateGroup_t)dlsym(handle, "IMP_OSD_CreateGroup");
	if (!IMP_OSD_CreateGroup_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_DestroyGroup_ptr = (IMP_OSD_DestroyGroup_t)dlsym(handle, "IMP_OSD_DestroyGroup");
	if (!IMP_OSD_DestroyGroup_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_AttachToGroup_ptr = (IMP_OSD_AttachToGroup_t)dlsym(handle, "IMP_OSD_AttachToGroup");
	if (!IMP_OSD_AttachToGroup_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_CreateRgn_ptr = (IMP_OSD_CreateRgn_t)dlsym(handle, "IMP_OSD_CreateRgn");
	if (!IMP_OSD_CreateRgn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_DestroyRgn_ptr = (IMP_OSD_DestroyRgn_t)dlsym(handle, "IMP_OSD_DestroyRgn");
	if (!IMP_OSD_DestroyRgn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_RegisterRgn_ptr = (IMP_OSD_RegisterRgn_t)dlsym(handle, "IMP_OSD_RegisterRgn");
	if (!IMP_OSD_RegisterRgn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_UnRegisterRgn_ptr = (IMP_OSD_UnRegisterRgn_t)dlsym(handle, "IMP_OSD_UnRegisterRgn");
	if (!IMP_OSD_UnRegisterRgn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_SetRgnAttr_ptr = (IMP_OSD_SetRgnAttr_t)dlsym(handle, "IMP_OSD_SetRgnAttr");
	if (!IMP_OSD_SetRgnAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_SetRgnAttrWithTimestamp_ptr = (IMP_OSD_SetRgnAttrWithTimestamp_t)dlsym(handle, "IMP_OSD_SetRgnAttrWithTimestamp");
	if (!IMP_OSD_SetRgnAttrWithTimestamp_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_GetRgnAttr_ptr = (IMP_OSD_GetRgnAttr_t)dlsym(handle, "IMP_OSD_GetRgnAttr");
	if (!IMP_OSD_GetRgnAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_UpdateRgnAttrData_ptr = (IMP_OSD_UpdateRgnAttrData_t)dlsym(handle, "IMP_OSD_UpdateRgnAttrData");
	if (!IMP_OSD_UpdateRgnAttrData_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_SetGrpRgnAttr_ptr = (IMP_OSD_SetGrpRgnAttr_t)dlsym(handle, "IMP_OSD_SetGrpRgnAttr");
	if (!IMP_OSD_SetGrpRgnAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_GetGrpRgnAttr_ptr = (IMP_OSD_GetGrpRgnAttr_t)dlsym(handle, "IMP_OSD_GetGrpRgnAttr");
	if (!IMP_OSD_GetGrpRgnAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_ShowRgn_ptr = (IMP_OSD_ShowRgn_t)dlsym(handle, "IMP_OSD_ShowRgn");
	if (!IMP_OSD_ShowRgn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_Start_ptr = (IMP_OSD_Start_t)dlsym(handle, "IMP_OSD_Start");
	if (!IMP_OSD_Start_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_OSD_Stop_ptr = (IMP_OSD_Stop_t)dlsym(handle, "IMP_OSD_Stop");
	if (!IMP_OSD_Stop_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Open_ptr = (IMP_ISP_Open_t)dlsym(handle, "IMP_ISP_Open");
	if (!IMP_ISP_Open_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Close_ptr = (IMP_ISP_Close_t)dlsym(handle, "IMP_ISP_Close");
	if (!IMP_ISP_Close_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_SetDefaultBinPath_ptr = (IMP_ISP_SetDefaultBinPath_t)dlsym(handle, "IMP_ISP_SetDefaultBinPath");
	if (!IMP_ISP_SetDefaultBinPath_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_GetDefaultBinPath_ptr = (IMP_ISP_GetDefaultBinPath_t)dlsym(handle, "IMP_ISP_GetDefaultBinPath");
	if (!IMP_ISP_GetDefaultBinPath_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_AddSensor_ptr = (IMP_ISP_AddSensor_t)dlsym(handle, "IMP_ISP_AddSensor");
	if (!IMP_ISP_AddSensor_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_DelSensor_ptr = (IMP_ISP_DelSensor_t)dlsym(handle, "IMP_ISP_DelSensor");
	if (!IMP_ISP_DelSensor_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_EnableSensor_ptr = (IMP_ISP_EnableSensor_t)dlsym(handle, "IMP_ISP_EnableSensor");
	if (!IMP_ISP_EnableSensor_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_DisableSensor_ptr = (IMP_ISP_DisableSensor_t)dlsym(handle, "IMP_ISP_DisableSensor");
	if (!IMP_ISP_DisableSensor_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_SetSensorRegister_ptr = (IMP_ISP_SetSensorRegister_t)dlsym(handle, "IMP_ISP_SetSensorRegister");
	if (!IMP_ISP_SetSensorRegister_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_GetSensorRegister_ptr = (IMP_ISP_GetSensorRegister_t)dlsym(handle, "IMP_ISP_GetSensorRegister");
	if (!IMP_ISP_GetSensorRegister_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAutoZoom_ptr = (IMP_ISP_Tuning_SetAutoZoom_t)dlsym(handle, "IMP_ISP_Tuning_SetAutoZoom");
	if (!IMP_ISP_Tuning_SetAutoZoom_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_EnableTuning_ptr = (IMP_ISP_EnableTuning_t)dlsym(handle, "IMP_ISP_EnableTuning");
	if (!IMP_ISP_EnableTuning_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_DisableTuning_ptr = (IMP_ISP_DisableTuning_t)dlsym(handle, "IMP_ISP_DisableTuning");
	if (!IMP_ISP_DisableTuning_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetSensorFPS_ptr = (IMP_ISP_Tuning_SetSensorFPS_t)dlsym(handle, "IMP_ISP_Tuning_SetSensorFPS");
	if (!IMP_ISP_Tuning_SetSensorFPS_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetSensorFPS_ptr = (IMP_ISP_Tuning_GetSensorFPS_t)dlsym(handle, "IMP_ISP_Tuning_GetSensorFPS");
	if (!IMP_ISP_Tuning_GetSensorFPS_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAntiFlickerAttr_ptr = (IMP_ISP_Tuning_SetAntiFlickerAttr_t)dlsym(handle, "IMP_ISP_Tuning_SetAntiFlickerAttr");
	if (!IMP_ISP_Tuning_SetAntiFlickerAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAntiFlickerAttr_ptr = (IMP_ISP_Tuning_GetAntiFlickerAttr_t)dlsym(handle, "IMP_ISP_Tuning_GetAntiFlickerAttr");
	if (!IMP_ISP_Tuning_GetAntiFlickerAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetBrightness_ptr = (IMP_ISP_Tuning_SetBrightness_t)dlsym(handle, "IMP_ISP_Tuning_SetBrightness");
	if (!IMP_ISP_Tuning_SetBrightness_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetBrightness_ptr = (IMP_ISP_Tuning_GetBrightness_t)dlsym(handle, "IMP_ISP_Tuning_GetBrightness");
	if (!IMP_ISP_Tuning_GetBrightness_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetContrast_ptr = (IMP_ISP_Tuning_SetContrast_t)dlsym(handle, "IMP_ISP_Tuning_SetContrast");
	if (!IMP_ISP_Tuning_SetContrast_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetContrast_ptr = (IMP_ISP_Tuning_GetContrast_t)dlsym(handle, "IMP_ISP_Tuning_GetContrast");
	if (!IMP_ISP_Tuning_GetContrast_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetSharpness_ptr = (IMP_ISP_Tuning_SetSharpness_t)dlsym(handle, "IMP_ISP_Tuning_SetSharpness");
	if (!IMP_ISP_Tuning_SetSharpness_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetSharpness_ptr = (IMP_ISP_Tuning_GetSharpness_t)dlsym(handle, "IMP_ISP_Tuning_GetSharpness");
	if (!IMP_ISP_Tuning_GetSharpness_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetBcshHue_ptr = (IMP_ISP_Tuning_SetBcshHue_t)dlsym(handle, "IMP_ISP_Tuning_SetBcshHue");
	if (!IMP_ISP_Tuning_SetBcshHue_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetBcshHue_ptr = (IMP_ISP_Tuning_GetBcshHue_t)dlsym(handle, "IMP_ISP_Tuning_GetBcshHue");
	if (!IMP_ISP_Tuning_GetBcshHue_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetSaturation_ptr = (IMP_ISP_Tuning_SetSaturation_t)dlsym(handle, "IMP_ISP_Tuning_SetSaturation");
	if (!IMP_ISP_Tuning_SetSaturation_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetSaturation_ptr = (IMP_ISP_Tuning_GetSaturation_t)dlsym(handle, "IMP_ISP_Tuning_GetSaturation");
	if (!IMP_ISP_Tuning_GetSaturation_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetISPBypass_ptr = (IMP_ISP_Tuning_SetISPBypass_t)dlsym(handle, "IMP_ISP_Tuning_SetISPBypass");
	if (!IMP_ISP_Tuning_SetISPBypass_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetTotalGain_ptr = (IMP_ISP_Tuning_GetTotalGain_t)dlsym(handle, "IMP_ISP_Tuning_GetTotalGain");
	if (!IMP_ISP_Tuning_GetTotalGain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetISPHflip_ptr = (IMP_ISP_Tuning_SetISPHflip_t)dlsym(handle, "IMP_ISP_Tuning_SetISPHflip");
	if (!IMP_ISP_Tuning_SetISPHflip_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetISPHflip_ptr = (IMP_ISP_Tuning_GetISPHflip_t)dlsym(handle, "IMP_ISP_Tuning_GetISPHflip");
	if (!IMP_ISP_Tuning_GetISPHflip_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetISPVflip_ptr = (IMP_ISP_Tuning_SetISPVflip_t)dlsym(handle, "IMP_ISP_Tuning_SetISPVflip");
	if (!IMP_ISP_Tuning_SetISPVflip_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetISPVflip_ptr = (IMP_ISP_Tuning_GetISPVflip_t)dlsym(handle, "IMP_ISP_Tuning_GetISPVflip");
	if (!IMP_ISP_Tuning_GetISPVflip_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetISPRunningMode_ptr = (IMP_ISP_Tuning_SetISPRunningMode_t)dlsym(handle, "IMP_ISP_Tuning_SetISPRunningMode");
	if (!IMP_ISP_Tuning_SetISPRunningMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetISPRunningMode_ptr = (IMP_ISP_Tuning_GetISPRunningMode_t)dlsym(handle, "IMP_ISP_Tuning_GetISPRunningMode");
	if (!IMP_ISP_Tuning_GetISPRunningMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetISPCustomMode_ptr = (IMP_ISP_Tuning_SetISPCustomMode_t)dlsym(handle, "IMP_ISP_Tuning_SetISPCustomMode");
	if (!IMP_ISP_Tuning_SetISPCustomMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetISPCustomMode_ptr = (IMP_ISP_Tuning_GetISPCustomMode_t)dlsym(handle, "IMP_ISP_Tuning_GetISPCustomMode");
	if (!IMP_ISP_Tuning_GetISPCustomMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetGamma_ptr = (IMP_ISP_Tuning_SetGamma_t)dlsym(handle, "IMP_ISP_Tuning_SetGamma");
	if (!IMP_ISP_Tuning_SetGamma_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetGamma_ptr = (IMP_ISP_Tuning_GetGamma_t)dlsym(handle, "IMP_ISP_Tuning_GetGamma");
	if (!IMP_ISP_Tuning_GetGamma_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAeComp_ptr = (IMP_ISP_Tuning_SetAeComp_t)dlsym(handle, "IMP_ISP_Tuning_SetAeComp");
	if (!IMP_ISP_Tuning_SetAeComp_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeComp_ptr = (IMP_ISP_Tuning_GetAeComp_t)dlsym(handle, "IMP_ISP_Tuning_GetAeComp");
	if (!IMP_ISP_Tuning_GetAeComp_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeLuma_ptr = (IMP_ISP_Tuning_GetAeLuma_t)dlsym(handle, "IMP_ISP_Tuning_GetAeLuma");
	if (!IMP_ISP_Tuning_GetAeLuma_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAeFreeze_ptr = (IMP_ISP_Tuning_SetAeFreeze_t)dlsym(handle, "IMP_ISP_Tuning_SetAeFreeze");
	if (!IMP_ISP_Tuning_SetAeFreeze_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetExpr_ptr = (IMP_ISP_Tuning_SetExpr_t)dlsym(handle, "IMP_ISP_Tuning_SetExpr");
	if (!IMP_ISP_Tuning_SetExpr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetExpr_ptr = (IMP_ISP_Tuning_GetExpr_t)dlsym(handle, "IMP_ISP_Tuning_GetExpr");
	if (!IMP_ISP_Tuning_GetExpr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetWB_ptr = (IMP_ISP_Tuning_SetWB_t)dlsym(handle, "IMP_ISP_Tuning_SetWB");
	if (!IMP_ISP_Tuning_SetWB_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetWB_ptr = (IMP_ISP_Tuning_GetWB_t)dlsym(handle, "IMP_ISP_Tuning_GetWB");
	if (!IMP_ISP_Tuning_GetWB_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetWB_Statis_ptr = (IMP_ISP_Tuning_GetWB_Statis_t)dlsym(handle, "IMP_ISP_Tuning_GetWB_Statis");
	if (!IMP_ISP_Tuning_GetWB_Statis_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetWB_GOL_Statis_ptr = (IMP_ISP_Tuning_GetWB_GOL_Statis_t)dlsym(handle, "IMP_ISP_Tuning_GetWB_GOL_Statis");
	if (!IMP_ISP_Tuning_GetWB_GOL_Statis_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAwbClust_ptr = (IMP_ISP_Tuning_SetAwbClust_t)dlsym(handle, "IMP_ISP_Tuning_SetAwbClust");
	if (!IMP_ISP_Tuning_SetAwbClust_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAwbClust_ptr = (IMP_ISP_Tuning_GetAwbClust_t)dlsym(handle, "IMP_ISP_Tuning_GetAwbClust");
	if (!IMP_ISP_Tuning_GetAwbClust_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAwbCtTrend_ptr = (IMP_ISP_Tuning_SetAwbCtTrend_t)dlsym(handle, "IMP_ISP_Tuning_SetAwbCtTrend");
	if (!IMP_ISP_Tuning_SetAwbCtTrend_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAwbCtTrend_ptr = (IMP_ISP_Tuning_GetAwbCtTrend_t)dlsym(handle, "IMP_ISP_Tuning_GetAwbCtTrend");
	if (!IMP_ISP_Tuning_GetAwbCtTrend_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_Awb_GetRgbCoefft_ptr = (IMP_ISP_Tuning_Awb_GetRgbCoefft_t)dlsym(handle, "IMP_ISP_Tuning_Awb_GetRgbCoefft");
	if (!IMP_ISP_Tuning_Awb_GetRgbCoefft_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_Awb_SetRgbCoefft_ptr = (IMP_ISP_Tuning_Awb_SetRgbCoefft_t)dlsym(handle, "IMP_ISP_Tuning_Awb_SetRgbCoefft");
	if (!IMP_ISP_Tuning_Awb_SetRgbCoefft_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetMaxAgain_ptr = (IMP_ISP_Tuning_SetMaxAgain_t)dlsym(handle, "IMP_ISP_Tuning_SetMaxAgain");
	if (!IMP_ISP_Tuning_SetMaxAgain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetMaxAgain_ptr = (IMP_ISP_Tuning_GetMaxAgain_t)dlsym(handle, "IMP_ISP_Tuning_GetMaxAgain");
	if (!IMP_ISP_Tuning_GetMaxAgain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetMaxDgain_ptr = (IMP_ISP_Tuning_SetMaxDgain_t)dlsym(handle, "IMP_ISP_Tuning_SetMaxDgain");
	if (!IMP_ISP_Tuning_SetMaxDgain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetMaxDgain_ptr = (IMP_ISP_Tuning_GetMaxDgain_t)dlsym(handle, "IMP_ISP_Tuning_GetMaxDgain");
	/*if (!IMP_ISP_Tuning_GetMaxDgain_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}*/
	IMP_ISP_Tuning_SetHiLightDepress_ptr = (IMP_ISP_Tuning_SetHiLightDepress_t)dlsym(handle, "IMP_ISP_Tuning_SetHiLightDepress");
	if (!IMP_ISP_Tuning_SetHiLightDepress_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetHiLightDepress_ptr = (IMP_ISP_Tuning_GetHiLightDepress_t)dlsym(handle, "IMP_ISP_Tuning_GetHiLightDepress");
	if (!IMP_ISP_Tuning_GetHiLightDepress_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetBacklightComp_ptr = (IMP_ISP_Tuning_SetBacklightComp_t)dlsym(handle, "IMP_ISP_Tuning_SetBacklightComp");
	if (!IMP_ISP_Tuning_SetBacklightComp_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetBacklightComp_ptr = (IMP_ISP_Tuning_GetBacklightComp_t)dlsym(handle, "IMP_ISP_Tuning_GetBacklightComp");
	if (!IMP_ISP_Tuning_GetBacklightComp_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetTemperStrength_ptr = (IMP_ISP_Tuning_SetTemperStrength_t)dlsym(handle, "IMP_ISP_Tuning_SetTemperStrength");
	if (!IMP_ISP_Tuning_SetTemperStrength_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetSinterStrength_ptr = (IMP_ISP_Tuning_SetSinterStrength_t)dlsym(handle, "IMP_ISP_Tuning_SetSinterStrength");
	if (!IMP_ISP_Tuning_SetSinterStrength_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetEVAttr_ptr = (IMP_ISP_Tuning_GetEVAttr_t)dlsym(handle, "IMP_ISP_Tuning_GetEVAttr");
	if (!IMP_ISP_Tuning_GetEVAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_EnableMovestate_ptr = (IMP_ISP_Tuning_EnableMovestate_t)dlsym(handle, "IMP_ISP_Tuning_EnableMovestate");
	if (!IMP_ISP_Tuning_EnableMovestate_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_DisableMovestate_ptr = (IMP_ISP_Tuning_DisableMovestate_t)dlsym(handle, "IMP_ISP_Tuning_DisableMovestate");
	if (!IMP_ISP_Tuning_DisableMovestate_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAeWeight_ptr = (IMP_ISP_Tuning_SetAeWeight_t)dlsym(handle, "IMP_ISP_Tuning_SetAeWeight");
	if (!IMP_ISP_Tuning_SetAeWeight_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeWeight_ptr = (IMP_ISP_Tuning_GetAeWeight_t)dlsym(handle, "IMP_ISP_Tuning_GetAeWeight");
	if (!IMP_ISP_Tuning_GetAeWeight_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_AE_GetROI_ptr = (IMP_ISP_Tuning_AE_GetROI_t)dlsym(handle, "IMP_ISP_Tuning_AE_GetROI");
	if (!IMP_ISP_Tuning_AE_GetROI_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_AE_SetROI_ptr = (IMP_ISP_Tuning_AE_SetROI_t)dlsym(handle, "IMP_ISP_Tuning_AE_SetROI");
	if (!IMP_ISP_Tuning_AE_SetROI_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAwbWeight_ptr = (IMP_ISP_Tuning_SetAwbWeight_t)dlsym(handle, "IMP_ISP_Tuning_SetAwbWeight");
	if (!IMP_ISP_Tuning_SetAwbWeight_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAwbWeight_ptr = (IMP_ISP_Tuning_GetAwbWeight_t)dlsym(handle, "IMP_ISP_Tuning_GetAwbWeight");
	if (!IMP_ISP_Tuning_GetAwbWeight_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAwbZone_ptr = (IMP_ISP_Tuning_GetAwbZone_t)dlsym(handle, "IMP_ISP_Tuning_GetAwbZone");
	if (!IMP_ISP_Tuning_GetAwbZone_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetWB_ALGO_ptr = (IMP_ISP_Tuning_SetWB_ALGO_t)dlsym(handle, "IMP_ISP_Tuning_SetWB_ALGO");
	if (!IMP_ISP_Tuning_SetWB_ALGO_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAeHist_ptr = (IMP_ISP_Tuning_SetAeHist_t)dlsym(handle, "IMP_ISP_Tuning_SetAeHist");
	if (!IMP_ISP_Tuning_SetAeHist_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeHist_ptr = (IMP_ISP_Tuning_GetAeHist_t)dlsym(handle, "IMP_ISP_Tuning_GetAeHist");
	if (!IMP_ISP_Tuning_GetAeHist_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeHist_Origin_ptr = (IMP_ISP_Tuning_GetAeHist_Origin_t)dlsym(handle, "IMP_ISP_Tuning_GetAeHist_Origin");
	if (!IMP_ISP_Tuning_GetAeHist_Origin_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAwbHist_ptr = (IMP_ISP_Tuning_GetAwbHist_t)dlsym(handle, "IMP_ISP_Tuning_GetAwbHist");
	if (!IMP_ISP_Tuning_GetAwbHist_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAwbHist_ptr = (IMP_ISP_Tuning_SetAwbHist_t)dlsym(handle, "IMP_ISP_Tuning_SetAwbHist");
	if (!IMP_ISP_Tuning_SetAwbHist_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAFMetrices_ptr = (IMP_ISP_Tuning_GetAFMetrices_t)dlsym(handle, "IMP_ISP_Tuning_GetAFMetrices");
	if (!IMP_ISP_Tuning_GetAFMetrices_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAfHist_ptr = (IMP_ISP_Tuning_GetAfHist_t)dlsym(handle, "IMP_ISP_Tuning_GetAfHist");
	if (!IMP_ISP_Tuning_GetAfHist_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAfHist_ptr = (IMP_ISP_Tuning_SetAfHist_t)dlsym(handle, "IMP_ISP_Tuning_SetAfHist");
	if (!IMP_ISP_Tuning_SetAfHist_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAfWeight_ptr = (IMP_ISP_Tuning_SetAfWeight_t)dlsym(handle, "IMP_ISP_Tuning_SetAfWeight");
	if (!IMP_ISP_Tuning_SetAfWeight_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAfWeight_ptr = (IMP_ISP_Tuning_GetAfWeight_t)dlsym(handle, "IMP_ISP_Tuning_GetAfWeight");
	if (!IMP_ISP_Tuning_GetAfWeight_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAfZone_ptr = (IMP_ISP_Tuning_GetAfZone_t)dlsym(handle, "IMP_ISP_Tuning_GetAfZone");
	if (!IMP_ISP_Tuning_GetAfZone_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_WaitFrame_ptr = (IMP_ISP_Tuning_WaitFrame_t)dlsym(handle, "IMP_ISP_Tuning_WaitFrame");
	if (!IMP_ISP_Tuning_WaitFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAeMin_ptr = (IMP_ISP_Tuning_SetAeMin_t)dlsym(handle, "IMP_ISP_Tuning_SetAeMin");
	if (!IMP_ISP_Tuning_SetAeMin_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeMin_ptr = (IMP_ISP_Tuning_GetAeMin_t)dlsym(handle, "IMP_ISP_Tuning_GetAeMin");
	if (!IMP_ISP_Tuning_GetAeMin_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAe_IT_MAX_ptr = (IMP_ISP_Tuning_SetAe_IT_MAX_t)dlsym(handle, "IMP_ISP_Tuning_SetAe_IT_MAX");
	if (!IMP_ISP_Tuning_SetAe_IT_MAX_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAE_IT_MAX_ptr = (IMP_ISP_Tuning_GetAE_IT_MAX_t)dlsym(handle, "IMP_ISP_Tuning_GetAE_IT_MAX");
	if (!IMP_ISP_Tuning_GetAE_IT_MAX_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeZone_ptr = (IMP_ISP_Tuning_GetAeZone_t)dlsym(handle, "IMP_ISP_Tuning_GetAeZone");
	if (!IMP_ISP_Tuning_GetAeZone_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAeTargetList_ptr = (IMP_ISP_Tuning_SetAeTargetList_t)dlsym(handle, "IMP_ISP_Tuning_SetAeTargetList");
	if (!IMP_ISP_Tuning_SetAeTargetList_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeTargetList_ptr = (IMP_ISP_Tuning_GetAeTargetList_t)dlsym(handle, "IMP_ISP_Tuning_GetAeTargetList");
	if (!IMP_ISP_Tuning_GetAeTargetList_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetModuleControl_ptr = (IMP_ISP_Tuning_SetModuleControl_t)dlsym(handle, "IMP_ISP_Tuning_SetModuleControl");
	if (!IMP_ISP_Tuning_SetModuleControl_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetModuleControl_ptr = (IMP_ISP_Tuning_GetModuleControl_t)dlsym(handle, "IMP_ISP_Tuning_GetModuleControl");
	if (!IMP_ISP_Tuning_GetModuleControl_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetFrontCrop_ptr = (IMP_ISP_Tuning_SetFrontCrop_t)dlsym(handle, "IMP_ISP_Tuning_SetFrontCrop");
	if (!IMP_ISP_Tuning_SetFrontCrop_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetFrontCrop_ptr = (IMP_ISP_Tuning_GetFrontCrop_t)dlsym(handle, "IMP_ISP_Tuning_GetFrontCrop");
	if (!IMP_ISP_Tuning_GetFrontCrop_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_WDR_ENABLE_ptr = (IMP_ISP_WDR_ENABLE_t)dlsym(handle, "IMP_ISP_WDR_ENABLE");
	if (!IMP_ISP_WDR_ENABLE_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_WDR_ENABLE_Get_ptr = (IMP_ISP_WDR_ENABLE_Get_t)dlsym(handle, "IMP_ISP_WDR_ENABLE_Get");
	if (!IMP_ISP_WDR_ENABLE_Get_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetDPC_Strength_ptr = (IMP_ISP_Tuning_SetDPC_Strength_t)dlsym(handle, "IMP_ISP_Tuning_SetDPC_Strength");
	if (!IMP_ISP_Tuning_SetDPC_Strength_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetDPC_Strength_ptr = (IMP_ISP_Tuning_GetDPC_Strength_t)dlsym(handle, "IMP_ISP_Tuning_GetDPC_Strength");
	if (!IMP_ISP_Tuning_GetDPC_Strength_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetDRC_Strength_ptr = (IMP_ISP_Tuning_SetDRC_Strength_t)dlsym(handle, "IMP_ISP_Tuning_SetDRC_Strength");
	if (!IMP_ISP_Tuning_SetDRC_Strength_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetDRC_Strength_ptr = (IMP_ISP_Tuning_GetDRC_Strength_t)dlsym(handle, "IMP_ISP_Tuning_GetDRC_Strength");
	if (!IMP_ISP_Tuning_GetDRC_Strength_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetHVFLIP_ptr = (IMP_ISP_Tuning_SetHVFLIP_t)dlsym(handle, "IMP_ISP_Tuning_SetHVFLIP");
	if (!IMP_ISP_Tuning_SetHVFLIP_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetHVFlip_ptr = (IMP_ISP_Tuning_GetHVFlip_t)dlsym(handle, "IMP_ISP_Tuning_GetHVFlip");
	if (!IMP_ISP_Tuning_GetHVFlip_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetMask_ptr = (IMP_ISP_Tuning_SetMask_t)dlsym(handle, "IMP_ISP_Tuning_SetMask");
	if (!IMP_ISP_Tuning_SetMask_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetMask_ptr = (IMP_ISP_Tuning_GetMask_t)dlsym(handle, "IMP_ISP_Tuning_GetMask");
	if (!IMP_ISP_Tuning_GetMask_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetSensorAttr_ptr = (IMP_ISP_Tuning_GetSensorAttr_t)dlsym(handle, "IMP_ISP_Tuning_GetSensorAttr");
	if (!IMP_ISP_Tuning_GetSensorAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_EnableDRC_ptr = (IMP_ISP_Tuning_EnableDRC_t)dlsym(handle, "IMP_ISP_Tuning_EnableDRC");
	if (!IMP_ISP_Tuning_EnableDRC_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_EnableDefog_ptr = (IMP_ISP_Tuning_EnableDefog_t)dlsym(handle, "IMP_ISP_Tuning_EnableDefog");
	if (!IMP_ISP_Tuning_EnableDefog_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAwbCt_ptr = (IMP_ISP_Tuning_SetAwbCt_t)dlsym(handle, "IMP_ISP_Tuning_SetAwbCt");
	if (!IMP_ISP_Tuning_SetAwbCt_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAWBCt_ptr = (IMP_ISP_Tuning_GetAWBCt_t)dlsym(handle, "IMP_ISP_Tuning_GetAWBCt");
	if (!IMP_ISP_Tuning_GetAWBCt_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetCCMAttr_ptr = (IMP_ISP_Tuning_SetCCMAttr_t)dlsym(handle, "IMP_ISP_Tuning_SetCCMAttr");
	if (!IMP_ISP_Tuning_SetCCMAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetCCMAttr_ptr = (IMP_ISP_Tuning_GetCCMAttr_t)dlsym(handle, "IMP_ISP_Tuning_GetCCMAttr");
	if (!IMP_ISP_Tuning_GetCCMAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetAeAttr_ptr = (IMP_ISP_Tuning_SetAeAttr_t)dlsym(handle, "IMP_ISP_Tuning_SetAeAttr");
	if (!IMP_ISP_Tuning_SetAeAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeAttr_ptr = (IMP_ISP_Tuning_GetAeAttr_t)dlsym(handle, "IMP_ISP_Tuning_GetAeAttr");
	if (!IMP_ISP_Tuning_GetAeAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetAeState_ptr = (IMP_ISP_Tuning_GetAeState_t)dlsym(handle, "IMP_ISP_Tuning_GetAeState");
	if (!IMP_ISP_Tuning_GetAeState_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetScalerLv_ptr = (IMP_ISP_Tuning_SetScalerLv_t)dlsym(handle, "IMP_ISP_Tuning_SetScalerLv");
	if (!IMP_ISP_Tuning_SetScalerLv_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_SetAeAlgoFunc_ptr = (IMP_ISP_SetAeAlgoFunc_t)dlsym(handle, "IMP_ISP_SetAeAlgoFunc");
	if (!IMP_ISP_SetAeAlgoFunc_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_SetAwbAlgoFunc_ptr = (IMP_ISP_SetAwbAlgoFunc_t)dlsym(handle, "IMP_ISP_SetAwbAlgoFunc");
	if (!IMP_ISP_SetAwbAlgoFunc_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetBlcAttr_ptr = (IMP_ISP_Tuning_GetBlcAttr_t)dlsym(handle, "IMP_ISP_Tuning_GetBlcAttr");
	if (!IMP_ISP_Tuning_GetBlcAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetDefog_Strength_ptr = (IMP_ISP_Tuning_SetDefog_Strength_t)dlsym(handle, "IMP_ISP_Tuning_SetDefog_Strength");
	if (!IMP_ISP_Tuning_SetDefog_Strength_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetDefog_Strength_ptr = (IMP_ISP_Tuning_GetDefog_Strength_t)dlsym(handle, "IMP_ISP_Tuning_GetDefog_Strength");
	if (!IMP_ISP_Tuning_GetDefog_Strength_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetCsc_Attr_ptr = (IMP_ISP_Tuning_SetCsc_Attr_t)dlsym(handle, "IMP_ISP_Tuning_SetCsc_Attr");
	if (!IMP_ISP_Tuning_SetCsc_Attr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetCsc_Attr_ptr = (IMP_ISP_Tuning_GetCsc_Attr_t)dlsym(handle, "IMP_ISP_Tuning_GetCsc_Attr");
	if (!IMP_ISP_Tuning_GetCsc_Attr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_SetWdr_OutputMode_ptr = (IMP_ISP_Tuning_SetWdr_OutputMode_t)dlsym(handle, "IMP_ISP_Tuning_SetWdr_OutputMode");
	if (!IMP_ISP_Tuning_SetWdr_OutputMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_Tuning_GetWdr_OutputMode_ptr = (IMP_ISP_Tuning_GetWdr_OutputMode_t)dlsym(handle, "IMP_ISP_Tuning_GetWdr_OutputMode");
	if (!IMP_ISP_Tuning_GetWdr_OutputMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_SetFrameDrop_ptr = (IMP_ISP_SetFrameDrop_t)dlsym(handle, "IMP_ISP_SetFrameDrop");
	if (!IMP_ISP_SetFrameDrop_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_GetFrameDrop_ptr = (IMP_ISP_GetFrameDrop_t)dlsym(handle, "IMP_ISP_GetFrameDrop");
	if (!IMP_ISP_GetFrameDrop_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_SetFixedContraster_ptr = (IMP_ISP_SetFixedContraster_t)dlsym(handle, "IMP_ISP_SetFixedContraster");
	if (!IMP_ISP_SetFixedContraster_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_SET_GPIO_INIT_OR_FREE_ptr = (IMP_ISP_SET_GPIO_INIT_OR_FREE_t)dlsym(handle, "IMP_ISP_SET_GPIO_INIT_OR_FREE");
	if (!IMP_ISP_SET_GPIO_INIT_OR_FREE_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_ISP_SET_GPIO_STA_ptr = (IMP_ISP_SET_GPIO_STA_t)dlsym(handle, "IMP_ISP_SET_GPIO_STA");
	if (!IMP_ISP_SET_GPIO_STA_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_Init_ptr = (IMP_System_Init_t)dlsym(handle, "IMP_System_Init");
	if (!IMP_System_Init_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_Exit_ptr = (IMP_System_Exit_t)dlsym(handle, "IMP_System_Exit");
	if (!IMP_System_Exit_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_GetTimeStamp_ptr = (IMP_System_GetTimeStamp_t)dlsym(handle, "IMP_System_GetTimeStamp");
	if (!IMP_System_GetTimeStamp_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_RebaseTimeStamp_ptr = (IMP_System_RebaseTimeStamp_t)dlsym(handle, "IMP_System_RebaseTimeStamp");
	if (!IMP_System_RebaseTimeStamp_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_ReadReg32_ptr = (IMP_System_ReadReg32_t)dlsym(handle, "IMP_System_ReadReg32");
	if (!IMP_System_ReadReg32_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_WriteReg32_ptr = (IMP_System_WriteReg32_t)dlsym(handle, "IMP_System_WriteReg32");
	if (!IMP_System_WriteReg32_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_GetVersion_ptr = (IMP_System_GetVersion_t)dlsym(handle, "IMP_System_GetVersion");
	if (!IMP_System_GetVersion_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_Bind_ptr = (IMP_System_Bind_t)dlsym(handle, "IMP_System_Bind");
	if (!IMP_System_Bind_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_UnBind_ptr = (IMP_System_UnBind_t)dlsym(handle, "IMP_System_UnBind");
	if (!IMP_System_UnBind_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_GetBindbyDest_ptr = (IMP_System_GetBindbyDest_t)dlsym(handle, "IMP_System_GetBindbyDest");
	if (!IMP_System_GetBindbyDest_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_MemPoolRequest_ptr = (IMP_System_MemPoolRequest_t)dlsym(handle, "IMP_System_MemPoolRequest");
	if (!IMP_System_MemPoolRequest_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_System_MemPoolFree_ptr = (IMP_System_MemPoolFree_t)dlsym(handle, "IMP_System_MemPoolFree");
	if (!IMP_System_MemPoolFree_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_CreateGroup_ptr = (IMP_Encoder_CreateGroup_t)dlsym(handle, "IMP_Encoder_CreateGroup");
	if (!IMP_Encoder_CreateGroup_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_DestroyGroup_ptr = (IMP_Encoder_DestroyGroup_t)dlsym(handle, "IMP_Encoder_DestroyGroup");
	if (!IMP_Encoder_DestroyGroup_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetDefaultParam_ptr = (IMP_Encoder_SetDefaultParam_t)dlsym(handle, "IMP_Encoder_SetDefaultParam");
	if (!IMP_Encoder_SetDefaultParam_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_CreateChn_ptr = (IMP_Encoder_CreateChn_t)dlsym(handle, "IMP_Encoder_CreateChn");
	if (!IMP_Encoder_CreateChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_DestroyChn_ptr = (IMP_Encoder_DestroyChn_t)dlsym(handle, "IMP_Encoder_DestroyChn");
	if (!IMP_Encoder_DestroyChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetChnAttr_ptr = (IMP_Encoder_GetChnAttr_t)dlsym(handle, "IMP_Encoder_GetChnAttr");
	if (!IMP_Encoder_GetChnAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_RegisterChn_ptr = (IMP_Encoder_RegisterChn_t)dlsym(handle, "IMP_Encoder_RegisterChn");
	if (!IMP_Encoder_RegisterChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_UnRegisterChn_ptr = (IMP_Encoder_UnRegisterChn_t)dlsym(handle, "IMP_Encoder_UnRegisterChn");
	if (!IMP_Encoder_UnRegisterChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_StartRecvPic_ptr = (IMP_Encoder_StartRecvPic_t)dlsym(handle, "IMP_Encoder_StartRecvPic");
	if (!IMP_Encoder_StartRecvPic_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_StopRecvPic_ptr = (IMP_Encoder_StopRecvPic_t)dlsym(handle, "IMP_Encoder_StopRecvPic");
	if (!IMP_Encoder_StopRecvPic_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_Query_ptr = (IMP_Encoder_Query_t)dlsym(handle, "IMP_Encoder_Query");
	if (!IMP_Encoder_Query_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetStream_ptr = (IMP_Encoder_GetStream_t)dlsym(handle, "IMP_Encoder_GetStream");
	if (!IMP_Encoder_GetStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_ReleaseStream_ptr = (IMP_Encoder_ReleaseStream_t)dlsym(handle, "IMP_Encoder_ReleaseStream");
	if (!IMP_Encoder_ReleaseStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_PollingStream_ptr = (IMP_Encoder_PollingStream_t)dlsym(handle, "IMP_Encoder_PollingStream");
	if (!IMP_Encoder_PollingStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetFd_ptr = (IMP_Encoder_GetFd_t)dlsym(handle, "IMP_Encoder_GetFd");
	if (!IMP_Encoder_GetFd_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetbufshareChn_ptr = (IMP_Encoder_SetbufshareChn_t)dlsym(handle, "IMP_Encoder_SetbufshareChn");
	if (!IMP_Encoder_SetbufshareChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnResizeMode_ptr = (IMP_Encoder_SetChnResizeMode_t)dlsym(handle, "IMP_Encoder_SetChnResizeMode");
	if (!IMP_Encoder_SetChnResizeMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetMaxStreamCnt_ptr = (IMP_Encoder_SetMaxStreamCnt_t)dlsym(handle, "IMP_Encoder_SetMaxStreamCnt");
	if (!IMP_Encoder_SetMaxStreamCnt_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetMaxStreamCnt_ptr = (IMP_Encoder_GetMaxStreamCnt_t)dlsym(handle, "IMP_Encoder_GetMaxStreamCnt");
	if (!IMP_Encoder_GetMaxStreamCnt_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_RequestIDR_ptr = (IMP_Encoder_RequestIDR_t)dlsym(handle, "IMP_Encoder_RequestIDR");
	if (!IMP_Encoder_RequestIDR_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_FlushStream_ptr = (IMP_Encoder_FlushStream_t)dlsym(handle, "IMP_Encoder_FlushStream");
	if (!IMP_Encoder_FlushStream_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetChnFrmRate_ptr = (IMP_Encoder_GetChnFrmRate_t)dlsym(handle, "IMP_Encoder_GetChnFrmRate");
	if (!IMP_Encoder_GetChnFrmRate_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnFrmRate_ptr = (IMP_Encoder_SetChnFrmRate_t)dlsym(handle, "IMP_Encoder_SetChnFrmRate");
	if (!IMP_Encoder_SetChnFrmRate_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnBitRate_ptr = (IMP_Encoder_SetChnBitRate_t)dlsym(handle, "IMP_Encoder_SetChnBitRate");
	if (!IMP_Encoder_SetChnBitRate_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnGopLength_ptr = (IMP_Encoder_SetChnGopLength_t)dlsym(handle, "IMP_Encoder_SetChnGopLength");
	if (!IMP_Encoder_SetChnGopLength_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetChnAttrRcMode_ptr = (IMP_Encoder_GetChnAttrRcMode_t)dlsym(handle, "IMP_Encoder_GetChnAttrRcMode");
	if (!IMP_Encoder_GetChnAttrRcMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnAttrRcMode_ptr = (IMP_Encoder_SetChnAttrRcMode_t)dlsym(handle, "IMP_Encoder_SetChnAttrRcMode");
	if (!IMP_Encoder_SetChnAttrRcMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetChnGopAttr_ptr = (IMP_Encoder_GetChnGopAttr_t)dlsym(handle, "IMP_Encoder_GetChnGopAttr");
	if (!IMP_Encoder_GetChnGopAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnGopAttr_ptr = (IMP_Encoder_SetChnGopAttr_t)dlsym(handle, "IMP_Encoder_SetChnGopAttr");
	if (!IMP_Encoder_SetChnGopAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnQp_ptr = (IMP_Encoder_SetChnQp_t)dlsym(handle, "IMP_Encoder_SetChnQp");
	if (!IMP_Encoder_SetChnQp_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnQpBounds_ptr = (IMP_Encoder_SetChnQpBounds_t)dlsym(handle, "IMP_Encoder_SetChnQpBounds");
	if (!IMP_Encoder_SetChnQpBounds_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnQpIPDelta_ptr = (IMP_Encoder_SetChnQpIPDelta_t)dlsym(handle, "IMP_Encoder_SetChnQpIPDelta");
	if (!IMP_Encoder_SetChnQpIPDelta_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetFisheyeEnableStatus_ptr = (IMP_Encoder_SetFisheyeEnableStatus_t)dlsym(handle, "IMP_Encoder_SetFisheyeEnableStatus");
	if (!IMP_Encoder_SetFisheyeEnableStatus_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetFisheyeEnableStatus_ptr = (IMP_Encoder_GetFisheyeEnableStatus_t)dlsym(handle, "IMP_Encoder_GetFisheyeEnableStatus");
	if (!IMP_Encoder_GetFisheyeEnableStatus_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetChnEncType_ptr = (IMP_Encoder_GetChnEncType_t)dlsym(handle, "IMP_Encoder_GetChnEncType");
	if (!IMP_Encoder_GetChnEncType_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetPool_ptr = (IMP_Encoder_SetPool_t)dlsym(handle, "IMP_Encoder_SetPool");
	if (!IMP_Encoder_SetPool_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetPool_ptr = (IMP_Encoder_GetPool_t)dlsym(handle, "IMP_Encoder_GetPool");
	if (!IMP_Encoder_GetPool_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetStreamBufSize_ptr = (IMP_Encoder_SetStreamBufSize_t)dlsym(handle, "IMP_Encoder_SetStreamBufSize");
	if (!IMP_Encoder_SetStreamBufSize_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetStreamBufSize_ptr = (IMP_Encoder_GetStreamBufSize_t)dlsym(handle, "IMP_Encoder_GetStreamBufSize");
	if (!IMP_Encoder_GetStreamBufSize_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetChnAveBitrate_ptr = (IMP_Encoder_GetChnAveBitrate_t)dlsym(handle, "IMP_Encoder_GetChnAveBitrate");
	if (!IMP_Encoder_GetChnAveBitrate_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_GetChnEvalInfo_ptr = (IMP_Encoder_GetChnEvalInfo_t)dlsym(handle, "IMP_Encoder_GetChnEvalInfo");
	if (!IMP_Encoder_GetChnEvalInfo_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetChnEntropyMode_ptr = (IMP_Encoder_SetChnEntropyMode_t)dlsym(handle, "IMP_Encoder_SetChnEntropyMode");
	if (!IMP_Encoder_SetChnEntropyMode_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Encoder_SetFrameRelease_ptr = (IMP_Encoder_SetFrameRelease_t)dlsym(handle, "IMP_Encoder_SetFrameRelease");
	if (!IMP_Encoder_SetFrameRelease_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	/**IMP_IVS_CreateMoveInterface_ptr = (IMP_IVS_CreateMoveInterface_t)dlsym(handle, "*IMP_IVS_CreateMoveInterface");
	if (!*IMP_IVS_CreateMoveInterface_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}*/
	IMP_IVS_DestroyMoveInterface_ptr = (IMP_IVS_DestroyMoveInterface_t)dlsym(handle, "IMP_IVS_DestroyMoveInterface");
	if (!IMP_IVS_DestroyMoveInterface_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	/*imp_log_fun_ptr = (imp_log_fun_t)dlsym(handle, "imp_log_fun");
	if (!imp_log_fun_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Log_Set_Option_ptr = (IMP_Log_Set_Option_t)dlsym(handle, "IMP_Log_Set_Option");
	if (!IMP_Log_Set_Option_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_Log_Get_Option_ptr = (IMP_Log_Get_Option_t)dlsym(handle, "IMP_Log_Get_Option");
	if (!IMP_Log_Get_Option_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}*/
	IMP_FrameSource_CreateChn_ptr = (IMP_FrameSource_CreateChn_t)dlsym(handle, "IMP_FrameSource_CreateChn");
	if (!IMP_FrameSource_CreateChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_DestroyChn_ptr = (IMP_FrameSource_DestroyChn_t)dlsym(handle, "IMP_FrameSource_DestroyChn");
	if (!IMP_FrameSource_DestroyChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_EnableChn_ptr = (IMP_FrameSource_EnableChn_t)dlsym(handle, "IMP_FrameSource_EnableChn");
	if (!IMP_FrameSource_EnableChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_DisableChn_ptr = (IMP_FrameSource_DisableChn_t)dlsym(handle, "IMP_FrameSource_DisableChn");
	if (!IMP_FrameSource_DisableChn_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_SetSource_ptr = (IMP_FrameSource_SetSource_t)dlsym(handle, "IMP_FrameSource_SetSource");
	if (!IMP_FrameSource_SetSource_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_GetChnAttr_ptr = (IMP_FrameSource_GetChnAttr_t)dlsym(handle, "IMP_FrameSource_GetChnAttr");
	if (!IMP_FrameSource_GetChnAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_SetChnAttr_ptr = (IMP_FrameSource_SetChnAttr_t)dlsym(handle, "IMP_FrameSource_SetChnAttr");
	if (!IMP_FrameSource_SetChnAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_SetFrameDepth_ptr = (IMP_FrameSource_SetFrameDepth_t)dlsym(handle, "IMP_FrameSource_SetFrameDepth");
	if (!IMP_FrameSource_SetFrameDepth_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_GetFrameDepth_ptr = (IMP_FrameSource_GetFrameDepth_t)dlsym(handle, "IMP_FrameSource_GetFrameDepth");
	if (!IMP_FrameSource_GetFrameDepth_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_GetFrame_ptr = (IMP_FrameSource_GetFrame_t)dlsym(handle, "IMP_FrameSource_GetFrame");
	if (!IMP_FrameSource_GetFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_GetTimedFrame_ptr = (IMP_FrameSource_GetTimedFrame_t)dlsym(handle, "IMP_FrameSource_GetTimedFrame");
	if (!IMP_FrameSource_GetTimedFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_ReleaseFrame_ptr = (IMP_FrameSource_ReleaseFrame_t)dlsym(handle, "IMP_FrameSource_ReleaseFrame");
	if (!IMP_FrameSource_ReleaseFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_SnapFrame_ptr = (IMP_FrameSource_SnapFrame_t)dlsym(handle, "IMP_FrameSource_SnapFrame");
	if (!IMP_FrameSource_SnapFrame_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_SetMaxDelay_ptr = (IMP_FrameSource_SetMaxDelay_t)dlsym(handle, "IMP_FrameSource_SetMaxDelay");
	if (!IMP_FrameSource_SetMaxDelay_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_GetMaxDelay_ptr = (IMP_FrameSource_GetMaxDelay_t)dlsym(handle, "IMP_FrameSource_GetMaxDelay");
	if (!IMP_FrameSource_GetMaxDelay_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_SetDelay_ptr = (IMP_FrameSource_SetDelay_t)dlsym(handle, "IMP_FrameSource_SetDelay");
	if (!IMP_FrameSource_SetDelay_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_GetDelay_ptr = (IMP_FrameSource_GetDelay_t)dlsym(handle, "IMP_FrameSource_GetDelay");
	if (!IMP_FrameSource_GetDelay_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_SetChnFifoAttr_ptr = (IMP_FrameSource_SetChnFifoAttr_t)dlsym(handle, "IMP_FrameSource_SetChnFifoAttr");
	if (!IMP_FrameSource_SetChnFifoAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_GetChnFifoAttr_ptr = (IMP_FrameSource_GetChnFifoAttr_t)dlsym(handle, "IMP_FrameSource_GetChnFifoAttr");
	if (!IMP_FrameSource_GetChnFifoAttr_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_SetPool_ptr = (IMP_FrameSource_SetPool_t)dlsym(handle, "IMP_FrameSource_SetPool");
	if (!IMP_FrameSource_SetPool_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_GetPool_ptr = (IMP_FrameSource_GetPool_t)dlsym(handle, "IMP_FrameSource_GetPool");
	if (!IMP_FrameSource_GetPool_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_ChnStatQuery_ptr = (IMP_FrameSource_ChnStatQuery_t)dlsym(handle, "IMP_FrameSource_ChnStatQuery");
	if (!IMP_FrameSource_ChnStatQuery_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
	IMP_FrameSource_SetChnRotate_ptr = (IMP_FrameSource_SetChnRotate_t)dlsym(handle, "IMP_FrameSource_SetChnRotate");
	if (!IMP_FrameSource_SetChnRotate_ptr) {
		fprintf(stderr, "Error loading symbol: %s\n", dlerror());
	}
}
void unload_library() {
	if (handle) dlclose(handle);
}
