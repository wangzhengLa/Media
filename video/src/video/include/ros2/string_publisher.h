#ifndef __STRING_PUBLISHER_H__
#define __STRING_PUBLISHER_H__

#if defined(ENABLE_ROS2)
#include <memory>
#include <mutex>
#include <unordered_map>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

class StringPublisher : public rclcpp::Node
{
public:
    explicit StringPublisher(const std::string &node_name) : Node(node_name) {}

    void publish_string(const std::string &topic_name,
                        std::unique_ptr<std_msgs::msg::String> msg)
    {
        if (topic_name.empty())
        {
            RCLCPP_ERROR(get_logger(), "Topic name cannot be empty");
            return;
        }

        if (!msg)
        {
            RCLCPP_ERROR(get_logger(), "Message is null for topic: %s",
                         topic_name.c_str());
            return;
        }

        if (msg->data.empty())
        {
            RCLCPP_ERROR(get_logger(), "String data is empty for topic: %s",
                         topic_name.c_str());
            return;
        }

        auto publisher = get_publisher(topic_name);
        publisher->publish(std::move(msg));
    }

    void delete_publisher(const std::string &topic_name)
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = publishers_.find(topic_name);
        if (it != publishers_.end())
        {
            publishers_.erase(it);
            RCLCPP_INFO(get_logger(), "Deleted publisher for topic: %s",
                        topic_name.c_str());
        }
        else
        {
            RCLCPP_WARN(get_logger(), "Topic not found for deletion: %s",
                        topic_name.c_str());
        }
    }

private:
    std::mutex map_mutex_;
    std::unordered_map<std::string,
                       rclcpp::Publisher<std_msgs::msg::String>::SharedPtr>
        publishers_;

    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr get_publisher(
        const std::string &topic_name)
    {
        std::lock_guard<std::mutex> lock(map_mutex_);

        auto it = publishers_.find(topic_name);
        if (it != publishers_.end())
        {
            return it->second;
        }

        try
        {
            auto new_publisher =
                this->create_publisher<std_msgs::msg::String>(topic_name, 10);

            publishers_[topic_name] = new_publisher;
            RCLCPP_INFO(get_logger(), "Created new publisher for topic: %s",
                        topic_name.c_str());
            return new_publisher;
        }
        catch (const std::exception &e)
        {
            RCLCPP_ERROR(get_logger(), "Failed to create publisher: %s", e.what());
            throw;
        }
    }
};
#endif // ENABLE_ROS2

#endif // __STRING_PUBLISHER_H__