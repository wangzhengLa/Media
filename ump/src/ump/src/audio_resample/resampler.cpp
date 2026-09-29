#include "audio_resample/resampler.h"
#include <speex/speex_resampler.h>
#include <cstring>

struct AudioResampler::Impl {
    SpeexResamplerState* state = nullptr;
    int channels = 1;
    int in_rate = 0;
    int out_rate = 8000;
    bool initialized = false;
};

AudioResampler::AudioResampler() : pImpl_(new Impl) {}
AudioResampler::~AudioResampler() { Reset(); }

bool AudioResampler::Init(int channels, int in_rate, int out_rate, int quality) {
    Reset();
    if (channels <= 0 || in_rate <= 0 || out_rate <= 0) return false;
    int err = 0;
    pImpl_->state = speex_resampler_init(channels, in_rate, out_rate, quality, &err);
    if (!pImpl_->state) return false;
    pImpl_->channels = channels;
    pImpl_->in_rate = in_rate;
    pImpl_->out_rate = out_rate;
    pImpl_->initialized = true;
    return true;
}

size_t AudioResampler::Process(const int16_t* input, size_t input_samples,
                               std::vector<int16_t>& output) {
    if (!pImpl_->initialized || !input || input_samples == 0) return 0;

    spx_uint32_t in_len_per_channel = input_samples / pImpl_->channels;
    if (in_len_per_channel == 0) return 0;

    // 估算输出每声道样本数
    size_t out_samples_per_channel = (size_t)((double)in_len_per_channel * pImpl_->out_rate / pImpl_->in_rate + 0.5);
    size_t total_out_samples = out_samples_per_channel * pImpl_->channels;
    output.resize(total_out_samples);

    spx_uint32_t in_len = in_len_per_channel;
    spx_uint32_t out_len = out_samples_per_channel;

    // 单声道：使用speex_resampler_process_int处理单声道数据
    // 多声道：默认是交织格式，使用speex_resampler_process_interleaved_int处理交织的多声道数据
    int ret;
    if (pImpl_->channels == 1) {
        ret = speex_resampler_process_int(pImpl_->state, 0,
                                          input, &in_len,
                                          output.data(), &out_len);
    } else {
        ret = speex_resampler_process_interleaved_int(pImpl_->state,
                                                      input, &in_len,
                                                      output.data(), &out_len);
    }

    if (ret != 0) {
        return 0;
    }

    size_t actual_out = out_len * pImpl_->channels;
    if (actual_out < output.size()) output.resize(actual_out);
    return actual_out;
}

void AudioResampler::Reset() {
    if (pImpl_->state) {
        speex_resampler_destroy(pImpl_->state);
        pImpl_->state = nullptr;
    }
    pImpl_->initialized = false;
}
