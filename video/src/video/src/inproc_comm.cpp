#include "inproc_comm.h"

// ============================================================================
// SimpleNode 实现（保持不变）
// ============================================================================
SimpleNode::SimpleNode(const std::string &name) : name_(name), running_(true), next_id_(0)
{
    worker_ = std::thread(&SimpleNode::workLoop, this);
    initParameterServices();
}

SimpleNode::~SimpleNode() { shutdown(); }

void SimpleNode::shutdown()
{
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false))
    {
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }
}

void SimpleNode::setParamValue(const std::string &name, const ParamValue &value)
{
    std::vector<ParamCallback> callbacks;
    std::vector<ParamCallback> wildcards;
    {
        std::lock_guard<std::mutex> lock(params_mutex_);
        params_[name] = value;
        auto it = param_callbacks_.find(name);
        if (it != param_callbacks_.end())
            callbacks = it->second;
        wildcards = wildcard_callbacks_;
    }
    for (auto &cb : callbacks)
        cb(name, value);
    for (auto &cb : wildcards)
        cb(name, value);
}

void SimpleNode::initParameterServices()
{
    serve<std::string, ParamValue>("get_param", [this](const std::string &name) -> ParamValue
                                   {
        std::lock_guard<std::mutex> lock(params_mutex_);
        auto it = params_.find(name);
        if (it != params_.end()) return it->second;
        return ParamValue(); });
    serve<std::pair<std::string, ParamValue>, bool>("set_param", [this](const std::pair<std::string, ParamValue> &req) -> bool
                                                    {
        setParamValue(req.first, req.second);
        return true; });
    serve<Empty, std::vector<std::string>>("list_params", [this](Empty) -> std::vector<std::string>
                                           { return getParamNames(); });
    serve<std::string, bool>("has_param", [this](const std::string &name) -> bool
                             { return hasParam(name); });
    serve<std::string, bool>("delete_param", [this](const std::string &name) -> bool
                             { return deleteParam(name); });
}

void SimpleNode::workLoop()
{
    while (running_.load())
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [this]
                     { return !queue_.empty() || !running_.load(); });
        if (!running_.load() && queue_.empty())
            break;
        std::queue<Request> local_queue;
        std::swap(local_queue, queue_);
        lock.unlock();
        while (!local_queue.empty())
        {
            Request req = local_queue.front();
            local_queue.pop();
            boost::any resp;
            {
                std::lock_guard<std::mutex> lock(services_mutex_);
                auto it = services_.find(req.service);
                if (it != services_.end())
                    resp = it->second(req.data);
            }
            if (!resp.empty())
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                auto it = pending_.find(req.id);
                if (it != pending_.end())
                {
                    it->second(resp);
                    pending_.erase(it);
                }
            }
        }
    }
}

boost::optional<ParamValue> SimpleNode::getParam(const std::string &name)
{
    std::lock_guard<std::mutex> lock(params_mutex_);
    auto it = params_.find(name);
    if (it != params_.end())
        return it->second;
    return boost::none;
}

bool SimpleNode::hasParam(const std::string &name)
{
    std::lock_guard<std::mutex> lock(params_mutex_);
    return params_.find(name) != params_.end();
}

bool SimpleNode::deleteParam(const std::string &name)
{
    std::lock_guard<std::mutex> lock(params_mutex_);
    return params_.erase(name) > 0;
}

std::vector<std::string> SimpleNode::getParamNames()
{
    std::lock_guard<std::mutex> lock(params_mutex_);
    std::vector<std::string> names;
    names.reserve(params_.size());
    for (const auto &pair : params_)
        names.push_back(pair.first);
    return names;
}

void SimpleNode::watchParam(const std::string &name, ParamCallback callback)
{
    std::lock_guard<std::mutex> lock(params_mutex_);
    param_callbacks_[name].push_back(callback);
}

void SimpleNode::watchAllParams(ParamCallback callback)
{
    std::lock_guard<std::mutex> lock(params_mutex_);
    wildcard_callbacks_.push_back(callback);
}

// ============================================================================
// NodeManager 实现
// ============================================================================
NodeManager &NodeManager::instance()
{
    static NodeManager inst;
    return inst;
}

std::shared_ptr<SimpleNode> NodeManager::createNode(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodes_.find(name);
    if (it != nodes_.end())
    {
        auto node = it->second.lock();
        if (node && node->isRunning())
            return node;
        nodes_.erase(it);
    }
    auto node = std::make_shared<SimpleNode>(name);
    nodes_[name] = node;
    return node;
}

void NodeManager::removeNode(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodes_.find(name);
    if (it != nodes_.end())
    {
        if (auto node = it->second.lock())
            node->shutdown();
        nodes_.erase(it);
    }
}

void NodeManager::removeAllNodes()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &pair : nodes_)
    {
        if (auto node = pair.second.lock())
            node->shutdown();
    }
    nodes_.clear();
}

bool NodeManager::nodeExists(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodes_.find(name);
    return it != nodes_.end() && !it->second.expired();
}

// ============================================================================
// MessageBus 实现
// ============================================================================
MessageBus &MessageBus::instance()
{
    static MessageBus bus;
    return bus;
}
