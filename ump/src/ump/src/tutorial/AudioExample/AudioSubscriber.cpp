// AudioSubscriber.cpp
#include <iostream>
#include <fstream>                // 新增：文件操作
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include "AudioDataPubSubTypes.h"
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/core/status/SubscriptionMatchedStatus.hpp>

using namespace eprosima::fastdds::dds;

// 监听器：接收数据并写入文件 + 终端打印
class SubListener : public DataReaderListener {
public:
    SubListener(const std::string& filename) {
        // 打开日志文件（追加模式，若不存在则创建）
        log_file_.open(filename, std::ios::out | std::ios::app);
        if (!log_file_.is_open()) {
            std::cerr << "Failed to open log file: " << filename << std::endl;
        } else {
            // 写入文件头（记录启动时间）
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            log_file_ << "=== Audio Subscriber started at " << std::ctime(&time_t_now) << "===\n";
            log_file_.flush();
        }
    }

    ~SubListener() {
        if (log_file_.is_open()) {
            log_file_.close();
        }
    }

    void on_data_available(DataReader* reader) override {
        AudioData audio;
        SampleInfo info;
        if (reader->take_next_sample(&audio, &info) == ReturnCode_t::RETCODE_OK) {
            if (info.valid_data) {
                // 1. 打印到终端（保留原行为）
                std::cout << "Received audio frame: "
                          << "timestamp=" << audio.timestamp()
                          << ", sample_rate=" << audio.sample_rate()
                          << ", channels=" << audio.channels()
                          << ", samples=" << audio.samples()
                          << ", data_size=" << audio.audio_data().size() << " bytes"
                          << std::endl;

                // 2. 写入文件（新增）
                if (log_file_.is_open()) {
                    log_file_ << "timestamp=" << audio.timestamp()
                              << ", sample_rate=" << audio.sample_rate()
                              << ", channels=" << audio.channels()
                              << ", samples=" << audio.samples()
                              << ", data_size=" << audio.audio_data().size() << " bytes\n";
                    // 可选：实时刷新，以便即使程序意外退出也能保留数据
                    log_file_.flush();
                }
            }
        }
    }

    void on_subscription_matched(DataReader*, const SubscriptionMatchedStatus& info) override {
        if (info.current_count_change == 1) {
            std::cout << "Subscriber matched." << std::endl;
        } else if (info.current_count_change == -1) {
            std::cout << "Subscriber unmatched." << std::endl;
        }
    }

private:
    std::ofstream log_file_;
};

int main() {
    std::cout << "Starting Audio Subscriber..." << std::endl;

    // 1. 创建 DomainParticipant
    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name("AudioSubscriber");
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

    // 3. 创建 Subscriber
    SubscriberQos subqos = SUBSCRIBER_QOS_DEFAULT;
    Subscriber* subscriber = participant->create_subscriber(subqos);
    if (!subscriber) {
        std::cerr << "Failed to create subscriber." << std::endl;
        return 1;
    }

    // 4. 创建 Topic（必须与发布者一致）
    TopicQos tqos = TOPIC_QOS_DEFAULT;
    Topic* topic = participant->create_topic("AudioTopic", "AudioData", tqos);
    if (!topic) {
        std::cerr << "Failed to create topic." << std::endl;
        return 1;
    }

    // 5. 创建 DataReader 并配置 QoS (BEST_EFFORT, 深度 30)
    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    rqos.history().depth = 30;

    // 创建监听器，指定日志文件路径（放在 /tmp 下）
    SubListener listener("/tmp/audio_data.log");
    DataReader* reader = subscriber->create_datareader(topic, rqos, &listener);
    if (!reader) {
        std::cerr << "Failed to create data reader." << std::endl;
        return 1;
    }

    std::cout << "Subscriber running. Press Enter to stop..." << std::endl;
    std::cout << "Data will also be written to /tmp/audio_data.log" << std::endl;

    // 等待用户按回车退出（保留原始交互）
    std::cin.ignore();

    // 清理资源
    subscriber->delete_datareader(reader);
    participant->delete_subscriber(subscriber);
    participant->delete_topic(topic);
    DomainParticipantFactory::get_instance()->delete_participant(participant);

    std::cout << "Audio Subscriber stopped." << std::endl;
    return 0;
}