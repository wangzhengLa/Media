#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class AudioResampler {
public:
    AudioResampler();
    ~AudioResampler();

    bool Init(int channels, int in_rate, int out_rate, int quality = 3);
    size_t Process(const int16_t* input, size_t input_samples, std::vector<int16_t>& output);
    void Reset();

private:
    struct Impl;
    Impl* pImpl_;
};
