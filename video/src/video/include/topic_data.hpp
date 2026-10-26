#ifndef TOPIC_DATA_HPP
#define TOPIC_DATA_HPP

#include <cstdint>

struct NV12Frame
{
    static constexpr uint32_t MAX_WIDTH = 640;
    static constexpr uint32_t MAX_HEIGHT = 480;
    static constexpr uint32_t MAX_FRAME_SIZE = MAX_WIDTH * MAX_HEIGHT * 3 / 2;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t data[MAX_FRAME_SIZE];
};

struct H264Frame
{
    static constexpr uint32_t MAX_H264_SIZE = 128 * 1024;
    uint32_t size;
    uint8_t data[MAX_H264_SIZE];
};

#endif // TOPIC_DATA_HPP