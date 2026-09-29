#include "audio_processor/audio_processor.h"
#include "api/audio/audio_processing.h"
#include <new>
#include <vector>

struct AudioProcessor {
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig stream_config;
    std::vector<int16_t> silence;
    std::vector<int16_t> render_output;

    explicit AudioProcessor(int sample_rate)
        : apm(webrtc::AudioProcessingBuilder().Create()),
          stream_config(sample_rate, 1),
          silence(sample_rate / 100, 0),
          render_output(sample_rate / 100) {}
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
    // 启用高通滤波器
    config.high_pass_filter.enabled = true;

    if (noise_level <= 0) {
        // 纯 AEC 隔离模式：关闭 AGC 与 NS，只保留回声消除，便于验证 AEC 本体效果
        config.noise_suppression.enabled = false;
        config.gain_controller1.enabled = false;
    } else {
        // 启用噪声抑制，根据传入等级设置
        config.noise_suppression.enabled = true;
        config.noise_suppression.level = int_to_noise_level(noise_level);
 
        // 启用增益控制，
        //AGC1 的增益发生在AEC之前，AEC对"已放大"的信号仍按比例消，抵消率不变，输出的回声相对人声仍是消过的那份，不会因为增益被重新抬高。所以AGC1下AEC表现"正常消除",效果比较好。
        //AGC2 在AEC之后施加增益,放大了AEC的残余回声,导致能够听到回消的声音，效果不太好；
        config.gain_controller1.enabled = true;
        config.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
        config.gain_controller1.analog_gain_controller.enabled = false;
    }

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
                            int16_t* output_pcm) {
    if (!processor || !record_pcm || !output_pcm) {
        return -1;
    }

    // 帧长度 = sample_rate / 100（10ms 帧）
    int frame_size = processor->stream_config.sample_rate_hz() / 100;

    // 确保复用缓冲足够容纳当前帧长（正常应已在 create 时分配；兜底防未来帧长变更越界）
    if (processor->render_output.size() < (size_t)frame_size) {
        processor->render_output.resize(frame_size);
    }
    if (processor->silence.size() < (size_t)frame_size) {
        processor->silence.assign(frame_size, 0);
    }

    // 如果 render_pcm 为 NULL，构造一个全零数组（安全起见）
    if (!render_pcm) {
        processor->silence.assign(frame_size, 0);
        render_pcm = processor->silence.data();
    }

    // ProcessReverseStream
    int result = processor->apm->ProcessReverseStream(
        render_pcm,
        processor->stream_config,
        processor->stream_config,
        processor->render_output.data());
    if (result != webrtc::AudioProcessing::kNoError) {
        return result;
    }

    // 延时交由 AEC3 内部自动估算：
    // set_stream_delay_ms() 只是把平台已知的外部缓冲延时告诉 AEC，并非延时开关。
    // 这里不调用（等价 external delay=0），AEC3 通过内部延时估计器自动对齐
    // 参考信号与回声（硬回采场景近零延时，内部估计器可覆盖）。

    return processor->apm->ProcessStream(record_pcm,
                                         processor->stream_config,
                                         processor->stream_config,
                                         output_pcm);
}

int audio_processor_get_stats(AudioProcessor* processor, AudioProcStats* stats) {
    if (!processor || !stats) {
        return -1;
    }

    // 先以哨兵值填充，便于唤醒无效字段
    stats->delay_ms = -1;
    stats->delay_median_ms = -1;
    stats->delay_std_ms = -1;
    stats->echo_return_loss = -1.0;
    stats->echo_return_loss_enhancement = -1.0;
    stats->residual_echo_likelihood = -1.0;
    stats->divergent_filter_fraction = -1.0;

    webrtc::AudioProcessingStats s = processor->apm->GetStatistics();
    if (s.delay_ms.has_value()) stats->delay_ms = *s.delay_ms;
    if (s.delay_median_ms.has_value()) stats->delay_median_ms = *s.delay_median_ms;
    if (s.delay_standard_deviation_ms.has_value()) stats->delay_std_ms = *s.delay_standard_deviation_ms;
    if (s.echo_return_loss.has_value()) stats->echo_return_loss = *s.echo_return_loss;
    if (s.echo_return_loss_enhancement.has_value())
        stats->echo_return_loss_enhancement = *s.echo_return_loss_enhancement;
    if (s.residual_echo_likelihood.has_value())
        stats->residual_echo_likelihood = *s.residual_echo_likelihood;
    if (s.divergent_filter_fraction.has_value())
        stats->divergent_filter_fraction = *s.divergent_filter_fraction;
    return 0;
}

void audio_processor_destroy(AudioProcessor* processor) {
    delete processor;
}
