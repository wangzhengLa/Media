#include "chalk.h"

namespace chalk {

// ============================================================================
// SubscriberRegistry
// ============================================================================
uint64_t SubscriberRegistry::addKey(const std::string& key, ParamCallback cb) {
    const uint64_t id = next_id_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    key_subs_[key].push_back({id, std::move(cb)});
    return id;
}

uint64_t SubscriberRegistry::addWildcard(ParamCallback cb) {
    const uint64_t id = next_id_.fetch_add(1);
    std::lock_guard<std::mutex> lock(mutex_);
    wildcard_subs_.push_back({id, std::move(cb)});
    return id;
}

void SubscriberRegistry::remove(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = key_subs_.begin(); it != key_subs_.end();) {
        auto& vec = it->second;
        for (auto vit = vec.begin(); vit != vec.end(); ++vit) {
            if (vit->first == id) { vec.erase(vit); break; }
        }
        if (vec.empty()) it = key_subs_.erase(it);
        else             ++it;
    }

    for (auto it = wildcard_subs_.begin(); it != wildcard_subs_.end(); ++it) {
        if (it->first == id) { wildcard_subs_.erase(it); break; }
    }
}

void SubscriberRegistry::fire(const std::string& key, const ParamValue& value) {
    std::vector<ParamCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = key_subs_.find(key);
        if (it != key_subs_.end()) {
            callbacks.reserve(it->second.size() + wildcard_subs_.size());
            for (auto& p : it->second) callbacks.push_back(p.second);
        } else {
            callbacks.reserve(wildcard_subs_.size());
        }
        for (auto& p : wildcard_subs_) callbacks.push_back(p.second);
    }

    for (auto& cb : callbacks) {
        try { cb(key, value); }
        catch (const std::exception& e) {
            ump_warn("blackboard subscriber exception on %s: %s",
                     key.c_str(), e.what());
        } catch (...) {
            ump_warn("blackboard subscriber unknown exception on %s", key.c_str());
        }
    }
}

// ============================================================================
// Blackboard
// ============================================================================
Blackboard::Blackboard()
    : registry_(std::make_shared<SubscriberRegistry>()) {}

Blackboard::Blackboard(Ptr parent)
    : parent_(std::move(parent)),
      registry_(std::make_shared<SubscriberRegistry>()) {}

Blackboard::Ptr Blackboard::create() {
    return Ptr(new Blackboard());
}

Blackboard::Ptr Blackboard::create(Ptr parent) {
    return Ptr(new Blackboard(std::move(parent)));
}

Blackboard::Ptr Blackboard::createChild() {
    return create(shared_from_this());
}

void Blackboard::setValue(const std::string& key, const ParamValue& value) {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        values_[key] = value;
    }
    registry_->fire(key, value);
}

boost::optional<ParamValue> Blackboard::getValue(const std::string& key) const {
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = values_.find(key);
        if (it != values_.end()) return it->second;
    }
    if (parent_) return parent_->getValue(key);
    return boost::none;
}

bool Blackboard::has(const std::string& key) const {
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (values_.find(key) != values_.end()) return true;
    }
    if (parent_) return parent_->has(key);
    return false;
}

bool Blackboard::hasLocal(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return values_.find(key) != values_.end();
}

void Blackboard::erase(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    values_.erase(key);
}

std::vector<std::string> Blackboard::keys() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(values_.size());
    for (auto& kv : values_) out.push_back(kv.first);
    return out;
}

Subscription Blackboard::watch(const std::string& key, ParamCallback cb) {
    auto registry = registry_;
    const uint64_t id = registry->addKey(key, std::move(cb));
    return Subscription([registry, id]() { registry->remove(id); });
}

Subscription Blackboard::watchAll(ParamCallback cb) {
    auto registry = registry_;
    const uint64_t id = registry->addWildcard(std::move(cb));
    return Subscription([registry, id]() { registry->remove(id); });
}

// ============================================================================
// ThreadPool
// ============================================================================
ThreadPool::ThreadPool(size_t threads, size_t max_queue)
    : stop_(false),
      max_queue_(max_queue ? max_queue : 500),
      pending_count_(0),
      drop_count_(0) {
    if (threads == 0) threads = 1;
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    condition_.wait(lock, [this] {
                        return stop_ || !tasks_.empty();
                    });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                    pending_count_--;
                }
                try { task(); }
                catch (const std::exception& e) {
                    ump_warn("ThreadPool task exception: %s", e.what());
                } catch (...) {
                    ump_warn("ThreadPool task unknown exception");
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::stop() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (stop_) return;
        stop_ = true;
    }
    condition_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

void ThreadPool::setOverflowCallback(std::function<void(size_t)> cb) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    overflow_callback_ = std::move(cb);
}

// ============================================================================
// MessageBus
// ============================================================================
MessageBus::MessageBus(size_t async_threads, size_t max_queue)
    : pool_(std::make_shared<ThreadPool>(async_threads, max_queue)) {}

MessageBus::~MessageBus() {
    if (pool_) pool_->stop();
}

void MessageBus::setOverflowCallback(std::function<void(size_t)> cb) {
    std::shared_ptr<ThreadPool> pool_snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        pool_snapshot = pool_;
    }
    if (pool_snapshot) pool_snapshot->setOverflowCallback(std::move(cb));
}

size_t MessageBus::pendingTasks() const {
    std::shared_ptr<ThreadPool> pool_snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        pool_snapshot = pool_;
    }
    return pool_snapshot ? pool_snapshot->pendingTasks() : 0;
}

size_t MessageBus::subscriberCount(const std::string& topic) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    size_t n = 0;
    auto it = subs_.find(topic);
    if (it != subs_.end()) n += it->second.size();
    auto it2 = async_subs_.find(topic);
    if (it2 != async_subs_.end()) n += it2->second.size();
    return n;
}

uint64_t MessageBus::addSyncSub(const std::string& topic,
                                std::function<void(const boost::any&)> cb) {
    const uint64_t id = next_sub_id_.fetch_add(1);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    subs_[topic].push_back(SubEntry{id, std::move(cb)});
    return id;
}

uint64_t MessageBus::addAsyncSub(const std::string& topic,
                                 std::function<void(const boost::any&)> cb) {
    const uint64_t id = next_sub_id_.fetch_add(1);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    async_subs_[topic].push_back(SubEntry{id, std::move(cb)});
    return id;
}

void MessageBus::removeSub(const std::string& topic, uint64_t id) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    auto remove_from = [&](SubMap& m) {
        auto it = m.find(topic);
        if (it == m.end()) return;
        auto& vec = it->second;
        for (auto vit = vec.begin(); vit != vec.end(); ++vit) {
            if (vit->id == id) { vec.erase(vit); break; }
        }
        if (vec.empty()) m.erase(it);
    };
    remove_from(subs_);
    remove_from(async_subs_);
}

}  // namespace chalk
