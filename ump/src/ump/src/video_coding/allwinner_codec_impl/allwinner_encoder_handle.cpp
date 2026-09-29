#include "allwinner_encoder_handle.h"
#include "EncAdapter.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "osp.h"
#include "log.h"

#define CEDARC_UNUSE(param) (void)param // just for remove compile warning

namespace ecovacs
{
	BitMapInfoS bit_map_info[13]; //= {{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0}};

	static long long GetNowUs()
	{
		struct timeval now;
		gettimeofday(&now, NULL);
		return now.tv_sec * 1000000 + now.tv_usec;
	}

	static void init_jpeg_exif(EXIFInfo *exifinfo)
	{
		exifinfo->ThumbWidth = 640;
		exifinfo->ThumbHeight = 480;

		strcpy((char *)exifinfo->CameraMake, "allwinner make test");
		strcpy((char *)exifinfo->CameraModel, "allwinner model test");
		strcpy((char *)exifinfo->DateTime, "2014:02:21 10:54:05");
		strcpy((char *)exifinfo->gpsProcessingMethod, "allwinner gps");

		exifinfo->Orientation = 0;

		exifinfo->ExposureTime.num = 2;
		exifinfo->ExposureTime.den = 1000;

		exifinfo->FNumber.num = 20;
		exifinfo->FNumber.den = 10;
		exifinfo->ISOSpeed = 50;

		exifinfo->ExposureBiasValue.num = -4;
		exifinfo->ExposureBiasValue.den = 1;

		exifinfo->MeteringMode = 1;
		exifinfo->FlashUsed = 0;

		exifinfo->FocalLength.num = 1400;
		exifinfo->FocalLength.den = 100;

		exifinfo->DigitalZoomRatio.num = 4;
		exifinfo->DigitalZoomRatio.den = 1;

		exifinfo->WhiteBalance = 1;
		exifinfo->ExposureMode = 1;

		exifinfo->enableGpsInfo = 1;

		exifinfo->gps_latitude = 23.2368;
		exifinfo->gps_longitude = 24.3244;
		exifinfo->gps_altitude = 1234.5;

		exifinfo->gps_timestamp = (long)time(NULL);

		strcpy((char *)exifinfo->CameraSerialNum, "123456789");
		strcpy((char *)exifinfo->ImageName, "exif-name-test");
		strcpy((char *)exifinfo->ImageDescription, "exif-descriptor-test");
	}

	static void init_h265_gop(VencH265GopStruct *h265Gop)
	{
		h265Gop->gop_size = 8;
		h265Gop->intra_period = 16;

		h265Gop->use_lt_ref_flag = 1;
		if (h265Gop->use_lt_ref_flag)
		{
			h265Gop->max_num_ref_pics = 2;
			h265Gop->num_ref_idx_l0_default_active = 2;
			h265Gop->num_ref_idx_l1_default_active = 2;
			h265Gop->use_sps_rps_flag = 0;
		}
		else
		{
			h265Gop->max_num_ref_pics = 1;
			h265Gop->num_ref_idx_l0_default_active = 1;
			h265Gop->num_ref_idx_l1_default_active = 1;
			h265Gop->use_sps_rps_flag = 1;
		}
		// 1:user config the reference info; 0:encoder config the reference info
		h265Gop->custom_rps_flag = 0;
	}

	static void init_mb_mode(VencMBModeCtrl *pMBMode, encode_param_t *encode_param)
	{
		unsigned int mb_num;
		// unsigned int j;

		mb_num = (ALIGN_XXB(16, encode_param->dst_width) >> 4) * (ALIGN_XXB(16, encode_param->dst_height) >> 4);
		pMBMode->p_info = (unsigned char *)malloc(sizeof(VencMBModeCtrlInfo) * mb_num);
		if (pMBMode->p_info == NULL)
		{
			ump_error("malloc fail!");
			return;
		}

		pMBMode->mode_ctrl_en = 1;

#if 0
    for (j = 0; j < mb_num / 2; j++)
    {
        pMBMode->p_info[j].mb_en = 1;
        pMBMode->p_info[j].mb_skip_flag = 0;
        pMBMode->p_info[j].mb_qp = 22;
    }
    for (; j < mb_num; j++)
    {
        pMBMode->p_info[j].mb_en = 1;
        pMBMode->p_info[j].mb_skip_flag = 0;
        pMBMode->p_info[j].mb_qp = 32;
    }
#endif
	}

	static void init_mb_info(VencMBInfo *MBInfo, encode_param_t *encode_param)
	{
		if (encode_param->encode_format == VENC_CODEC_H265)
		{
			MBInfo->num_mb = (ALIGN_XXB(32, encode_param->dst_width) *
							  ALIGN_XXB(32, encode_param->dst_height)) >>
							 10;
		}
		else
		{
			MBInfo->num_mb = (ALIGN_XXB(16, encode_param->dst_width) *
							  ALIGN_XXB(16, encode_param->dst_height)) >>
							 8;
		}
		MBInfo->p_para = (VencMBInfoPara *)malloc(sizeof(VencMBInfoPara) * MBInfo->num_mb);
		if (MBInfo->p_para == NULL)
		{
			ump_error("malloc MBInfo->p_para error\n");
			return;
		}
		ump_info("mb_num:%d, mb_info_queue_addr:%p\n", MBInfo->num_mb, MBInfo->p_para);
	}

	static void init_fix_qp(VencFixQP *fixQP)
	{
		fixQP->bEnable = 1;
		fixQP->nIQp = 35;
		fixQP->nPQp = 35;
	}

	static void init_super_frame_cfg(VencSuperFrameConfig *sSuperFrameCfg)
	{
		sSuperFrameCfg->eSuperFrameMode = VENC_SUPERFRAME_NONE;
		sSuperFrameCfg->nMaxIFrameBits = 30000 * 8;
		sSuperFrameCfg->nMaxPFrameBits = 15000 * 8;
	}

	static void init_svc_skip(VencH264SVCSkip *SVCSkip)
	{
		SVCSkip->nTemporalSVC = T_LAYER_4;
		switch (SVCSkip->nTemporalSVC)
		{
		case T_LAYER_4:
			SVCSkip->nSkipFrame = SKIP_8;
			break;
		case T_LAYER_3:
			SVCSkip->nSkipFrame = SKIP_4;
			break;
		case T_LAYER_2:
			SVCSkip->nSkipFrame = SKIP_2;
			break;
		default:
			SVCSkip->nSkipFrame = NO_SKIP;
			break;
		}
	}

	static void init_aspect_ratio(VencH264AspectRatio *sAspectRatio)
	{
		sAspectRatio->aspect_ratio_idc = 255;
		sAspectRatio->sar_width = 4;
		sAspectRatio->sar_height = 3;
	}

	static void init_video_signal(VencH264VideoSignal *sVideoSignal)
	{
		sVideoSignal->video_format = (VENC_VIDEO_FORMAT)5;
		sVideoSignal->src_colour_primaries = (VENC_COLOR_SPACE)0;
		sVideoSignal->dst_colour_primaries = (VENC_COLOR_SPACE)1;
	}

	static void init_intra_refresh(VencCyclicIntraRefresh *sIntraRefresh)
	{
		sIntraRefresh->bEnable = 1;
		sIntraRefresh->nBlockNumber = 10;
	}

	static void init_roi(VencROIConfig *sRoiConfig)
	{
		sRoiConfig[0].bEnable = 1;
		sRoiConfig[0].index = 0;
		sRoiConfig[0].nQPoffset = 10;
		sRoiConfig[0].sRect.nLeft = 0;
		sRoiConfig[0].sRect.nTop = 0;
		sRoiConfig[0].sRect.nWidth = 1280;
		sRoiConfig[0].sRect.nHeight = 320;

		sRoiConfig[1].bEnable = 1;
		sRoiConfig[1].index = 1;
		sRoiConfig[1].nQPoffset = 10;
		sRoiConfig[1].sRect.nLeft = 320;
		sRoiConfig[1].sRect.nTop = 180;
		sRoiConfig[1].sRect.nWidth = 320;
		sRoiConfig[1].sRect.nHeight = 180;

		sRoiConfig[2].bEnable = 1;
		sRoiConfig[2].index = 2;
		sRoiConfig[2].nQPoffset = 10;
		sRoiConfig[2].sRect.nLeft = 320;
		sRoiConfig[2].sRect.nTop = 180;
		sRoiConfig[2].sRect.nWidth = 320;
		sRoiConfig[2].sRect.nHeight = 180;

		sRoiConfig[3].bEnable = 1;
		sRoiConfig[3].index = 3;
		sRoiConfig[3].nQPoffset = 10;
		sRoiConfig[3].sRect.nLeft = 320;
		sRoiConfig[3].sRect.nTop = 180;
		sRoiConfig[3].sRect.nWidth = 320;
		sRoiConfig[3].sRect.nHeight = 180;
	}

	static void init_alter_frame_rate_info(VencAlterFrameRateInfo *pAlterFrameRateInfo)
	{
		memset(pAlterFrameRateInfo, 0, sizeof(VencAlterFrameRateInfo));
		pAlterFrameRateInfo->bEnable = 1;
		pAlterFrameRateInfo->bUseUserSetRoiInfo = 1;
		pAlterFrameRateInfo->sRoiBgFrameRate.nSrcFrameRate = 25;
		pAlterFrameRateInfo->sRoiBgFrameRate.nDstFrameRate = 5;

		pAlterFrameRateInfo->roi_param[0].bEnable = 1;
		pAlterFrameRateInfo->roi_param[0].index = 0;
		pAlterFrameRateInfo->roi_param[0].nQPoffset = 10;
		pAlterFrameRateInfo->roi_param[0].roi_abs_flag = 1;
		pAlterFrameRateInfo->roi_param[0].sRect.nLeft = 0;
		pAlterFrameRateInfo->roi_param[0].sRect.nTop = 0;
		pAlterFrameRateInfo->roi_param[0].sRect.nWidth = 320;
		pAlterFrameRateInfo->roi_param[0].sRect.nHeight = 320;

		pAlterFrameRateInfo->roi_param[1].bEnable = 1;
		pAlterFrameRateInfo->roi_param[1].index = 0;
		pAlterFrameRateInfo->roi_param[1].nQPoffset = 10;
		pAlterFrameRateInfo->roi_param[1].roi_abs_flag = 1;
		pAlterFrameRateInfo->roi_param[1].sRect.nLeft = 320;
		pAlterFrameRateInfo->roi_param[1].sRect.nTop = 320;
		pAlterFrameRateInfo->roi_param[1].sRect.nWidth = 320;
		pAlterFrameRateInfo->roi_param[1].sRect.nHeight = 320;
	}

	static void init_enc_proc_info(VeProcSet *ve_proc_set)
	{
		ve_proc_set->bProcEnable = 1;
		ve_proc_set->nProcFreq = 60;
	}

	static void init_overlay_info(VencOverlayInfoS *pOverlayInfo, encode_param_t *encode_param)
	{
		int i;
		unsigned char num_bitMap = 2;
		BitMapInfoS *pBitMapInfo;
		unsigned int time_id_list[19];
		unsigned int start_mb_x;
		unsigned int start_mb_y;

		memset(pOverlayInfo, 0, sizeof(VencOverlayInfoS));

#if 0
    char filename[64];
    int ret;
    for(i = 0; i < num_bitMap; i++)
    {
        FILE* icon_hdle = NULL;
        int width  = 0;
        int height = 0;

        sprintf(filename, "%s%d.bmp", "/mnt/libcedarc/bitmap/icon_720p_",i);

        icon_hdle   = fopen(filename, "r");
        if (icon_hdle == NULL) {
            printf("get wartermark %s error\n", filename);
            return;
        }

        //get watermark picture size
        fseek(icon_hdle, 18, SEEK_SET);
        fread(&width, 1, 4, icon_hdle);
        fread(&height, 1, 4, icon_hdle);

        fseek(icon_hdle, 54, SEEK_SET);

        bit_map_info[i].argb_addr = NULL;
        bit_map_info[i].width = 0;
        bit_map_info[i].height = 0;

        bit_map_info[i].width = width;
        bit_map_info[i].height = height*(-1);

        bit_map_info[i].width_aligh16 = ALIGN_XXB(16, bit_map_info[i].width);
        bit_map_info[i].height_aligh16 = ALIGN_XXB(16, bit_map_info[i].height);
        if(bit_map_info[i].argb_addr == NULL) {
            bit_map_info[i].argb_addr =
            (unsigned char*)malloc(bit_map_info[i].width_aligh16*bit_map_info[i].height_aligh16*4);

            if(bit_map_info[i].argb_addr == NULL)
            {
                loge("malloc bit_map_info[%d].argb_addr fail\n", i);
                return;
            }
        }
        logd("bitMap[%d] size[%d,%d], size_align16[%d, %d], argb_addr:%p\n", i,
                                    bit_map_info[i].width,
                                    bit_map_info[i].height,
                                    bit_map_info[i].width_aligh16,
                                    bit_map_info[i].height_aligh16,
                                    bit_map_info[i].argb_addr);

        ret = fread(bit_map_info[i].argb_addr, 1,
            bit_map_info[i].width*bit_map_info[i].height*4, icon_hdle);
        if(ret != bit_map_info[i].width*bit_map_info[i].height*4)
            loge("read bitMap[%d] error, ret value:%d\n", i, ret);

        bit_map_info[i].size = bit_map_info[i].width_aligh16 * bit_map_info[i].height_aligh16 * 4;

        if (icon_hdle) {
            fclose(icon_hdle);
            icon_hdle = NULL;
        }
    }

    //time 2017-04-27 18:28:26
    time_id_list[0] = 2;
    time_id_list[1] = 0;
    time_id_list[2] = 1;
    time_id_list[3] = 7;
    time_id_list[4] = 11;
    time_id_list[5] = 0;
    time_id_list[6] = 4;
    time_id_list[7] = 11;
    time_id_list[8] = 2;
    time_id_list[9] = 7;
    time_id_list[10] = 10;
    time_id_list[11] = 1;
    time_id_list[12] = 8;
    time_id_list[13] = 12;
    time_id_list[14] = 2;
    time_id_list[15] = 8;
    time_id_list[16] = 12;
    time_id_list[17] = 2;
    time_id_list[18] = 6;

    logd("pOverlayInfo:%p\n", pOverlayInfo);
    pOverlayInfo->blk_num = 19;
#else
		FILE *icon_hdle = NULL;
		int width = 464;
		int height = 32;
		memset(time_id_list, 0, sizeof(time_id_list));

		icon_hdle = fopen(encode_param->overlay_file, "r");
		ump_debug("icon_hdle = %p\r\n", icon_hdle);
		if (icon_hdle == NULL)
		{
			ump_error("get icon_hdle error\n");
			return;
		}

		for (i = 0; i < num_bitMap; i++)
		{
			bit_map_info[i].argb_addr = NULL;
			bit_map_info[i].width = width;
			bit_map_info[i].height = height;

			bit_map_info[i].width_aligh16 = ALIGN_XXB(16, bit_map_info[i].width);
			bit_map_info[i].height_aligh16 = ALIGN_XXB(16, bit_map_info[i].height);
			if (bit_map_info[i].argb_addr == NULL)
			{
				bit_map_info[i].argb_addr =
					(unsigned char *)malloc(bit_map_info[i].width_aligh16 * bit_map_info[i].height_aligh16 * 4);

				if (bit_map_info[i].argb_addr == NULL)
				{
					ump_error("malloc bit_map_info[%d].argb_addr fail\n", i);
					if (icon_hdle)
					{
						fclose(icon_hdle);
						icon_hdle = NULL;
					}

					return;
				}
			}
			ump_debug("bitMap[%d] size[%d,%d], size_align16[%d, %d], argb_addr:%p\n", i,
					  bit_map_info[i].width,
					  bit_map_info[i].height,
					  bit_map_info[i].width_aligh16,
					  bit_map_info[i].height_aligh16,
					  bit_map_info[i].argb_addr);

			int ret;
			ret = fread(bit_map_info[i].argb_addr, 1,
						bit_map_info[i].width * bit_map_info[i].height * 4, icon_hdle);
			if (ret != (int)(bit_map_info[i].width * bit_map_info[i].height * 4))
				ump_error("read bitMap[%d] error, ret value:%d\n", i, ret);

			bit_map_info[i].size = bit_map_info[i].width_aligh16 * bit_map_info[i].height_aligh16 * 4;
			fseek(icon_hdle, 0, SEEK_SET);
		}
		if (icon_hdle)
		{
			fclose(icon_hdle);
			icon_hdle = NULL;
		}
#endif
		pOverlayInfo->argb_type = VENC_OVERLAY_ARGB8888;
		pOverlayInfo->blk_num = num_bitMap;
		ump_debug("blk_num:%d, argb_type:%d\n", pOverlayInfo->blk_num, pOverlayInfo->argb_type);
		// pOverlayInfo->invert_threshold = 200;
		// pOverlayInfo->invert_mode = 3;

		start_mb_x = 0;
		start_mb_y = 0;
		for (i = 0; i < pOverlayInfo->blk_num; i++)
		{
			// id = time_id_list[i];
			// pBitMapInfo = &bit_map_info[id];
			pBitMapInfo = &bit_map_info[i];

			pOverlayInfo->overlayHeaderList[i].start_mb_x = start_mb_x;
			pOverlayInfo->overlayHeaderList[i].start_mb_y = start_mb_y;
			pOverlayInfo->overlayHeaderList[i].end_mb_x = start_mb_x + (pBitMapInfo->width_aligh16 / 16 - 1);
			pOverlayInfo->overlayHeaderList[i].end_mb_y = start_mb_y + (pBitMapInfo->height_aligh16 / 16 - 1);

			pOverlayInfo->overlayHeaderList[i].extra_alpha_flag = 0;
			pOverlayInfo->overlayHeaderList[i].extra_alpha = 8;
			if (i % 3 == 0)
				pOverlayInfo->overlayHeaderList[i].overlay_type = LUMA_REVERSE_OVERLAY;
			else if (i % 2 == 0 && i != 0)
				pOverlayInfo->overlayHeaderList[i].overlay_type = COVER_OVERLAY;
			else
				pOverlayInfo->overlayHeaderList[i].overlay_type = NORMAL_OVERLAY;

			if (pOverlayInfo->overlayHeaderList[i].overlay_type == COVER_OVERLAY)
			{
				pOverlayInfo->overlayHeaderList[i].cover_yuv.cover_y = 0xff;
				pOverlayInfo->overlayHeaderList[i].cover_yuv.cover_u = 0xff;
				pOverlayInfo->overlayHeaderList[i].cover_yuv.cover_v = 0xff;
			}

			// pOverlayInfo->overlayHeaderList[i].bforce_reverse_flag = 1;

			pOverlayInfo->overlayHeaderList[i].overlay_blk_addr = pBitMapInfo->argb_addr;
			pOverlayInfo->overlayHeaderList[i].bitmap_size = pBitMapInfo->size;

			ump_info("blk_%d[%d], start_mb[%d,%d], end_mb[%d,%d],extra_alpha_flag:%d, extra_alpha:%d\n",
					 i,
					 time_id_list[i],
					 pOverlayInfo->overlayHeaderList[i].start_mb_x,
					 pOverlayInfo->overlayHeaderList[i].start_mb_y,
					 pOverlayInfo->overlayHeaderList[i].end_mb_x,
					 pOverlayInfo->overlayHeaderList[i].end_mb_y,
					 pOverlayInfo->overlayHeaderList[i].extra_alpha_flag,
					 pOverlayInfo->overlayHeaderList[i].extra_alpha);
			ump_info("overlay_type:%d, cover_yuv[%d,%d,%d], overlay_blk_addr:%p, bitmap_size:%d\n",
					 pOverlayInfo->overlayHeaderList[i].overlay_type,
					 pOverlayInfo->overlayHeaderList[i].cover_yuv.cover_y,
					 pOverlayInfo->overlayHeaderList[i].cover_yuv.cover_u,
					 pOverlayInfo->overlayHeaderList[i].cover_yuv.cover_v,
					 pOverlayInfo->overlayHeaderList[i].overlay_blk_addr,
					 pOverlayInfo->overlayHeaderList[i].bitmap_size);
			// if(i != 5)
			{
				start_mb_x += pBitMapInfo->width_aligh16 / 16;
				start_mb_y += pBitMapInfo->height_aligh16 / 16;
			}
		}

		return;
	}

	static void init_jpeg_rate_ctrl(jpeg_func_t *jpeg_func)
	{
		jpeg_func->jpeg_biteRate = 12 * 1024 * 1024;
		jpeg_func->jpeg_frameRate = 30;
		jpeg_func->bitRateRange.bitRateMax = 14 * 1024 * 1024;
		jpeg_func->bitRateRange.bitRateMin = 10 * 1024 * 1024;
	}

	static int initH264Func(h264_func_t *h264_func, encode_param_t *encode_param)
	{
		memset(h264_func, 0, sizeof(h264_func_t));

		// init h264Param
		h264_func->h264Param.bEntropyCodingCABAC = 1;
		h264_func->h264Param.nBitrate = encode_param->bit_rate;
		h264_func->h264Param.nFramerate = encode_param->frame_rate;
		h264_func->h264Param.nCodingMode = VENC_FRAME_CODING;
		h264_func->h264Param.nMaxKeyInterval = encode_param->codec_settings.codec_specific_.H264.gop;
		if (video_common::kProfileBase == encode_param->codec_settings.codec_specific_.H264.profile)
		{
			h264_func->h264Param.sProfileLevel.nProfile = VENC_H264ProfileBaseline;
			h264_func->h264Param.sProfileLevel.nLevel = VENC_H264Level11;
		}
		else if (video_common::kProfileMain == encode_param->codec_settings.codec_specific_.H264.profile)
		{
			h264_func->h264Param.sProfileLevel.nProfile = VENC_H264ProfileMain;
			h264_func->h264Param.sProfileLevel.nLevel = VENC_H264Level31;
		}
		else if (video_common::kProfileHigh == encode_param->codec_settings.codec_specific_.H264.profile)
		{
			h264_func->h264Param.sProfileLevel.nProfile = VENC_H264ProfileHigh;
			h264_func->h264Param.sProfileLevel.nLevel = VENC_H264Level51;
		}

#ifdef VBR
		h264_func->h264Param.sRcParam.eRcMode = AW_VBR;
#endif

		if (encode_param->qp_min > 0 && encode_param->qp_max)
		{
			h264_func->h264Param.sQPRange.nMinqp = encode_param->qp_min;
			h264_func->h264Param.sQPRange.nMaxqp = encode_param->qp_max;
		}
		else
		{
			h264_func->h264Param.sQPRange.nMinqp = 10;
			h264_func->h264Param.sQPRange.nMaxqp = 50;
		}
		// h264_func->h264Param.bLongRefEnable = 1;
		// h264_func->h264Param.nLongRefPoc = 0;

#if 1
		h264_func->sH264Smart.img_bin_en = 1;
		h264_func->sH264Smart.img_bin_th = 27;
		h264_func->sH264Smart.shift_bits = 2;
		h264_func->sH264Smart.smart_fun_en = 1;
#endif

		// init VencMBModeCtrl
		init_mb_mode(&h264_func->h264MBMode, encode_param);

		// init VencMBInfo
		init_mb_info(&h264_func->MBInfo, encode_param);

		// init VencH264FixQP
		init_fix_qp(&h264_func->fixQP);

		// init VencSuperFrameConfig
		init_super_frame_cfg(&h264_func->sSuperFrameCfg);

		// init VencH264SVCSkip
		init_svc_skip(&h264_func->SVCSkip);

		// init VencH264AspectRatio
		init_aspect_ratio(&h264_func->sAspectRatio);

		// init VencH264AspectRatio
		init_video_signal(&h264_func->sVideoSignal);

		// init CyclicIntraRefresh
		init_intra_refresh(&h264_func->sIntraRefresh);

		// init VencROIConfig
		init_roi(h264_func->sRoiConfig);

		// init proc info
		init_enc_proc_info(&h264_func->sVeProcInfo);

		// init VencOverlayConfig
		init_overlay_info(&h264_func->sOverlayInfo, encode_param);

		return 0;
	}

	static int initJpegFunc(jpeg_func_t *jpeg_func, encode_param_t *encode_param)
	{
		memset(jpeg_func, 0, sizeof(jpeg_func_t));

		jpeg_func->quality = 95;
		jpeg_func->jpeg_mode = 0;

		if (0 == jpeg_func->jpeg_mode)
			init_jpeg_exif(&jpeg_func->exifinfo);

		// init VencOverlayConfig
		init_overlay_info(&jpeg_func->sOverlayInfo, encode_param);

		return 0;
	}

	static int setEncParam(VideoEncoder *pVideoEnc, encode_param_t *encode_param)
	{
		int result = 0;
		VeProcSet mProcSet;
		mProcSet.bProcEnable = 1;
		mProcSet.nProcFreq = 30;
		mProcSet.nStatisBitRateTime = 1000;
		mProcSet.nStatisFrRateTime = 1000;
		VideoEncSetParameter(pVideoEnc, VENC_IndexParamProcSet, &mProcSet);

		if (encode_param->encode_format == VENC_CODEC_JPEG)
		{
			result = initJpegFunc(&encode_param->jpeg_func, encode_param);
			if (result)
			{
				ump_error("initJpegFunc error, return \n");
				return -1;
			}

			if (1 == encode_param->jpeg_func.jpeg_mode)
			{
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamJpegEncMode, &encode_param->jpeg_func.jpeg_mode);
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamBitrate, &encode_param->jpeg_func.jpeg_biteRate);
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamFramerate, &encode_param->jpeg_func.jpeg_frameRate);
				VideoEncSetParameter(pVideoEnc,
									 VENC_IndexParamSetBitRateRange, &encode_param->jpeg_func.bitRateRange);
			}
			else
			{
				unsigned int vbv_size = 4 * 1024 * 1024;
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamSetVbvSize, &vbv_size);
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamJpegQuality, &encode_param->jpeg_func.quality);
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamJpegExifInfo, &encode_param->jpeg_func.exifinfo);
			}

			if (encode_param->jpeg_func.pRoiConfig.bEnable == 1)
			{
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamRoi, &encode_param->jpeg_func.pRoiConfig);
			}

			if (encode_param->test_overlay_flag == 1)
			{
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamSetOverlay, &encode_param->jpeg_func.sOverlayInfo);
			}
		}
		else if (encode_param->encode_format == VENC_CODEC_H264)
		{
			result = initH264Func(&encode_param->h264_func, encode_param);
			if (result)
			{
				ump_error("initH264Func error, return \n");
				return -1;
			}
			unsigned int vbv_size = 12 * 1024 * 1024;
			VideoEncSetParameter(pVideoEnc, VENC_IndexParamH264Param, &encode_param->h264_func.h264Param);
			VideoEncSetParameter(pVideoEnc, VENC_IndexParamSetVbvSize, &vbv_size);
			// VideoEncSetParameter(pVideoEnc, VENC_IndexParamH264FixQP, &h264_func.fixQP);
			if (encode_param->test_overlay_flag == 1)
			{
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamSetOverlay, &encode_param->h264_func.sOverlayInfo);
			}

			VideoEncSetParameter(pVideoEnc, VENC_IndexParamProcSet, &encode_param->h264_func.sVeProcInfo);
#ifdef USE_VIDEO_SIGNAL
			VencH264VideoSignal mVencH264VideoSignal;
			memset(&mVencH264VideoSignal, 0, sizeof(VencH264VideoSignal));
			mVencH264VideoSignal.video_format = DEFAULT;
			mVencH264VideoSignal.full_range_flag = 1;
			mVencH264VideoSignal.src_colour_primaries = VENC_BT709;
			mVencH264VideoSignal.dst_colour_primaries = VENC_BT709;
			VideoEncSetParameter(pVideoEnc, VENC_IndexParamH264VideoSignal, &mVencH264VideoSignal);
#endif

#ifdef DETECT_MOTION
			MotionParam mMotionPara;
			mMotionPara.nMaxNumStaticFrame = 4;
			mMotionPara.nMotionDetectEnable = 1;
			mMotionPara.nMotionDetectRatio = 0;
			mMotionPara.nMV64x64Ratio = 0.01;
			mMotionPara.nMVXTh = 6;
			mMotionPara.nMVYTh = 6;
			mMotionPara.nStaticBitsRatio = 0.2;
			mMotionPara.nStaticDetectRatio = 2;
			VideoEncSetParameter(pVideoEnc, VENC_IndexParamMotionDetectStatus,
								 &mMotionPara);
#endif

#ifdef USE_VIDEO_SIGNAL
			VencH264VideoSignal mVencH264VideoSignal;
			memset(&mVencH264VideoSignal, 0, sizeof(VencH264VideoSignal));
			mVencH264VideoSignal.video_format = DEFAULT;
			mVencH264VideoSignal.full_range_flag = 1;
			mVencH264VideoSignal.src_colour_primaries = VENC_BT709;
			mVencH264VideoSignal.dst_colour_primaries = VENC_BT709;
			VideoEncSetParameter(pVideoEnc, VENC_IndexParamH264VideoSignal, &mVencH264VideoSignal);
#endif

#ifdef DETECT_MOTION
			MotionParam mMotionPara;
			mMotionPara.nMaxNumStaticFrame = 4;
			mMotionPara.nMotionDetectEnable = 1;
			mMotionPara.nMotionDetectRatio = 0;
			mMotionPara.nMV64x64Ratio = 0.01;
			mMotionPara.nMVXTh = 6;
			mMotionPara.nMVYTh = 6;
			mMotionPara.nStaticBitsRatio = 0.2;
			mMotionPara.nStaticDetectRatio = 2;

			VideoEncSetParameter(pVideoEnc, VENC_IndexParamMotionDetectStatus,
								 &mMotionPara);
#endif

#ifdef GET_MB_INFO
			VideoEncSetParameter(pVideoEnc, VENC_IndexParamMBInfoOutput, &encode_param->h264_func.MBInfo);
#endif
		}

		return 0;
	}

	static void setMbMode(VideoEncoder *pVideoEnc, encode_param_t *encode_param)
	{
		if (encode_param->encode_format == VENC_CODEC_H264 && encode_param->h264_func.h264MBMode.mode_ctrl_en)
		{
			VideoEncSetParameter(pVideoEnc, VENC_IndexParamMBModeCtrl, &encode_param->h264_func.h264MBMode);
		}
		else if (encode_param->encode_format == VENC_CODEC_H265 && encode_param->h265_func.h265MBMode.mode_ctrl_en)
		{
			VideoEncSetParameter(pVideoEnc, VENC_IndexParamMBModeCtrl, &encode_param->h265_func.h265MBMode);
		}
		else
			return;
	}

	static void getMbMinfo(encode_param_t *encode_param)
	{
		VencMBInfo *pMBInfo;

		if (encode_param->encode_format == VENC_CODEC_H264 && encode_param->h264_func.h264MBMode.mode_ctrl_en)
		{
			pMBInfo = &encode_param->h264_func.MBInfo;
		}
		else if (encode_param->encode_format == VENC_CODEC_H265 && encode_param->h265_func.h265MBMode.mode_ctrl_en)
		{
			pMBInfo = &encode_param->h265_func.MBInfo;
		}
		else
			return;

		unsigned int i;
		for (i = 0; i < pMBInfo->num_mb; i++)
		{
			ump_debug("No.%d MB: mad=%d, qp=%d, sse=%d, psnr=%f\n", i, pMBInfo->p_para[i].mb_mad,
					  pMBInfo->p_para[i].mb_qp, pMBInfo->p_para[i].mb_sse, pMBInfo->p_para[i].mb_psnr);
		}
	}

	static void releaseMb(encode_param_t *encode_param)
	{
		VencMBInfo *pMBInfo;
		VencMBModeCtrl *pMBMode;
		if (encode_param->encode_format == VENC_CODEC_H264 && encode_param->h264_func.h264MBMode.mode_ctrl_en)
		{
			pMBInfo = &encode_param->h264_func.MBInfo;
			pMBMode = &encode_param->h264_func.h264MBMode;
		}
		else if (encode_param->encode_format == VENC_CODEC_H265 && encode_param->h265_func.h265MBMode.mode_ctrl_en)
		{
			pMBInfo = &encode_param->h264_func.MBInfo;
			pMBMode = &encode_param->h265_func.h265MBMode;
		}
		else
			return;

		if (pMBInfo->p_para)
			free(pMBInfo->p_para);
		if (pMBMode->p_info)
			free(pMBMode->p_info);
	}

	int32_t MpiEncHandle::allwinner_init(const video_common::VideoCodecConfig *codec_settings)
	{
		memset(&sMbSumInfo, 0, sizeof(VencMBSumInfo));
		memset(&encode_param, 0, sizeof(encode_param));

		/******** begin set the default encode param ********/
		encode_param.codec_settings = *codec_settings;
		encode_param.src_width = codec_settings->width;
		encode_param.src_height = codec_settings->height;
		encode_param.dst_width = codec_settings->width;
		encode_param.dst_height = codec_settings->height;
		encode_param.bit_rate = codec_settings->initBitrate;
		encode_param.frame_rate = codec_settings->maxFramerate;
		if (video_common::kVideoCodecH264 == codec_settings->codecType)
		{
			encode_param.encode_format = VENC_CODEC_H264;
			encode_param.qp_min = codec_settings->codec_specific_.H264.min_qp;
			encode_param.qp_max = codec_settings->codec_specific_.H264.max_qp;
		}
		else if (video_common::kVideoCodecJPEG == codec_settings->codecType)
			encode_param.encode_format = VENC_CODEC_JPEG;
		else
		{
			ump_error("not support codectype\r\n");
			return -1;
		}

		/******** begin set baseConfig param********/
		memset(&baseConfig, 0, sizeof(VencBaseConfig));
		memset(&bufferParam, 0, sizeof(VencAllocateBufferParam));
		if (video_common::kNV12 == codec_settings->frameType)
			baseConfig.eInputFormat = VENC_PIXEL_YUV420SP;
		else if (video_common::kNV21 == codec_settings->frameType)
			baseConfig.eInputFormat = VENC_PIXEL_YVU420SP;
		else
		{
			ump_error("not support frametype\r\n");
			return -1;
		}
		baseConfig.memops = MemAdapterGetOpsS();
		if (baseConfig.memops == NULL)
		{
			printf("MemAdapterGetOpsS failed\n");
			return -1;
		}
		CdcMemOpen(baseConfig.memops);
		baseConfig.nInputWidth = encode_param.src_width;
		baseConfig.nInputHeight = encode_param.src_height;
		baseConfig.nStride = encode_param.src_width;
		baseConfig.nDstWidth = encode_param.dst_width;
		baseConfig.nDstHeight = encode_param.dst_height;
		baseConfig.bEncH264Nalu = 0;

		//* ve require 16-align
		int nAlignW = (baseConfig.nInputWidth + 15) & ~15;
		int nAlignH = (baseConfig.nInputHeight + 15) & ~15;

		if (baseConfig.eInputFormat == VENC_PIXEL_YUYV422 || baseConfig.eInputFormat == VENC_PIXEL_UYVY422)
		{
			bufferParam.nSizeY = nAlignW * nAlignH * 2;
			bufferParam.nSizeC = 0;
		}
		else
		{
			bufferParam.nSizeY = nAlignW * nAlignH;
			bufferParam.nSizeC = nAlignW * nAlignH / 2;
		}
		bufferParam.nBufferNum = 1;

		// create encoder
		ump_debug("encode_param.encode_format:%d\n", encode_param.encode_format);
		pVideoEnc = VideoEncCreate((VENC_CODEC_TYPE)encode_param.encode_format);

		// set enc parameter
		int result = setEncParam(pVideoEnc, &encode_param);
		if (result)
		{
			ump_error("setEncParam error, return");
			goto out;
		}

		VideoEncInit(pVideoEnc, &baseConfig);

		if (encode_param.frequency > 0)
			VideoEncoderSetFreq(pVideoEnc, encode_param.frequency);

		if (encode_param.encode_format == VENC_CODEC_H264 ||
			encode_param.encode_format == VENC_CODEC_H265)
		{
			if (encode_param.encode_format == VENC_CODEC_H264)
			{
				VideoEncGetParameter(pVideoEnc, VENC_IndexParamH264SPSPPS, &sps_pps_data);
			}
			else if (encode_param.encode_format == VENC_CODEC_H265)
			{
				VideoEncGetParameter(pVideoEnc, VENC_IndexParamH265Header, &sps_pps_data);
			}
		}

		AllocInputBuffer(pVideoEnc, &bufferParam);

		return 0;

	out:

		if (pVideoEnc)
		{
			VideoEncDestroy(pVideoEnc);
		}
		pVideoEnc = NULL;

		if (baseConfig.memops)
		{
			CdcMemClose(baseConfig.memops);
		}

		releaseMb(&encode_param);

		return -1;
	}

	int32_t MpiEncHandle::allwinner_encode(const video_common::VideoFrameBuffer *frame_buffer, uint8_t **out_buf, uint32_t *out_buf_len)
	{
		if (baseConfig.eInputFormat == VENC_PIXEL_YUV420SP || baseConfig.eInputFormat == VENC_PIXEL_YVU420SP)
		{
			/* get one input buffer */
			GetOneAllocInputBuffer(pVideoEnc, &inputBuffer);
			for (int32_t row = 0; row < baseConfig.nInputHeight; row++)
				memcpy(inputBuffer.pAddrVirY + row * baseConfig.nInputWidth, frame_buffer->DataY() + row * frame_buffer->StrideY(), baseConfig.nInputWidth);
			for (int32_t row = 0; row < baseConfig.nInputHeight / 2; row++)
				memcpy(inputBuffer.pAddrVirC + row * baseConfig.nInputWidth, frame_buffer->DataUV() + row * frame_buffer->StrideUV(), baseConfig.nInputWidth);
		}
		else
		{
			ump_error("input_format is not supported\r\n");
			return -1;
		}

		inputBuffer.bEnableCorp = 0;
		inputBuffer.sCropInfo.nLeft = 240;
		inputBuffer.sCropInfo.nTop = 240;
		inputBuffer.sCropInfo.nWidth = 240;
		inputBuffer.sCropInfo.nHeight = 240;

		FlushCacheAllocInputBuffer(pVideoEnc, &inputBuffer);
		AddOneInputBuffer(pVideoEnc, &inputBuffer);

#ifdef SET_MB_INFO
		setMbMode(pVideoEnc, &encode_param);
#endif

#ifdef SET_SMART
		if (encode_param.encode_frame_num == 0)
		{
			if (encode_param.encode_format == VENC_CODEC_H264)
			{
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamSmartFuntion,
									 &h264_func.sH264Smart);
			}
			else if (encode_param.encode_format == VENC_CODEC_H265)
			{
				VideoEncSetParameter(pVideoEnc, VENC_IndexParamSmartFuntion,
									 &h265_func.h265Smart);
			}
		}
#endif

#if ENABLE_OVERLAY_BY_WATERMARK
		VencWatermarkInfoS mWatermarkInfo;
		memset(&mWatermarkInfo, 0, sizeof(VencWatermarkInfoS));

		mWatermarkInfo.item_num = 1;
		mWatermarkInfo.item[0].start_pos_x = 16;
		mWatermarkInfo.item[0].start_pos_y = 16;

		getTimeString((char *)mWatermarkInfo.item[0].context, WATERMARK_CONTEXT_LEN);

		strcpy((char *)mWatermarkInfo.icon_pic_path, ICON_PIC_PATH);

		VideoEncSetParameter(pVideoEnc, VENC_IndexParamSetOverlayByWatermark,
							 &mWatermarkInfo);
#endif

		/* encode */
		long long time1 = GetNowUs();
		VideoEncodeOneFrame(pVideoEnc);
		long long time2 = GetNowUs();
		// ump_info("encode frame %u use time is %lldus...\n", encode_param.encode_frame_num, (time2 - time1));

#ifdef GET_MB_INFO
		getMbMinfo(&encode_param);

		if (encode_param.encode_format == VENC_CODEC_H264)
		{
			VideoEncGetParameter(pVideoEnc, VENC_IndexParamMBSumInfoOutput, &sMbSumInfo);
		}
		else if (encode_param.encode_format == VENC_CODEC_H265)
		{
			VideoEncGetParameter(pVideoEnc, VENC_IndexParamMBSumInfoOutput, &sMbSumInfo);
		}
		logv("mbInfo: mad = %d, sse = %lld, qp = %d, mbNum = %d", sMbSumInfo.sum_mad, sMbSumInfo.sum_sse,
			 sMbSumInfo.sum_qp, h264_func.MBInfo.num_mb);

		unsigned pic_size = encode_param.dst_width * encode_param.dst_height;
		double cur_psnr = 10.0 * log10(255.0 * 255.0 * 1024 / (1.0 * sMbSumInfo.sum_sse / pic_size));
		avr_sse_period += sMbSumInfo.sum_sse;
		avr_sse += sMbSumInfo.sum_sse;
		sum_sse = avr_sse;
		if (testNumber % 60 == 0)
		{
			avr_sse_period /= 60;
			double period_psnr = 10.0 * log10(255.0 * 255.0 * 1024 / (1.0 * avr_sse_period / pic_size));
			logd("frame num = %d, psnr = %0.2f , avr = %0.2f, diff = %0.2f, I frame",
				 testNumber, cur_psnr,
				 period_psnr, cur_psnr - period_psnr);
		}
		// else
		logv("frame num = %d, psnr = %0.2f, diff = %0.2f, sum_mad = %d", testNumber, cur_psnr,
			 cur_psnr - pre_psnr, sMbSumInfo.sum_mad);
		pre_psnr = cur_psnr;
		if (testNumber == 0)
		{
			min_sse = sMbSumInfo.sum_sse;
			max_sse = sMbSumInfo.sum_sse;
		}
		else
		{
			if (sMbSumInfo.sum_sse < min_sse)
			{
				min_sse = sMbSumInfo.sum_sse;
				min_sse_frame = testNumber;
			}
			else if (sMbSumInfo.sum_sse > max_sse)
			{
				max_sse = sMbSumInfo.sum_sse;
				max_sse_frame = testNumber;
			}
		}
		if (testNumber == (encode_param.encode_frame_num - 1))
		{
			double min_psnr, max_psnr, avr_psnr;
			unsigned pic_size = encode_param.dst_width * encode_param.dst_height;

			avr_sse /= encode_param.encode_frame_num;
			max_psnr = 10.0 * log10(255.0 * 255.0 * 1024 / (1.0 * min_sse / pic_size));
			min_psnr = 10.0 * log10(255.0 * 255.0 * 1024 / (1.0 * max_sse / pic_size));
			avr_psnr = 10.0 * log10(255.0 * 255.0 * 1024 / (1.0 * avr_sse / pic_size));
			logd("frame[%d] get min_psnr:%f, frame[%d] get max_psnr:%f, average_psnr:%f\n",
				 max_sse_frame,
				 min_psnr,
				 min_sse_frame,
				 max_psnr,
				 avr_psnr);
		}
#endif

		/* return inputbuffer to encoder */
		AlreadyUsedInputBuffer(pVideoEnc, &inputBuffer);
		ReturnOneAllocInputBuffer(pVideoEnc, &inputBuffer);

		/* get encode data */
		int result = GetOneBitstreamFrame(pVideoEnc, &outputBuffer);
		if (result == -1)
			return -1;

		uint8_t *data = (uint8_t *)IntelliUtilMalloc(outputBuffer.nSize0 + outputBuffer.nSize1 + outputBuffer.nSize2);
		memset(data, 0, outputBuffer.nSize0 + outputBuffer.nSize1 + outputBuffer.nSize2);

		/* write output data */
		memcpy(data, outputBuffer.pData0, outputBuffer.nSize0);
		if (outputBuffer.nSize1)
			memcpy(data + outputBuffer.nSize0, outputBuffer.pData1, outputBuffer.nSize1);
		if (outputBuffer.nSize2)
			memcpy(data + outputBuffer.nSize0 + outputBuffer.nSize1, outputBuffer.pData2, outputBuffer.nSize2);

		*out_buf = data;
		*out_buf_len = outputBuffer.nSize0 + outputBuffer.nSize1 + outputBuffer.nSize2;

		/* return outbuffer */
		FreeOneBitStreamFrame(pVideoEnc, &outputBuffer);

		encode_param.encode_frame_num++;

		return 0;
	}

	void MpiEncHandle::allwinner_request_idr()
	{
		int force_key_frame = 1;
		VideoEncSetParameter(pVideoEnc, VENC_IndexParamForceKeyFrame, &force_key_frame);
	}

	int32_t MpiEncHandle::allwinner_adjust_bitrate(uint32_t bitrate)
	{
		int biteRate = 1000 * bitrate;
		VideoEncSetParameter(pVideoEnc, VENC_IndexParamBitrate, &biteRate);
		return 0;
	}

	void MpiEncHandle::allwinner_uninit()
	{
		ReleaseAllocInputBuffer(pVideoEnc);

		if (pVideoEnc)
		{
			VideoEncDestroy(pVideoEnc);
		}
		pVideoEnc = NULL;

		if (baseConfig.memops)
		{
			CdcMemClose(baseConfig.memops);
		}

		releaseMb(&encode_param);
	}

} // namespace ecovacs
