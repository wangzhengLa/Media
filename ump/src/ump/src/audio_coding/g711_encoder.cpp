#include "audio_coding/g711_encoder.h"
#include "audio_coding/audio_g711.h"

G711Encoder::G711Encoder(Type type) : type_(type) {}

std::vector<uint8_t> G711Encoder::Encode(const int16_t* pcm, size_t num_samples) const {
    std::vector<uint8_t> out(num_samples);
    if (type_ == ALAW) {
        for (size_t i = 0; i < num_samples; ++i)
            out[i] = linear_to_alaw(pcm[i]);
    } else {
        for (size_t i = 0; i < num_samples; ++i)
            out[i] = linear_to_ulaw(pcm[i]);
    }
    return out;
}

std::vector<int16_t> G711Encoder::Decode(const uint8_t* g711_data, size_t num_bytes) const {
    std::vector<int16_t> out(num_bytes);
    if (type_ == ALAW) {
        for (size_t i = 0; i < num_bytes; ++i)
            out[i] = alaw_to_linear(g711_data[i]);
    } else {
        for (size_t i = 0; i < num_bytes; ++i)
            out[i] = ulaw_to_linear(g711_data[i]);
    }
    return out;
}
