#include "video_encoder_impl.h"
#include "allwinner_encoder_handle.h"
#include "allwinner_encoder_impl.h"
#include "log.h"
#include "osp.h"

namespace ecovacs
{
	AllWinnerEncoderImpl::AllWinnerEncoderImpl() {}

	AllWinnerEncoderImpl::~AllWinnerEncoderImpl() {}

	int32_t AllWinnerEncoderImpl::InitEncode(const video_common::VideoCodecConfig *codec_settings)
	{
		int32_t s32Ret;
		s32Ret = CheckCodecConfig(codec_settings);
		if (s32Ret != WEBRTC_VIDEO_CODEC_OK)
		{
			ump_error("----------video_encoder_check_config error s32Ret:%d------------\r\n", s32Ret);
			return WEBRTC_VIDEO_CODEC_ERROR;
		}
		s32Ret = mpi_enc_handle_.allwinner_init(codec_settings);
		if (0 != s32Ret)
		{
			ump_error("----------InitEncode error s32Ret:%d------------\r\n", s32Ret);
			return WEBRTC_VIDEO_CODEC_ERROR;
		}

		this->codec_settings = *codec_settings;

		return WEBRTC_VIDEO_CODEC_OK;
	}

	int32_t AllWinnerEncoderImpl::Release()
	{
		mpi_enc_handle_.allwinner_uninit();

		return WEBRTC_VIDEO_CODEC_OK;
	}

	int32_t AllWinnerEncoderImpl::Encode(const video_common::VideoFrameBuffer *frame_buffer)
	{
		uint8_t *out_buf = nullptr;
		uint32_t out_buf_len = 0;
		mpi_enc_handle_.allwinner_encode(frame_buffer, &out_buf, &out_buf_len);
		if (encoded_image_callback_ && 0 != out_buf_len && out_buf)
		{
			uint8_t *encoded_image_data = static_cast<uint8_t *>(out_buf);
			video_common::EncodedImageBuffer *encoded_image_buffer = new video_common::EncodedImageBuffer(encoded_image_data, out_buf_len);
			video_common::CodecSpecificInfo codec_specific_info;
			if (video_common::kVideoCodecH264 == codec_settings.codecType)
			{
				codec_specific_info.codecType = video_common::kVideoCodecH264;
				// IDR帧主动插入sps_pps
				if (NALU_TYPE_IDR == (encoded_image_data[4] & 0x1F))
				{
					codec_specific_info.codecSpecific.H264.idr_frame = true;
					codec_specific_info.codecSpecific.H264.sps_pps = mpi_enc_handle_.sps_pps_data.pBuffer;
					codec_specific_info.codecSpecific.H264.sps_pps_len = mpi_enc_handle_.sps_pps_data.nLength;
				}
				else
				{
					codec_specific_info.codecSpecific.H264.idr_frame = false;
					codec_specific_info.codecSpecific.H264.sps_pps = nullptr;
					codec_specific_info.codecSpecific.H264.sps_pps_len = 0;
				}
			}
			else
				codec_specific_info.codecType = video_common::kVideoCodecJPEG;

			encoded_image_buffer->AddRef();
			encoded_image_callback_->OnFrame(encoded_image_buffer, &codec_specific_info);
			encoded_image_buffer->RelRef();
			IntelliUtilFree(out_buf);
		}
		return WEBRTC_VIDEO_CODEC_OK;
	}

	int32_t AllWinnerEncoderImpl::RequestKeyFrame()
	{
		if (video_common::kVideoCodecH264 == codec_settings.codecType)
			mpi_enc_handle_.allwinner_request_idr();
		return WEBRTC_VIDEO_CODEC_OK;
	}

	void AllWinnerEncoderImpl::SetRates(const video_common::RateControlParameters &parameters)
	{
		if (video_common::kVideoCodecH264 == codec_settings.codecType)
			// allwinner_adjust_bitrate internally multiplies by 1000 and
			// feeds bps to cedarc, so pass kbps here.
			mpi_enc_handle_.allwinner_adjust_bitrate(parameters.target_bitrate / 1000);
		return;
	}

} // namespace ecovacs
