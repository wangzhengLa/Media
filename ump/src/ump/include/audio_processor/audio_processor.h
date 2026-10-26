#ifndef AUDIO_PROCESSOR_H_
#define AUDIO_PROCESSOR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioProcessor AudioProcessor;

// 创建音频处理器，支持 8000, 16000, 32000, 48000 Hz 单声道
// sample_rate: 采样率
// noise_level: 降噪等级 1=Low, 2=Moderate, 3=High, 4=VeryHigh；其他值默认为 Low
// 返回 NULL 表示不支持的采样率或创建失败
AudioProcessor* audio_processor_create(int sample_rate, int noise_level);

// 处理一帧数据（帧长度 = sample_rate / 100，即 10ms 帧）
// record_pcm: 麦克风输入（帧长度样本）
// render_pcm: 参考信号（用于 AEC，必须提供；长度同 record_pcm）
// output_pcm: 处理后的输出（长度同 record_pcm）
// delay_ms: 播放-采集延迟（毫秒），<=0 时内部置 0
// 返回值：0 成功，其他失败
int audio_processor_process(AudioProcessor* processor,
                            const int16_t* record_pcm,
                            const int16_t* render_pcm,
                            int16_t* output_pcm,
                            int delay_ms);

void audio_processor_destroy(AudioProcessor* processor);

#ifdef __cplusplus
}
#endif

#endif  // AUDIO_PROCESSOR_H_
