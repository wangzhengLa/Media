#ifndef ALLWINNER_ENCODER_HANDLE_H_
#define ALLWINNER_ENCODER_HANDLE_H_


#include "memoryAdapter.h"
#include "vencoder.h"
#include "video_common_defines.h"


#define ROI_NUM 4
#define ALIGN_XXB(y, x) (((x) + ((y) - 1)) & ~((y) - 1))


namespace ecovacs
{
	typedef struct
	{
		EXIFInfo exifinfo;
		int quality;
		int jpeg_mode;
		VencJpegVideoSignal vs;
		int jpeg_biteRate;
		int jpeg_frameRate;
		VencBitRateRange bitRateRange;
		VencOverlayInfoS sOverlayInfo;
		VencCopyROIConfig pRoiConfig;
	} jpeg_func_t;


	typedef struct
	{
		VencHeaderData sps_pps_data;
		VencH264Param h264Param;
		VencMBModeCtrl h264MBMode;
		VencMBInfo MBInfo;
		VencFixQP fixQP;
		VencSuperFrameConfig sSuperFrameCfg;
		VencH264SVCSkip SVCSkip; // set SVC and skip_frame
		VencH264AspectRatio sAspectRatio;
		VencH264VideoSignal sVideoSignal;
		VencCyclicIntraRefresh sIntraRefresh;
		VencROIConfig sRoiConfig[ROI_NUM];
		VeProcSet sVeProcInfo;
		VencOverlayInfoS sOverlayInfo;
		VencSmartFun sH264Smart;
	} h264_func_t;


	typedef struct
	{
		VencH265Param h265Param;
		VencH265GopStruct h265Gop;
		VencHVS h265Hvs;
		VencH265TendRatioCoef h265Trc;
		VencSmartFun h265Smart;
		VencMBModeCtrl h265MBMode;
		VencMBInfo MBInfo;
		VencFixQP fixQP;
		VencSuperFrameConfig sSuperFrameCfg;
		VencH264SVCSkip SVCSkip; // set SVC and skip_frame
		VencH264AspectRatio sAspectRatio;
		VencH264VideoSignal sVideoSignal;
		VencCyclicIntraRefresh sIntraRefresh;
		VencROIConfig sRoiConfig[ROI_NUM];
		VencAlterFrameRateInfo sAlterFrameRateInfo;
		int h265_rc_frame_total;
		VeProcSet sVeProcInfo;
		VencOverlayInfoS sOverlayInfo;
	} h265_func_t;


	typedef struct
	{
		char intput_file[256];
		char output_file[256];
		char reference_file[256];
		char overlay_file[256];
		char log_file[256];
		int compare_flag;
		int log_flag;
		int compare_result;


		unsigned int encode_frame_num;
		unsigned int encode_format;


		unsigned int src_size;
		unsigned int dst_size;


		unsigned int src_width;
		unsigned int src_height;
		unsigned int dst_width;
		unsigned int dst_height;


		int frequency;
		int bit_rate;
		int frame_rate;
		int maxKeyFrame;
		unsigned int test_cycle;
		unsigned int test_overlay_flag;
		unsigned int test_lbc;
		unsigned int test_afbc;
		unsigned int limit_encode_speed; //* limit encoder speed by framerate
		unsigned int qp_min;
		unsigned int qp_max;


		jpeg_func_t jpeg_func;
		h264_func_t h264_func;
		h265_func_t h265_func;
		unsigned int nChannel;
		VencCopyROIConfig pRoiConfig;
		video_common::VideoCodecConfig codec_settings;
	} encode_param_t;


	typedef struct
	{
		unsigned int width;
		unsigned int height;
		unsigned int width_aligh16;
		unsigned int height_aligh16;
		unsigned char *argb_addr;
		unsigned int size;
	} BitMapInfoS;


	struct MpiEncHandle
	{
		MpiEncHandle(){};
		~MpiEncHandle(){};


		int32_t allwinner_init(const video_common::VideoCodecConfig *codec_settings);


		int32_t allwinner_encode(const video_common::VideoFrameBuffer *frame_buffer, uint8_t **out_buf, uint32_t *out_buf_len);


		void allwinner_request_idr();


		int32_t allwinner_adjust_bitrate(uint32_t bitrate);


		void allwinner_uninit();


		VencBaseConfig baseConfig;
		VencAllocateBufferParam bufferParam;
		VideoEncoder *pVideoEnc;
		VencInputBuffer inputBuffer;
		VencOutputBuffer outputBuffer;
		VencHeaderData sps_pps_data;
		VencMBSumInfo sMbSumInfo;
		encode_param_t encode_param;
	};


} // namespace ecovacs


#endif // ALLWINNER_ENCODER_HANDLE_H_

