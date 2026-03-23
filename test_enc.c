/* Minimal encoder test — bypass HAL, call SDK directly */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <imp/imp_system.h>
#include <imp/imp_common.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
#include <imp/imp_isp.h>
#include <imp/imp_osd.h>

int main(void)
{
	IMPSensorInfo sinfo;
	IMPFSChnAttr fsattr;
	IMPEncoderChnAttr encattr;
	IMPCell fs = {DEV_ID_FS, 0, 0};
	IMPCell enc = {DEV_ID_ENC, 0, 0};
	int ret;

	/* Sensor */
	memset(&sinfo, 0, sizeof(sinfo));
	strcpy(sinfo.name, "gc4653");
	sinfo.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
	strcpy(sinfo.i2c.type, "gc4653");
	sinfo.i2c.addr = 0x29;
	sinfo.rst_gpio = -1;
	sinfo.pwdn_gpio = -1;

	printf("ISP_Open\n");
	ret = IMP_ISP_Open();
	printf("  ret=%d\n", ret);

	printf("ISP_AddSensor\n");
	ret = IMP_ISP_AddSensor(&sinfo);
	printf("  ret=%d\n", ret);

	printf("ISP_EnableSensor\n");
	ret = IMP_ISP_EnableSensor();
	printf("  ret=%d\n", ret);

	printf("OSD_SetPoolSize\n");
	IMP_OSD_SetPoolSize(512 * 1024);

	printf("System_Init\n");
	ret = IMP_System_Init();
	printf("  ret=%d\n", ret);

	printf("ISP_EnableTuning\n");
	ret = IMP_ISP_EnableTuning();
	printf("  ret=%d\n", ret);

	printf("ISP_SetSensorFPS\n");
	ret = IMP_ISP_Tuning_SetSensorFPS(25, 1);
	printf("  ret=%d\n", ret);

	/* Framesource */
	memset(&fsattr, 0, sizeof(fsattr));
	fsattr.picWidth = 2560;
	fsattr.picHeight = 1440;
	fsattr.pixFmt = PIX_FMT_NV12;
	fsattr.outFrmRateNum = 25;
	fsattr.outFrmRateDen = 1;
	fsattr.nrVBs = 2;
	fsattr.type = FS_PHY_CHANNEL;

	printf("FS_CreateChn\n");
	ret = IMP_FrameSource_CreateChn(0, &fsattr);
	printf("  ret=%d\n", ret);

	printf("FS_SetChnAttr\n");
	ret = IMP_FrameSource_SetChnAttr(0, &fsattr);
	printf("  ret=%d\n", ret);

	/* Encoder */
	memset(&encattr, 0, sizeof(encattr));
	ret = IMP_Encoder_SetDefaultParam(&encattr, IMP_ENC_PROFILE_AVC_HIGH, IMP_ENC_RC_MODE_CBR,
					  2560, 1440, 25, 1, 30, /* gop */
					  2,			 /* maxSameSenceCnt */
					  -1,			 /* initQP */
					  3000			 /* bitrate in kbps */
	);
	printf("SetDefaultParam ret=%d\n", ret);

	/* gopAttr already set by SetDefaultParam — don't override uGopCtrlMode */

	printf("Enc_CreateGroup\n");
	ret = IMP_Encoder_CreateGroup(0);
	printf("  ret=%d\n", ret);

	printf("Enc_CreateChn\n");
	ret = IMP_Encoder_CreateChn(0, &encattr);
	printf("  ret=%d\n", ret);

	printf("Enc_RegisterChn\n");
	ret = IMP_Encoder_RegisterChn(0, 0);
	printf("  ret=%d\n", ret);

	/* Bind */
	printf("System_Bind FS->ENC\n");
	ret = IMP_System_Bind(&fs, &enc);
	printf("  ret=%d\n", ret);

	/* Enable */
	printf("FS_EnableChn\n");
	ret = IMP_FrameSource_EnableChn(0);
	printf("  ret=%d\n", ret);

	/* Start encoder */
	printf("Enc_StartRecvPic\n");
	ret = IMP_Encoder_StartRecvPic(0);
	printf("  ret=%d\n", ret);

	/* Poll */
	printf("Polling...\n");
	for (int i = 0; i < 10; i++) {
		ret = IMP_Encoder_PollingStream(0, 1000);
		printf("  poll[%d] ret=%d\n", i, ret);
		if (ret == 0) {
			IMPEncoderStream stream;
			ret = IMP_Encoder_GetStream(0, &stream, 1);
			printf("  GetStream ret=%d packs=%d\n", ret,
			       ret == 0 ? stream.packCount : -1);
			if (ret == 0)
				IMP_Encoder_ReleaseStream(0, &stream);
			break;
		}
	}

	/* Cleanup */
	IMP_Encoder_StopRecvPic(0);
	IMP_FrameSource_DisableChn(0);
	IMP_System_UnBind(&fs, &enc);
	IMP_Encoder_UnRegisterChn(0);
	IMP_Encoder_DestroyChn(0);
	IMP_Encoder_DestroyGroup(0);
	IMP_FrameSource_DestroyChn(0);
	IMP_System_Exit();
	IMP_ISP_DisableTuning();
	IMP_ISP_DisableSensor();
	IMP_ISP_DelSensor(&sinfo);
	IMP_ISP_Close();

	printf("done\n");
	return 0;
}
