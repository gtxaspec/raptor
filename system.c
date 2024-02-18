#include <stdlib.h>
#include <string.h>
#include <imp/imp_log.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#include "encoder.h"
#include "system.h"
#include "tcp.h"
#include "config.h"
#include "framesource.h"

#define TAG "encoder"

extern struct chn_conf chn[];

IMPSensorInfo sensor_info;

int system_initalize()
{
	int i, ret;

	/* Step.1 System init */
	ret = isp_init();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_System_Init() failed\n");
		return -1;
	}

 // update conf before framesource init... temp
 for (i = 0; i <  FS_CHN_NUM; i++) {
		if (chn[i].enable) {
		chn[i].fs_chn_attr.picWidth = config.sensor_1_width;
		chn[i].fs_chn_attr.picHeight = config.sensor_1_height;
		chn[i].fs_chn_attr.crop.width = config.sensor_1_width;
		chn[i].fs_chn_attr.crop.height = config.sensor_1_height;
		chn[i].fs_chn_attr.outFrmRateNum = config.sensor_1_fps;
		}
 }

	/* Step.2 FrameSource init */
	ret = framesource_init();
	//setup_framesource();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "FrameSource init failed\n");
		return -1;
	}

	/* Step.3 Encoder init */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_Encoder_CreateGroup(chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_CreateGroup(%d) error !\n", chn[i].index);
				return -1;
			}
		}
	}

	ret = encoder_init();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Encoder init failed\n");
		return -1;
	}

	/* Step.4 Bind */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_Bind(&chn[i].framesource_chn, &chn[i].imp_encoder);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "Bind FrameSource channel%d and Encoder failed\n",i);
				return -1;
			}
		}
	}

	/* Step.5 Stream On */
	ret = framesource_streamon();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "ImpStreamOn failed\n");
		return -1;
	}

	/*// Run TCP
	ret = setup_tcp();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "TCP failed\n");
		return -1;
	}*/

	pthread_t thread_id, fifo_thread;
	int video_channel = 0; // Example video channel ID

	// Start the video feeder thread
	if(pthread_create(&thread_id, NULL, video_feeder_thread, &video_channel) != 0) {
		perror("Failed to create video feeder thread");
		return -1;
	}

	const char *fifoPath = "/tmp/h264_fifo";

	// Start FIFO writer thread
	if (pthread_create(&fifo_thread, NULL, fifo_writer_thread, (void *)fifoPath)) {
		perror("Could not create FIFO writer thread");
		return -1;
	}

	// Run TCP server setup
	ret = setup_tcp();
	if (ret < 0) {
	IMP_LOG_ERR(TAG, "TCP failed\n");
	return -1;
	}

	// Cleanup
	pthread_join(thread_id, NULL); // Wait for the feeder thread to finish
	pthread_join(fifo_thread, NULL); // Wait for the FIFO writer thread to finish
	return 0;

	/* Step.a Stream Off */
	ret = framesource_streamoff();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "FrameSource StreamOff failed\n");
		return -1;
	}

	/* Step.b UnBind */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_UnBind(&chn[i].framesource_chn, &chn[i].imp_encoder);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "UnBind FrameSource channel%d and Encoder failed\n",i);
				return -1;
			}
		}
	}

	/* Step.c Encoder exit */
	ret = encoder_exit();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Encoder exit failed\n");
		return -1;
	}

	/* Step.d FrameSource exit */
	ret = framesource_exit();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "FrameSource exit failed\n");
		return -1;
	}

	/* Step.e System exit */
	ret = isp_exit();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "sample_system_exit() failed\n");
		return -1;
	}

	return 0;
}

int isp_init()
{
	int ret = 0;

/*
 *    IMP_System_MemPoolRequest(0, 12 * ( 1 << 20 ), "mempool0");
 *    IMP_System_MemPoolRequest(1, 10 * ( 1 << 20 ), "mempool1");
 *
 *    IMP_Encoder_SetPool(0, 0);
 *    IMP_Encoder_SetPool(1, 1);
 *
 *    IMP_FrameSource_SetPool(0, 0);
 *    IMP_FrameSource_SetPool(1, 1);
 *
 *    IMP_System_MemPoolFree(1);
 *    IMP_System_MemPoolFree(0);
 */

	memset(&sensor_info, 0, sizeof(IMPSensorInfo));

	// Ensure config.sensor_name is null-terminated and does not exceed the size of sensor_info.name
	strncpy(sensor_info.name, config.sensor_1_name, sizeof(sensor_info.name) - 1);
	// Explicitly null-terminate to avoid string overflow issues
	sensor_info.name[sizeof(sensor_info.name) - 1] = '\0';

	//sensor_info.cbus_type = SENSOR_CUBS_TYPE;
	if (strcmp(config.sensor_1_bus, "i2c") == 0) {
		sensor_info.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
	} else if (strcmp(config.sensor_1_bus, "spi") == 0) {
		sensor_info.cbus_type = TX_SENSOR_CONTROL_INTERFACE_SPI;
	} else {
		printf("unsupported bus type\n");
	}

	strncpy(sensor_info.i2c.type, config.sensor_1_name, sizeof(sensor_info.i2c.type) - 1);
	sensor_info.i2c.type[sizeof(sensor_info.i2c.type) - 1] = '\0';

	sensor_info.i2c.addr = config.sensor_1_i2c_address;

	//sensor_info.i2c.addr = SENSOR_I2C_ADDR;

	IMP_LOG_DBG(TAG, "sample_system_init start\n");

	ret = IMP_ISP_Open();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to open ISP\n");
		return -1;
	}

	ret = IMP_ISP_AddSensor(&sensor_info);
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to AddSensor\n");
		return -1;
	}

	ret = IMP_ISP_EnableSensor();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to EnableSensor\n");
		return -1;
	}

	ret = IMP_System_Init();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "IMP_System_Init failed\n");
		return -1;
	}

	/* enable turning, to debug graphics */
	ret = IMP_ISP_EnableTuning();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "IMP_ISP_EnableTuning failed\n");
		return -1;
	}
    IMP_ISP_Tuning_SetContrast(128);
    IMP_ISP_Tuning_SetSharpness(128);
    IMP_ISP_Tuning_SetSaturation(128);
    IMP_ISP_Tuning_SetBrightness(128);
#if 1
    ret = IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_DAY);
    if (ret < 0){
        IMP_LOG_ERR(TAG, "failed to set running mode\n");
        return -1;
    }
#endif
#if 0 // SENSOR FPS
    ret = IMP_ISP_Tuning_SetSensorFPS(SENSOR_FRAME_RATE_NUM, SENSOR_FRAME_RATE_DEN);
    if (ret < 0){
        IMP_LOG_ERR(TAG, "failed to set sensor fps\n");
        return -1;
    }
#endif
	IMP_LOG_DBG(TAG, "ImpSystemInit success\n");

	return 0;
}

int isp_exit()
{
	int ret = 0;

	IMP_LOG_DBG(TAG, "sample_system_exit start\n");

	IMP_System_Exit();

	ret = IMP_ISP_DisableSensor();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to EnableSensor\n");
		return -1;
	}

	ret = IMP_ISP_DelSensor(&sensor_info);
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to AddSensor\n");
		return -1;
	}

	ret = IMP_ISP_DisableTuning();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "IMP_ISP_DisableTuning failed\n");
		return -1;
	}

	if(IMP_ISP_Close()){
		IMP_LOG_ERR(TAG, "failed to open ISP\n");
		return -1;
	}

	IMP_LOG_DBG(TAG, " sample_system_exit success\n");

	return 0;
}

