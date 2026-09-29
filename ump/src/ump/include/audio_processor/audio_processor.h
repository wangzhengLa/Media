#ifndef AUDIO_PROCESSOR_H_
#define AUDIO_PROCESSOR_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioProcessor AudioProcessor;

// 创建音频处理器，支持 8000, 16000, 32000, 48000 Hz 单声道
// sample_rate: 采样率
// noise_level: 降噪等级；<=0=纯AEC隔离模式(关AGC+NS)，1=Low, 2=Moderate, 3=High, 4=VeryHigh
// 返回 NULL 表示不支持的采样率或创建失败
AudioProcessor* audio_processor_create(int sample_rate, int noise_level);

// 处理一帧数据（帧长度 = sample_rate / 100，即 10ms 帧）
// record_pcm: 麦克风输入（帧长度样本）
// render_pcm: 参考信号（用于 AEC，必须提供；长度同 record_pcm）
// output_pcm: 处理后的输出（长度同 record_pcm）
// 延时由 AEC3 内部自动估算，无需外部指定
// 返回值：0 成功，其他失败
int audio_processor_process(AudioProcessor* processor,
                            const int16_t* record_pcm,
                            const int16_t* render_pcm,
                            int16_t* output_pcm);

void audio_processor_destroy(AudioProcessor* processor);

// AEC 运行时统计（用于诊断双讲/延时漂移问题）
// 所有含无效值的字段会被写成对应以外的哨兵值：
// delay_ms / delay_median_ms / delay_std_ms 无效时为 -1；其余为 -1.0
typedef struct AudioProcStats {
    int delay_ms;                 // 瞬时延时估计 (ms)
    int delay_median_ms;          // 1秒聚合延时中位数 (ms)
    int delay_std_ms;             // 1秒聚合延时标准差 (ms)
    double echo_return_loss;      // ERL: 远端/回声比 (dB)，越大越好
    double echo_return_loss_enhancement; // ERLE: 回声消除增益 (dB)，越大越好
    double residual_echo_likelihood;     // 残响似然 0~1，越接近1越危险
    double divergent_filter_fraction;    // 线性滤波器发散占比 0~1，接近1=滤波失控
} AudioProcStats;

// 读取 AEC 运行时统计。processor 无效返回负值；其余以哨兵值填充无效字段。
int audio_processor_get_stats(AudioProcessor* processor, AudioProcStats* stats);

#ifdef __cplusplus
}
#endif

#endif  // AUDIO_PROCESSOR_H_
