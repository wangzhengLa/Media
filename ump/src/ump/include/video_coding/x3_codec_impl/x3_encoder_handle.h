#ifndef X3_ENCODER_HANDLE_H_
#define X3_ENCODER_HANDLE_H_

#include "video_encoder.h"
#include "vio/hb_vio_interface.h"
#include "vio/hb_vps_api.h"
#include "vio/hb_vp_api.h"
#include "vio/hb_comm_venc.h"
#include "vio/hb_venc.h"
#include "video_common_defines.h"

namespace ecovacs
{
    struct MpiEncHandle
    {
        MpiEncHandle();
        ~MpiEncHandle();

        int32_t iot_venc_init(const video_common::VideoCodecConfig *codec_settings);
        int32_t iot_venc_start();
        int32_t iot_venc_stop();
        int32_t iot_vps_unbind_venc(int32_t vpsGrp, int32_t vpsChn, int32_t vencChn);
        int32_t iot_venc_output_release(VIDEO_STREAM_S *pstStream);
        int32_t iot_venc_get_output(VIDEO_STREAM_S *pstStream);
        int32_t iot_venc_deinit();
        int32_t iot_venc_request_idr();

        int32_t iot_sys_alloc(uint64_t *pu64PhyAddr, void **ppVirAddr, uint32_t u32Len);
        int32_t iot_sys_free(uint64_t u64PhyAddr, void *pVirAddr);
        int32_t iot_venc_input(VIDEO_FRAME_S *pstFrame);

        int32_t mpp_read_image(void *mpp_buffer, const video_common::VideoFrameBuffer *frame_buffer);

        static int32_t iot_vp_init();
        static int32_t iot_vp_deinit();

        uint32_t width;
        uint32_t height;
        uint32_t hor_stride;
        uint32_t ver_stride;
        size_t frame_size;
        PIXEL_FORMAT_E pixFmt;

        VENC_CHN channel;
        VENC_CHN_ATTR_S vencChnAttr;
        VENC_PARAM_MOD_S stModParam;
        VENC_ROI_ATTR_S pstRoiAttr;

        VENC_RECV_PIC_PARAM_S pstRecvParam;
        VIDEO_FRAME_S pstFrame;
        VIDEO_STREAM_S pstStream;

        uint64_t mmz_paddr;
        char *mmz_vaddr;
    };

} // namespace ecovacs

#endif // X3_ENCODER_HANDLE_H_