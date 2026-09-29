#ifndef __ROS2_EXECUTOR_H__
#define __ROS2_EXECUTOR_H__

#if defined(ENABLE_ROS2)
#include <memory>
#include <mutex>
#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>

class GlobalExecutor
{
public:
    // 获取单例实例
    static GlobalExecutor &getInstance()
    {
        static GlobalExecutor instance;
        return instance;
    }

    // 添加节点 (线程安全)
    void addNode(rclcpp::Node::SharedPtr node)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!executor_thread_)
        {
            startExecutor(); // 首次添加节点时自动启动执行器
        }
        executor_.add_node(node);
    }

    // 移除节点 (线程安全)
    void removeNode(rclcpp::Node::SharedPtr node)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        executor_.remove_node(node);
    }

    // 停止执行器
    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (executor_thread_)
        {
            executor_.cancel();
            executor_thread_->join();
            executor_thread_.reset();
        }
    }

    // 禁止复制和移动
    GlobalExecutor(const GlobalExecutor &) = delete;
    GlobalExecutor &operator=(const GlobalExecutor &) = delete;

private:
    rclcpp::executors::MultiThreadedExecutor executor_;
    std::unique_ptr<std::thread> executor_thread_;
    std::mutex mutex_;

    // 私有构造函数
    GlobalExecutor()
        : executor_(rclcpp::executors::MultiThreadedExecutor(
              rclcpp::ExecutorOptions(), std::thread::hardware_concurrency())) {}

    // 启动执行器线程
    void startExecutor()
    {
        executor_thread_ = std::make_unique<std::thread>([this]()
                                                         {
      RCLCPP_INFO(rclcpp::get_logger("GlobalExecutor"),
                  "Starting multi-threaded executor with %zu threads",
                  this->executor_.get_number_of_threads());
      this->executor_.spin();
      RCLCPP_INFO(rclcpp::get_logger("GlobalExecutor"), "Executor stopped"); });
    }

    // 析构时自动清理
    ~GlobalExecutor() { shutdown(); }
};
#endif

#endif // __ROS2_EXECUTOR_H__