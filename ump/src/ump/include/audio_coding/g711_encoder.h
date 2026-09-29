#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class G711Encoder {
public:
    enum Type {
        ALAW,
        ULAW
    };

    explicit G711Encoder(Type type = ALAW);

    // 编码：将 16-bit PCM 样本（S16_LE）编码为 G.711 8-bit 数据
    std::vector<uint8_t> Encode(const int16_t* pcm, size_t num_samples) const;

    // 解码：将 G.711 8-bit 数据解码为 16-bit PCM 样本
    std::vector<int16_t> Decode(const uint8_t* g711_data, size_t num_bytes) const;

private:
    Type type_;
};
