// inproc_comm.h
#pragma once

#include <iostream>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <future>
#include <chrono>
#include <string>
#include <cstdint>
#include <typeindex>

#include <boost/any.hpp>
#include <boost/optional.hpp>
#include "log.h"

class ParamValue;
struct Empty
{
};

// ============================================================================
// 优化版线程池（带队列限制和原子计数）
// ============================================================================
class ThreadPool
{
public:
    explicit ThreadPool(size_t threads = 2) : stop_(false), pending_count_(0)
    {
        for (size_t i = 0; i < threads; ++i)
        {
            workers_.emplace_back([this]
                                  {
                while (true)
                {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex_);
                        condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                        pending_count_--;
                    }
                    task();
                } });
        }
    }

    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto &worker : workers_)
        {
            if (worker.joinable())
                worker.join();
        }
    }

    // 设置队列溢出回调（可选）
    void setOverflowCallback(std::function<void(size_t)> cb)
    {
        overflow_callback_ = cb;
    }

    template <typename F>
    bool enqueue(F &&f)
    {
        const size_t MAX_QUEUE_SIZE = 500;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (tasks_.size() >= MAX_QUEUE_SIZE)
            {
                static int drop_count = 0;
                if (++drop_count % 100 == 0)
                {
                    ump_warn("ThreadPool queue full (%zu), dropped %d tasks",
                             tasks_.size(), drop_count);
                }
                if (overflow_callback_)
                {
                    overflow_callback_(tasks_.size());
                }
                return false;
            }
            tasks_.emplace(std::forward<F>(f));
            pending_count_++;
        }
        condition_.notify_one();
        return true;
    }

    size_t pendingTasks() const
    {
        return pending_count_.load();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_;
    std::atomic<size_t> pending_count_;
    std::function<void(size_t)> overflow_callback_;
};

// ============================================================================
// 参数值类
// ============================================================================
class ParamValue
{
public:
    ParamValue() = default;
    template <typename T>
    ParamValue(const T &value);
    template <typename T>
    const T *as() const;
    template <typename T>
    T *as();
    template <typename T>
    bool is() const;
    const std::type_index &type() const { return type_index_; }
    bool empty() const { return value_.empty(); }

private:
    boost::any value_;
    std::type_index type_index_ = std::type_index(typeid(void));
};

using ParamCallback = std::function<void(const std::string &, const ParamValue &)>;

// ============================================================================
// 节点类
// ============================================================================
class SimpleNode : public std::enable_shared_from_this<SimpleNode>
{
public:
    explicit SimpleNode(const std::string &name);
    ~SimpleNode();
    void shutdown();

    template <typename T>
    void setParam(const std::string &name, const T &value);
    template <typename T>
    boost::optional<T> getParam(const std::string &name);
    boost::optional<ParamValue> getParam(const std::string &name);
    bool hasParam(const std::string &name);
    bool deleteParam(const std::string &name);
    std::vector<std::string> getParamNames();
    void watchParam(const std::string &name, ParamCallback callback);
    void watchAllParams(ParamCallback callback);

    template <typename Req, typename Resp>
    void serve(const std::string &name, std::function<Resp(const Req &)> handler);
    template <typename Req, typename Resp>
    boost::optional<Resp> call(const std::string &service, const Req &request, int timeout_ms = 5000);
    template <typename Req, typename Resp>
    std::future<Resp> async(const std::string &service, const Req &request);

    std::string name() const { return name_; }
    bool isRunning() const { return running_.load(); }

private:
    struct Request
    {
        std::string service;
        uint64_t id;
        boost::any data;
    };
    void setParamValue(const std::string &name, const ParamValue &value);
    void initParameterServices();
    void workLoop();

    std::string name_;
    std::atomic<bool> running_;
    std::thread worker_;
    std::unordered_map<std::string, std::function<boost::any(const boost::any &)>> services_;
    std::mutex services_mutex_;
    std::queue<Request> queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::unordered_map<uint64_t, std::function<void(const boost::any &)>> pending_;
    std::mutex pending_mutex_;
    std::atomic<uint64_t> next_id_;
    std::unordered_map<std::string, ParamValue> params_;
    std::unordered_map<std::string, std::vector<ParamCallback>> param_callbacks_;
    std::vector<ParamCallback> wildcard_callbacks_;
    std::mutex params_mutex_;
};

// ============================================================================
// 节点管理器
// ============================================================================
class NodeManager
{
public:
    static NodeManager &instance();
    std::shared_ptr<SimpleNode> createNode(const std::string &name);
    void removeNode(const std::string &name);
    void removeAllNodes();
    bool nodeExists(const std::string &name);

private:
    NodeManager() = default;
    ~NodeManager() = default;
    NodeManager(const NodeManager &) = delete;
    NodeManager &operator=(const NodeManager &) = delete;
    std::unordered_map<std::string, std::weak_ptr<SimpleNode>> nodes_;
    std::mutex mutex_;
};

// ============================================================================
// 独立消息总线（支持同步/异步订阅）
// ============================================================================
class MessageBus
{
public:
    static MessageBus &instance();

    /**
     * @brief 同步订阅（回调在发布者线程中执行）
     *
     * ✅ 适用：极轻量操作（统计计数、日志打印）
     * ❌ 禁用：耗时操作（编码、文件IO），会阻塞发布者
     */
    template <typename T>
    void subscribe(const std::string &topic, std::function<void(const T &)> callback)
    {
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        subs_[topic].push_back([callback](const boost::any &data)
                               {
            const T* value = boost::any_cast<const T>(&data);
            if (value) callback(*value); });
    }

    /**
     * @brief 异步订阅（回调在线程池中执行）
     *
     * ✅ 适用：耗时操作（编码、文件IO、网络发送）
     * ⚠️ 注意：回调执行顺序不保证
     */
    template <typename T>
    void subscribe_async(const std::string &topic, std::function<void(const T &)> callback)
    {
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        async_subs_[topic].push_back([callback](const boost::any &data)
                                     {
            const T* value = boost::any_cast<const T>(&data);
            if (value) callback(*value); });
    }

    /**
     * @brief 发布消息
     *
     * 执行顺序：同步订阅者 -> 异步订阅者提交到线程池
     *
     * @tparam T 消息类型
     * @param topic 主题名称
     * @param data 消息数据（大数据建议用 shared_ptr）
     */
    template <typename T>
    void publish(const std::string &topic, const T &data)
    {
        boost::any any_data(data);

        // 1. 处理同步订阅者（复制回调列表，避免死锁）
        {
            std::shared_lock<std::shared_mutex> lock(rw_mutex_);
            auto it = subs_.find(topic);
            if (it != subs_.end())
            {
                auto callbacks = it->second;
                lock.unlock();
                for (auto &cb : callbacks)
                {
                    cb(any_data);
                }
            }
        }

        // 2. 处理异步订阅者（提交到线程池）
        {
            std::shared_lock<std::shared_mutex> lock(rw_mutex_);
            auto it = async_subs_.find(topic);
            if (it != async_subs_.end())
            {
                auto callbacks = it->second;
                lock.unlock();
                for (auto &cb : callbacks)
                {
                    pool_->enqueue([cb, any_data]()
                                   { cb(any_data); });
                }
            }
        }
    }

    // 设置线程池大小（必须在第一次发布前调用）
    void setThreadPoolSize(size_t threads)
    {
        if (!pool_initialized_)
        {
            pool_ = std::make_unique<ThreadPool>(threads);
            pool_initialized_ = true;
        }
        else
        {
            ump_warn("ThreadPool already initialized, cannot resize");
        }
    }

    // 设置队列溢出回调
    void setOverflowCallback(std::function<void(size_t)> cb)
    {
        if (pool_)
        {
            pool_->setOverflowCallback(cb);
        }
    }

    // 获取线程池待处理任务数
    size_t pendingTasks() const
    {
        return pool_ ? pool_->pendingTasks() : 0;
    }

private:
    MessageBus()
        : pool_(std::make_unique<ThreadPool>(2)), pool_initialized_(true) {}
    ~MessageBus() = default;
    MessageBus(const MessageBus &) = delete;
    MessageBus &operator=(const MessageBus &) = delete;

    std::unordered_map<std::string, std::vector<std::function<void(const boost::any &)>>> subs_;
    std::unordered_map<std::string, std::vector<std::function<void(const boost::any &)>>> async_subs_;
    mutable std::shared_mutex rw_mutex_;
    std::unique_ptr<ThreadPool> pool_;
    bool pool_initialized_;
};

// ============================================================================
// 模板实现
// ============================================================================
template <typename T>
ParamValue::ParamValue(const T &value) : value_(value), type_index_(std::type_index(typeid(T))) {}

template <typename T>
const T *ParamValue::as() const { return boost::any_cast<const T>(&value_); }

template <typename T>
T *ParamValue::as() { return boost::any_cast<T>(&value_); }

template <typename T>
bool ParamValue::is() const { return type_index_ == std::type_index(typeid(T)); }

template <typename Req, typename Resp>
void SimpleNode::serve(const std::string &name, std::function<Resp(const Req &)> handler)
{
    std::lock_guard<std::mutex> lock(services_mutex_);
    services_[name] = [handler](const boost::any &req) -> boost::any
    {
        const Req *request = boost::any_cast<const Req>(&req);
        if (request)
            return boost::any(handler(*request));
        return boost::any();
    };
}

template <typename Req, typename Resp>
boost::optional<Resp> SimpleNode::call(const std::string &service, const Req &request, int timeout_ms)
{
    auto promise = std::make_shared<std::promise<Resp>>();
    std::future<Resp> future = promise->get_future();
    uint64_t id = next_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_[id] = [promise](const boost::any &resp)
        {
            const Resp *value = boost::any_cast<const Resp>(&resp);
            if (value)
                promise->set_value(*value);
            else
                promise->set_exception(std::make_exception_ptr(std::runtime_error("Response type mismatch")));
        };
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push({service, id, boost::any(request)});
    }
    cv_.notify_one();
    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::timeout)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(id);
        return boost::none;
    }
    try
    {
        return future.get();
    }
    catch (...)
    {
        return boost::none;
    }
}

template <typename Req, typename Resp>
std::future<Resp> SimpleNode::async(const std::string &service, const Req &request)
{
    auto promise = std::make_shared<std::promise<Resp>>();
    std::future<Resp> future = promise->get_future();
    uint64_t id = next_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_[id] = [promise](const boost::any &resp)
        {
            const Resp *value = boost::any_cast<const Resp>(&resp);
            if (value)
                promise->set_value(*value);
            else
                promise->set_exception(std::make_exception_ptr(std::runtime_error("Response type mismatch")));
        };
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push({service, id, boost::any(request)});
    }
    cv_.notify_one();
    return future;
}

template <typename T>
void SimpleNode::setParam(const std::string &name, const T &value)
{
    ParamValue new_value(value);
    setParamValue(name, new_value);
}

template <typename T>
boost::optional<T> SimpleNode::getParam(const std::string &name)
{
    std::lock_guard<std::mutex> lock(params_mutex_);
    auto it = params_.find(name);
    if (it != params_.end())
    {
        const T *value = it->second.as<T>();
        if (value)
            return *value;
    }
    return boost::none;
}