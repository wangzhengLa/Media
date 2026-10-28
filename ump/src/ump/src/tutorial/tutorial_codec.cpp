#include <signal.h>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <memory>
#include "osp.h"
#include "video_encoder.h"
#include "video_encoder_factory.h"

using namespace ecovacs;

// Simple log macros
#define LOG_INFO(fmt, ...)                                                  \
    do                                                                      \
    {                                                                       \
        time_t now = time(nullptr);                                         \
        struct tm *tm_info = localtime(&now);                               \
        char time_buf[32];                                                  \
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info); \
        printf("[%s] [INFO] " fmt "\n", time_buf, ##__VA_ARGS__);           \
    } while (0)

#define LOG_ERROR(fmt, ...)                                                 \
    do                                                                      \
    {                                                                       \
        time_t now = time(nullptr);                                         \
        struct tm *tm_info = localtime(&now);                               \
        char time_buf[32];                                                  \
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info); \
        fprintf(stderr, "[%s] [ERROR] " fmt "\n", time_buf, ##__VA_ARGS__); \
    } while (0)

class H264FileSink : public video_common::EncodedImageSinkInterface
{
public:
    H264FileSink()
    {
        h264_fp_ = fopen("/tmp/codec_test.h264", "wb+");
        if (!h264_fp_)
        {
            LOG_ERROR("Failed to open /tmp/codec_test.h264");
        }
    }

    ~H264FileSink()
    {
        if (h264_fp_)
            fclose(h264_fp_);
    }

    void OnFrame(video_common::EncodedImageBuffer *encoded_image,
                 const video_common::CodecSpecificInfo *codec_specific_info) override
    {
        if (!h264_fp_ || !encoded_image || encoded_image->size() == 0)
            return;

        // Check if IDR frame, insert sps/pps before IDR
        if (encoded_image->size() >= 5)
        {
            uint8_t nalu_type = encoded_image->data()[4] & 0x1F;
            if (nalu_type == 5 && codec_specific_info &&
                codec_specific_info->codecSpecific.H264.idr_frame &&
                codec_specific_info->codecSpecific.H264.sps_pps)
            {
                fwrite(codec_specific_info->codecSpecific.H264.sps_pps,
                       codec_specific_info->codecSpecific.H264.sps_pps_len, 1, h264_fp_);
            }
        }

        fwrite(encoded_image->data(), encoded_image->size(), 1, h264_fp_);
        fflush(h264_fp_);
    }

private:
    FILE *h264_fp_;
};

static bool g_running = true;

static void signal_handler(int signal)
{
    LOG_INFO("Received signal: %d", signal);
    if (signal == SIGTSTP || signal == SIGINT)
    {
        g_running = false;
    }
}

int main(int argc, char **argv)
{
    // Signal handling
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTSTP, signal_handler);

    LOG_INFO("Program started");

    // 1280x720 resolution (match ISP FR output)
    const int32_t width = 1280;
    const int32_t height = 720;
    int stride_y = width;
    int stride_uv = (width + 1) / 2;

    int size = width * height * 3 / 2; // NV12 format
    std::unique_ptr<char, decltype(&free)> data((char *)malloc(size), free);
    if (!data)
    {
        LOG_ERROR("Failed to allocate memory");
        return -1;
    }

    FILE *fp_yuv = fopen("/tmp/factory_test_1280_720_nv12.yuv", "rb");
    if (!fp_yuv)
    {
        LOG_ERROR("Cannot open /tmp/factory_test_1280_720_nv12.yuv");
        return -1;
    }

    LOG_INFO("Creating VideoFrameBuffer (1280x720)...");
    std::unique_ptr<video_common::VideoFrameBuffer> buffer(
        new video_common::VideoFrameBuffer(width, height, stride_y, stride_uv));
    if (!buffer)
    {
        LOG_ERROR("Failed to create VideoFrameBuffer");
        fclose(fp_yuv);
        return -1;
    }

    // Read YUV data
    size_t bytes_read = fread(data.get(), 1, width * height * 3 / 2, fp_yuv);
    LOG_INFO("Read %zu bytes from YUV file", bytes_read);

    uint8_t *buf_y = (uint8_t *)data.get();
    uint8_t *buf_uv = buf_y + width * height;

    LOG_INFO("Copying Y plane...");
    for (uint32_t row = 0; row < height; row++)
    {
        memcpy(buffer->MutableDataY() + row * buffer->StrideY(),
               buf_y + row * width, width);
    }

    LOG_INFO("Copying UV plane...");
    for (uint32_t row = 0; row < height / 2; row++)
    {
        memcpy(buffer->MutableDataUV() + row * buffer->StrideUV(),
               buf_uv + row * width, width);
    }

    LOG_INFO("YUV data loaded successfully");

    // Save test YUV file
    char file_path[128] = {0};
    snprintf(file_path, sizeof(file_path), "/userdata/1280x720_capture.yuv");
    IUtilImageToFile(buffer->MutableDataY(), buffer->MutableDataUV(), NULL, width, height,
                     buffer->StrideY(), buffer->StrideUV(), 0, 1, file_path);
    LOG_INFO("Test YUV saved to: %s", file_path);

    // Configure video codec
    video_common::VideoCodecConfig video_codec_config;
    video_codec_config.width = width;
    video_codec_config.height = height;
    video_codec_config.maxFramerate = 30;
    video_codec_config.frameType = video_common::FrameType::kNV12;
    video_codec_config.codecType = video_common::VideoCodecType::kVideoCodecH264;
    video_codec_config.maxBitrate = 2048 * 1000;  // 2 Mbps
    video_codec_config.minBitrate = 256 * 1000;   // 256 Kbps
    video_codec_config.initBitrate = 1024 * 1000; // 1 Mbps

    video_codec_config.codec_specific_.H264.profile = video_common::VideoCodecProfile::kProfileBase;
    video_codec_config.codec_specific_.H264.init_qp = 35;
    video_codec_config.codec_specific_.H264.min_qp = 25;
    video_codec_config.codec_specific_.H264.max_qp = 51;
    video_codec_config.codec_specific_.H264.gop = 30;

    LOG_INFO("Creating video encoder...");
    std::unique_ptr<VideoEncoderModule> video_encode_element(
        VideoEncoderFactory::Create(video_codec_config.codecType));
    if (!video_encode_element)
    {
        LOG_ERROR("Failed to create video encoder");
        fclose(fp_yuv);
        return -1;
    }

    std::unique_ptr<H264FileSink> h264_file_sink(new H264FileSink());
    video_encode_element->RegisterEncodeCompleteCallback(h264_file_sink.get());

    LOG_INFO("Initializing encoder...");
    int32_t ret = video_encode_element->InitEncode(&video_codec_config);
    if (ret != 0)
    {
        LOG_ERROR("InitEncode failed with code: %d", ret);
        return -1;
    }
    LOG_INFO("InitEncode success");

    // Encoding loop
    LOG_INFO("Starting encoding 100 frames...");
    for (int i = 0; i < 100 && g_running; i++)
    {
        int64_t start_time = IntelliUtilTimeMillis();
        ret = video_encode_element->Encode(buffer.get());
        int64_t end_time = IntelliUtilTimeMillis();

        if (ret != 0)
        {
            LOG_ERROR("Encode frame %d failed with code: %d", i, ret);
        }
        else
        {
            LOG_INFO("Frame %d encoded successfully, time: %ld ms", i, end_time - start_time);
        }

        // Optional: sleep to control frame rate
        // IntelliUtilSleepMs(33); // ~30fps
    }
    LOG_INFO("Encoding finished");

    LOG_INFO("Releasing encoder...");
    video_encode_element->Release();
    LOG_INFO("Encoder released");

    fclose(fp_yuv);
    LOG_INFO("Program finished successfully");

    while (g_running)
    {
        IntelliUtilSleepMs(1000);
    }

    return 0;
}