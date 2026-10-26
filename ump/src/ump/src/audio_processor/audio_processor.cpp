#include "audio_processor/audio_processor.h"
#include "api/audio/audio_processing.h"
#include <array>
#include <new>

struct AudioProcessor {
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig stream_config;

    explicit AudioProcessor(int sample_rate)
        : apm(webrtc::AudioProcessingBuilder().Create()),
          stream_config(sample_rate, 1) {}
};

// 辅助函数：将整数等级转换为 NoiseSuppression::Level
static webrtc::AudioProcessing::Config::NoiseSuppression::Level int_to_noise_level(int level) {
    switch (level) {
        case 1:return webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
        case 2: return webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
        case 3: return webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
        case 4: return webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;
        default: return webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
    }
}

AudioProcessor* audio_processor_create(int sample_rate, int noise_level) {
    // 支持 8k, 16k, 32k, 48k
    if (sample_rate != 8000 && sample_rate != 16000 &&
        sample_rate != 32000 && sample_rate != 48000) {
        return nullptr;
    }

    AudioProcessor* processor = new (std::nothrow) AudioProcessor(sample_rate);
    if (!processor || !processor->apm) {
        delete processor;
        return nullptr;
    }

    webrtc::AudioProcessing::Config config;
    // 启用 AEC（调试阶段参考信号传零，AEC 视为无回声）
    config.echo_canceller.enabled = true;
    // 启用噪声抑制，根据传入等级设置
    config.noise_suppression.enabled = true;
    config.noise_suppression.level = int_to_noise_level(noise_level);
    // 启用增益控制
    config.gain_controller1.enabled = true;
    config.gain_controller1.mode =
        webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
    config.gain_controller1.analog_gain_controller.enabled = false;

    // 新增：启用高通滤波器
    config.high_pass_filter.enabled = true;

    processor->apm->ApplyConfig(config);

    if (processor->apm->Initialize() != webrtc::AudioProcessing::kNoError) {
        delete processor;
        return nullptr;
    }
    return processor;
}

int audio_processor_process(AudioProcessor* processor,
                            const int16_t* record_pcm,
                            const int16_t* render_pcm,
                            int16_t* output_pcm,
                            int delay_ms) {
    if (!processor || !record_pcm || !output_pcm) {
        return -1;
    }

    // 帧长度 = sample_rate / 100（10ms 帧）
    int frame_size = processor->stream_config.sample_rate_hz() / 100;

    // 如果 render_pcm 为 NULL，构造一个全零数组（安全起见）
    std::vector<int16_t> silence;
    if (!render_pcm) {
        silence.assign(frame_size, 0);
        render_pcm = silence.data();
    }

    // ProcessReverseStream
    std::vector<int16_t> render_output(frame_size);
    int result = processor->apm->ProcessReverseStream(
        render_pcm,
        processor->stream_config,
        processor->stream_config,
        render_output.data());
    if (result != webrtc::AudioProcessing::kNoError) {
        return result;
    }

    // 使用传入的延迟值，若 <=0 则设为 0
    int delay_to_set = (delay_ms > 0) ? delay_ms : 0;
    processor->apm->set_stream_delay_ms(delay_to_set);

    return processor->apm->ProcessStream(record_pcm,
                                         processor->stream_config,
                                         processor->stream_config,
                                         output_pcm);
}

void audio_processor_destroy(AudioProcessor* processor) {
    delete processor;
}
