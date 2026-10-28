// chalk.h
//
// Chalk: 进程内通信中间件
//   - Blackboard : 层级键值存储 + 变化通知（状态共享）
//   - MessageBus : 发布-订阅事件广播
//
// 关键语义：
//   - Blackboard::set/erase : 只作用于当前黑板，只在当前层触发 watch
//   - Blackboard::get/has   : 沿父链向上查找
//   - Blackboard::watch     : 只监听当前层的 set，不跨作用域
//
// 生命周期约束：
//   - Subscription 必须在 Blackboard / MessageBus 存活期间析构
//   - 模块应先 stop()（reset 所有 Subscription），再销毁 AppContext
//
// 线程模型：
//   - Blackboard：shared_mutex 保护 values_
//   - SubscriberRegistry：mutex 保护订阅表
//   - MessageBus：shared_mutex 保护订阅表，ThreadPool 异步执行
//
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/any.hpp>
#include <boost/optional.hpp>

#if defined(__has_include)
#  if __has_include("log.h")
#    include "log.h"
#  endif
#endif

#ifndef ump_warn
#  define ump_warn(fmt, ...) std::fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef ump_info
#  define ump_info(fmt, ...) std::fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)
#endif

namespace chalk {

// ============================================================================
// ParamValue
// ============================================================================
class ParamValue {
public:
    ParamValue() = default;

    template <typename T>
    ParamValue(const T& value)
        : value_(value), type_index_(std::type_index(typeid(T))) {}

    template <typename T> const T* as() const { return boost::any_cast<const T>(&value_); }
    template <typename T> T*       as()       { return boost::any_cast<T>(&value_); }
    template <typename T> bool is() const { return type_index_ == std::type_index(typeid(T)); }

    const std::type_index& type() const noexcept { return type_index_; }
    bool empty() const noexcept { return value_.empty(); }

private:
    boost::any      value_;
    std::type_index type_index_ = std::type_index(typeid(void));
};

using ParamCallback = std::function<void(const std::string&, const ParamValue&)>;

// ============================================================================
// Subscription RAII
// ============================================================================
class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> unsubscribe)
        : unsubscribe_(std::move(unsubscribe)) {}
    ~Subscription() { reset(); }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& o) noexcept
        : unsubscribe_(std::move(o.unsubscribe_)) { o.unsubscribe_ = nullptr; }

    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) {
            reset();
            unsubscribe_ = std::move(o.unsubscribe_);
            o.unsubscribe_ = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (unsubscribe_) {
            try { unsubscribe_(); } catch (...) {}
            unsubscribe_ = nullptr;
        }
    }

private:
    std::function<void()> unsubscribe_;
};

// ============================================================================
// SubscriberRegistry（Blackboard 内部用）
// ============================================================================
class SubscriberRegistry {
public:
    uint64_t addKey(const std::string& key, ParamCallback cb);
    uint64_t addWildcard(ParamCallback cb);
    void     remove(uint64_t id);
    void     fire(const std::string& key, const ParamValue& value);

private:
    std::mutex mutex_;
    std::unordered_map<std::string,
        std::vector<std::pair<uint64_t, ParamCallback>>> key_subs_;
    std::vector<std::pair<uint64_t, ParamCallback>>      wildcard_subs_;
    std::atomic<uint64_t> next_id_{0};
};

// ============================================================================
// BbError / BbResult
// ============================================================================
enum class BbError {
    Ok = 0,
    NotFound,
    TypeMismatch,
};

template <typename T>
struct BbResult {
    BbError            error = BbError::Ok;
    boost::optional<T> value;

    bool ok() const noexcept { return error == BbError::Ok; }
    explicit operator bool() const noexcept { return ok(); }

    static BbResult make_ok(const T& v) {
        BbResult r; r.error = BbError::Ok; r.value = v; return r;
    }
    static BbResult fail(BbError e) {
        BbResult r; r.error = e; return r;
    }
};

// ============================================================================
// Blackboard
// ============================================================================
class Blackboard : public std::enable_shared_from_this<Blackboard> {
public:
    using Ptr = std::shared_ptr<Blackboard>;

    static Ptr create();
    static Ptr create(Ptr parent);

    template <typename T>
    void set(const std::string& key, const T& value) {
        setValue(key, ParamValue(value));
    }
    void setValue(const std::string& key, const ParamValue& value);

    boost::optional<ParamValue> getValue(const std::string& key) const;

    template <typename T>
    boost::optional<T> get(const std::string& key) const {
        auto v = getValue(key);
        if (!v) return boost::none;
        const T* p = v->as<T>();
        if (!p) return boost::none;
        return *p;
    }

    template <typename T>
    BbResult<T> getResult(const std::string& key) const {
        auto v = getValue(key);
        if (!v) return BbResult<T>::fail(BbError::NotFound);
        const T* p = v->as<T>();
        if (!p) return BbResult<T>::fail(BbError::TypeMismatch);
        return BbResult<T>::make_ok(*p);
    }

    bool has(const std::string& key) const;
    bool hasLocal(const std::string& key) const;
    void erase(const std::string& key);
    std::vector<std::string> keys() const;

    Subscription watch(const std::string& key, ParamCallback cb);
    Subscription watchAll(ParamCallback cb);

    Ptr parent() const { return parent_; }
    Ptr createChild();

private:
    Blackboard();
    explicit Blackboard(Ptr parent);
    Blackboard(const Blackboard&) = delete;
    Blackboard& operator=(const Blackboard&) = delete;

    Ptr                                         parent_;
    mutable std::shared_mutex                   mutex_;
    std::unordered_map<std::string, ParamValue> values_;
    std::shared_ptr<SubscriberRegistry>         registry_;
};

// ============================================================================
// ThreadPool
// ============================================================================
class ThreadPool {
public:
    explicit ThreadPool(size_t threads = 2, size_t max_queue = 500);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void setOverflowCallback(std::function<void(size_t)> cb);
    template <typename F> bool enqueue(F&& f);
    size_t pendingTasks() const noexcept { return pending_count_.load(); }
    size_t maxQueueSize() const noexcept { return max_queue_; }
    uint64_t droppedTasks() const noexcept { return drop_count_.load(); }
    void stop();

private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex                queue_mutex_;
    std::condition_variable           condition_;
    bool                              stop_;
    size_t                            max_queue_;
    std::atomic<size_t>               pending_count_;
    std::atomic<uint64_t>             drop_count_;
    std::function<void(size_t)>       overflow_callback_;
};

// ============================================================================
// MessageBus
// ============================================================================
class MessageBus {
public:
    explicit MessageBus(size_t async_threads = 2, size_t max_queue = 500);
    ~MessageBus();

    MessageBus(const MessageBus&) = delete;
    MessageBus& operator=(const MessageBus&) = delete;

    template <typename T>
    Subscription subscribe(const std::string& topic,
                           std::function<void(const T&)> callback);

    template <typename T>
    Subscription subscribe_async(const std::string& topic,
                                 std::function<void(const T&)> callback);

    template <typename T>
    void publish(const std::string& topic, const T& data);

    void setOverflowCallback(std::function<void(size_t)> cb);
    size_t pendingTasks() const;
    size_t subscriberCount(const std::string& topic) const;

private:
    struct SubEntry {
        uint64_t id;
        std::function<void(const boost::any&)> cb;
    };
    using SubMap = std::unordered_map<std::string, std::vector<SubEntry>>;

    uint64_t addSyncSub(const std::string& topic,
                        std::function<void(const boost::any&)> cb);
    uint64_t addAsyncSub(const std::string& topic,
                         std::function<void(const boost::any&)> cb);
    void     removeSub(const std::string& topic, uint64_t id);

    SubMap                          subs_;
    SubMap                          async_subs_;
    mutable std::shared_mutex       rw_mutex_;
    std::shared_ptr<ThreadPool>     pool_;
    std::atomic<uint64_t>           next_sub_id_{0};
};

// ============================================================================
// AppContext
// ============================================================================
class AppContext {
public:
    explicit AppContext(size_t bus_threads = 2, size_t bus_queue = 500)
        : blackboard_(Blackboard::create()),
          bus_(bus_threads, bus_queue) {}

    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;
    AppContext(AppContext&&) = delete;
    AppContext& operator=(AppContext&&) = delete;

    Blackboard::Ptr blackboard() const { return blackboard_; }
    MessageBus&     bus()              { return bus_; }

private:
    Blackboard::Ptr blackboard_;
    MessageBus      bus_;
};

// ============================================================================
// ThreadPool 模板实现
// ============================================================================
template <typename F>
bool ThreadPool::enqueue(F&& f) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_) return false;
        if (tasks_.size() >= max_queue_) {
            const uint64_t cnt = ++drop_count_;
            if (cnt % 100 == 1) {
                ump_warn("ThreadPool queue full (%zu), dropped %llu tasks",
                         tasks_.size(), static_cast<unsigned long long>(cnt));
            }
            if (overflow_callback_) overflow_callback_(tasks_.size());
            return false;
        }
        tasks_.emplace(std::forward<F>(f));
        pending_count_++;
    }
    condition_.notify_one();
    return true;
}

// ============================================================================
// MessageBus 模板实现
// ============================================================================
template <typename T>
Subscription MessageBus::subscribe(const std::string& topic,
                                   std::function<void(const T&)> callback) {
    auto cb = [callback](const boost::any& data) {
        const T* v = boost::any_cast<const T>(&data);
        if (v) callback(*v);
    };
    const uint64_t id = addSyncSub(topic, std::move(cb));
    return Subscription([this, topic, id]() { removeSub(topic, id); });
}

template <typename T>
Subscription MessageBus::subscribe_async(const std::string& topic,
                                         std::function<void(const T&)> callback) {
    auto cb = [callback](const boost::any& data) {
        const T* v = boost::any_cast<const T>(&data);
        if (v) callback(*v);
    };
    const uint64_t id = addAsyncSub(topic, std::move(cb));
    return Subscription([this, topic, id]() { removeSub(topic, id); });
}

template <typename T>
void MessageBus::publish(const std::string& topic, const T& data) {
    const boost::any any_data(data);

    // ---- 1) 同步订阅 ----
    {
        std::vector<std::function<void(const boost::any&)>> callbacks;
        {
            std::shared_lock<std::shared_mutex> lock(rw_mutex_);
            auto it = subs_.find(topic);
            if (it != subs_.end()) {
                callbacks.reserve(it->second.size());
                for (auto& e : it->second) callbacks.push_back(e.cb);
            }
        }
        for (auto& cb : callbacks) {
            try { cb(any_data); }
            catch (const std::exception& e) {
                ump_warn("sync subscriber exception on %s: %s", topic.c_str(), e.what());
            } catch (...) {
                ump_warn("sync subscriber unknown exception on %s", topic.c_str());
            }
        }
    }

    // ---- 2) 异步订阅 ----
    std::shared_ptr<ThreadPool> pool_snapshot;
    std::vector<std::function<void(const boost::any&)>> callbacks;
    {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        auto it = async_subs_.find(topic);
        if (it == async_subs_.end() || it->second.empty()) return;
        callbacks.reserve(it->second.size());
        for (auto& e : it->second) callbacks.push_back(e.cb);
        pool_snapshot = pool_;
    }
    if (!pool_snapshot) return;
    for (auto& cb : callbacks) {
        pool_snapshot->enqueue([cb, any_data]() {
            try { cb(any_data); }
            catch (const std::exception& e) {
                ump_warn("async subscriber exception: %s", e.what());
            } catch (...) {
                ump_warn("async subscriber unknown exception");
            }
        });
    }
}

}  // namespace chalk
