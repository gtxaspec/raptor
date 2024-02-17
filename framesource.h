int framesource_streamon();
int framesource_streamoff();

int framesource_init();
int framesource_exit();

typedef struct frame_source {
	int id;
	int group;
	int pixel_format;
	char pixel_format_name[255];
	int frame_rate_numerator;
	int frame_rate_denominator;
	int buffer_size;
	int channel_type;
	int crop_enable;
	int crop_top;
	int crop_left;
	int crop_width;
	int crop_height;
	int scaling_enable;
	int scaling_width;
	int scaling_height;
	int pic_width;
	int pic_height;

	IMPFSChnAttr imp_fs_attrs;

} FrameSource;

