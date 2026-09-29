#ifndef __FRAME_SUBSCRIBER_H__
#define __FRAME_SUBSCRIBER_H__

#if defined(ENABLE_ROS2)
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/image.hpp"

class FrameSubscriber : public rclcpp::Node
{
public:
    using Callback =
        std::function<void(const sensor_msgs::msg::Image::SharedPtr &)>;

    explicit FrameSubscriber(const std::string &node_name) : Node(node_name) {}

    void subscribe_image(const std::string &topic_name, Callback callback)
    {
        if (topic_name.empty())
        {
            RCLCPP_ERROR(get_logger(), "Topic name cannot be empty");
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // 检查是否已存在订阅
        if (subscriptions_.count(topic_name) > 0)
        {
            RCLCPP_WARN(get_logger(), "Already subscribed to topic: %s",
                        topic_name.c_str());
            return;
        }

        try
        {
            // 创建订阅并保存回调
            auto subscription = this->create_subscription<sensor_msgs::msg::Image>(
                topic_name, 10,
                [this, topic_name](const sensor_msgs::msg::Image::SharedPtr msg)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (auto it = callbacks_.find(topic_name); it != callbacks_.end())
                    {
                        it->second(msg);
                    }
                });

            subscriptions_[topic_name] = subscription;
            callbacks_[topic_name] = callback;
            RCLCPP_INFO(get_logger(), "Subscribed to topic: %s", topic_name.c_str());
        }
        catch (const std::exception &e)
        {
            RCLCPP_ERROR(get_logger(), "Failed to create subscription: %s", e.what());
            throw;
        }
    }

    void delete_subscription(const std::string &topic_name)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (auto it = subscriptions_.find(topic_name); it != subscriptions_.end())
        {
            subscriptions_.erase(it);
            callbacks_.erase(topic_name);
            RCLCPP_INFO(get_logger(), "Unsubscribed from topic: %s",
                        topic_name.c_str());
        }
        else
        {
            RCLCPP_WARN(get_logger(), "Topic not found: %s", topic_name.c_str());
        }
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string,
                       rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr>
        subscriptions_;
    std::unordered_map<std::string, Callback> callbacks_;
};
#endif // ENABLE_ROS2

#endif // __FRAME_SUBSCRIBER_H__