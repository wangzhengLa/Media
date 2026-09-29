// AudioPublisher.cpp
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <cmath>
#include <cstring>
#include <atomic>          // 新增，用于 std::atomic

#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>

#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>

#include "AudioDataPubSubTypes.h"

using namespace eprosima::fastdds::dds;

// 模拟生成音频数据：填充正弦波 PCM（16位单声道，采样率 44100，每帧 1024 样本）
void FillAudioData(AudioData& audio, uint32_t frame_index) {
    const int sample_rate = 44100;
    const int channels = 1;
    const int samples = 1024;               // 每帧样本数
    const float frequency = 440.0f;         // 440Hz 正弦波

    audio.timestamp(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
    audio.sample_rate(sample_rate);
    audio.channels(channels);
    audio.samples(samples);

    // 生成 PCM 数据 (int16_t 单声道)
    std::vector<uint8_t> pcm_data;
    pcm_data.resize(samples * sizeof(int16_t));
    int16_t* pcm = reinterpret_cast<int16_t*>(pcm_data.data());

    float phase = 2.0f * M_PI * frequency / sample_rate * frame_index * samples;
    for (int i = 0; i < samples; ++i) {
        float value = 0.8f * 32767.0f * sinf(phase + 2.0f * M_PI * frequency / sample_rate * i);
        pcm[i] = static_cast<int16_t>(value);
    }

    audio.audio_data().assign(pcm_data.begin(), pcm_data.end());
}

// 监听器：仅打印匹配状态，并记录匹配标志
class PubListener : public DataWriterListener {
public:
    std::atomic<bool> matched{false};

    void on_publication_matched(DataWriter*, const PublicationMatchedStatus& info) override {
        if (info.current_count_change == 1) {
            std::cout << "Publisher matched." << std::endl;
            matched = true;
        } else if (info.current_count_change == -1) {
            std::cout << "Publisher unmatched." << std::endl;
            matched = false;
        }
    }
};

int main() {
    std::cout << "Starting Audio Publisher..." << std::endl;

    // 1. 创建 DomainParticipant
    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name("AudioPublisher");
    DomainParticipant* participant = DomainParticipantFactory::get_instance()->create_participant(0, pqos);
    if (!participant) {
        std::cerr << "Failed to create participant." << std::endl;
        return 1;
    }

    // 2. 注册类型
    eprosima::fastdds::dds::TypeSupport type(new AudioDataPubSubType());
    if (participant->register_type(type) != eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK) {
        std::cerr << "Failed to register type." << std::endl;
        return 1;
    }

    // 3. 创建 Publisher
    PublisherQos pubqos = PUBLISHER_QOS_DEFAULT;
    Publisher* publisher = participant->create_publisher(pubqos);
    if (!publisher) {
        std::cerr << "Failed to create publisher." << std::endl;
        return 1;
    }

    // 4. 创建 Topic
    TopicQos tqos = TOPIC_QOS_DEFAULT;
    Topic* topic = participant->create_topic("AudioTopic", "AudioData", tqos);
    if (!topic) {
        std::cerr << "Failed to create topic." << std::endl;
        return 1;
    }

    // 5. 创建 DataWriter，使用 RELIABLE QoS
    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    wqos.history().depth = 30;

    PubListener listener;
    DataWriter* writer = publisher->create_datawriter(topic, wqos, &listener);
    if (!writer) {
        std::cerr << "Failed to create data writer." << std::endl;
        return 1;
    }

    // 等待匹配成功再发送
    std::cout << "Waiting for subscriber to match..." << std::endl;
    while (!listener.matched) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "Subscriber matched! Starting to publish now." << std::endl;
    std::cout << "Publishing audio frames (press Enter to stop)..." << std::endl;

    // 6. 循环发送音频帧
    bool stop = false;
    std::thread input_thread([&stop]() {
        std::cin.ignore();
        stop = true;
    });

    const uint32_t MAX_FRAMES = 100;
    uint32_t sent_ok = 0, sent_fail = 0;
    for (uint32_t frame_index = 0; frame_index < MAX_FRAMES && !stop; ++frame_index) {
        AudioData audio;
        FillAudioData(audio, frame_index);

        bool ok = writer->write(&audio);
        if (ok) { ++sent_ok; } else { ++sent_fail; }

        std::cout << "[SEND] frame=" << frame_index
                  << " rc=" << (ok ? "OK" : "FAIL")
                  << " ts=" << audio.timestamp() << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(23));
    }

    std::cout << "[SUMMARY] sent_ok=" << sent_ok
              << " sent_fail=" << sent_fail << std::endl;

    // 等待所有数据被确认（RELIABLE 模式）
    {
        auto ack_rc = writer->wait_for_acknowledgments(eprosima::fastrtps::c_TimeInfinite);
        bool ack_ok = (ack_rc == eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK);
        std::cout << "[WAIT_ACK] ok=" << (ack_ok ? "true" : "false")
                  << " value=" << ack_rc() << std::endl;
    }

    // 清理资源
    publisher->delete_datawriter(writer);
    participant->delete_publisher(publisher);
    participant->delete_topic(topic);
    DomainParticipantFactory::get_instance()->delete_participant(participant);

    input_thread.join();
    std::cout << "Audio Publisher stopped." << std::endl;
    return 0;
}
