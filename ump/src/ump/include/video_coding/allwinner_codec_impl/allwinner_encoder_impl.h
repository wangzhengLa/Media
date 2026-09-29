#ifndef ALLWINNER_ENCODER_IMPL_H_
#define ALLWINNER_ENCODER_IMPL_H_


#include "video_encoder.h"
#include "video_encoder_impl.h"
#include "allwinner_encoder_handle.h"


namespace ecovacs
{
	class AllWinnerEncoderImpl : public VideoEncoderImpl
	{
	public:
		enum NaluType
		{
			NALU_TYPE_SLICE = 1,
			NALU_TYPE_DPA = 2,
			NALU_TYPE_DPB = 3,
			NALU_TYPE_DPC = 4,
			NALU_TYPE_IDR = 5,
			NALU_TYPE_SEI = 6,
			NALU_TYPE_SPS = 7,
			NALU_TYPE_PPS = 8,
			NALU_TYPE_AUD = 9,
			NALU_TYPE_EOSEQ = 10,
			NALU_TYPE_EOSTREAM = 11,
			NALU_TYPE_FILL = 12
		};


		AllWinnerEncoderImpl();


		~AllWinnerEncoderImpl() override;


		int32_t InitEncode(const video_common::VideoCodecConfig *codec_settings) override;


		int32_t Release() override;


		int32_t Encode(const video_common::VideoFrameBuffer *frame_buffer) override;


		int32_t RequestKeyFrame() override;


		void SetRates(const video_common::RateControlParameters &parameters) override;


	private:
		MpiEncHandle mpi_enc_handle_;
		video_common::VideoCodecConfig codec_settings;
	};


} // namespace ecovacs


#endif // ALLWINNER_ENCODER_IMPL_H_
