#include "service_base.hpp"
#include "msg_port.hpp"
#include "rs_log.hpp"
#include "msg_types.hpp"
#include <cstdio>
#include <iostream>

namespace services {

namespace {
constexpr size_t kWorkerQueueDepth = 64;
} // namespace

ServiceBase::ServiceBase(const ServiceConfig& config) : config_(config) {}

ServiceBase::~ServiceBase() {
    // 若服务仍在运行，先正常停止
    if (state() == ServiceState::Running) {
        stop();
    }
    // 兜底：若工作线程仍在 joinable，强制唤醒并回收
    if (worker_thread_.joinable()) {
        worker_running_.store(false);
        worker_cv_.notify_all();
        worker_thread_.join();
    }
}

bool ServiceBase::init() {
    if (state() != ServiceState::Idle) return false;
    
    state_ = ServiceState::Initializing;
    bool ok = do_init();
    state_ = ok ? ServiceState::Idle : ServiceState::Error;
    return ok;
}

bool ServiceBase::start() {
    if (state() != ServiceState::Idle) return false;
    
    state_ = ServiceState::Running;
    bool ok = do_start();
    if (!ok) {
        state_ = ServiceState::Error;
        return false;
    }
    
    // 若配置了独立工作线程，启动之
    if (config_.use_worker_thread) {
        worker_running_.store(true);
        worker_thread_ = std::thread(&ServiceBase::worker_loop, this);
    }
    
    return true;
}

bool ServiceBase::stop() {
    if (state() != ServiceState::Running) return false;
    
    state_ = ServiceState::Stopping;
    
    // 若使用了工作线程，先停止线程再清理资源
    if (config_.use_worker_thread && worker_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            worker_running_.store(false);
        }
        worker_cv_.notify_all();
        worker_thread_.join();
        
        // 清空残留消息（pop 会销毁 Message，MsgPayload RAII 释放 BufferRef）
        std::lock_guard<std::mutex> lock(worker_mutex_);
        while (!worker_queue_.empty()) {
            worker_queue_.pop();
        }
    }
    
    bool ok = do_stop();
    subscriptions_.clear();  // 取消所有订阅
    state_ = ok ? ServiceState::Idle : ServiceState::Error;
    return ok;
}

ServiceState ServiceBase::state() const noexcept {
    return state_.load();
}

std::string ServiceBase::name() const {
    return config_.name;
}

uint32_t ServiceBase::module_id() const noexcept {
    return config_.module_id;
}

bool ServiceBase::use_worker_thread() const noexcept {
    return config_.use_worker_thread;
}

void ServiceBase::on_message(const middleware::Message& msg) {
    if (state() != ServiceState::Running) return;
    
    if (config_.use_worker_thread) {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            if (worker_queue_.size() >= kWorkerQueueDepth) {
                UPC_LOG_SERVICE_BASE(Warn,
                    "worker queue full (%zu), drop type=%u service=%s",
                    kWorkerQueueDepth,
                    static_cast<unsigned>(static_cast<uint32_t>(msg.type)),
                    config_.name.c_str());
                return;
            }
            worker_queue_.push(msg);
        }
        worker_cv_.notify_one();
    } else {
        // 直接在当前线程（LVGL/UI 线程）中处理
        handle_message(msg);
    }
}

void ServiceBase::worker_loop() {
    while (worker_running_.load()) {
        middleware::Message msg;
        
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [this]() {
                return !worker_running_.load() || !worker_queue_.empty();
            });
            
            if (!worker_running_.load() && worker_queue_.empty()) {
                break;
            }
            
            if (worker_queue_.empty()) {
                continue;
            }
            
            msg = std::move(worker_queue_.front());
            worker_queue_.pop();
        }
        
        // 在工作线程中处理消息
        // 注意：此处调用 handle_message() 时不在 LVGL/UI 线程，
        // 若需回发消息到 UI，请使用 publish_async()，切勿直接调用 publish()
        if (state() == ServiceState::Running) {
            handle_message(msg);
        }
    }
}

void ServiceBase::publish(const middleware::Message& msg) {
    middleware::MsgPort::instance().publish(msg);
}

bool ServiceBase::publish_async(const middleware::Message& msg) {
    return middleware::MsgPort::instance().publish_async(msg);
}

void ServiceBase::subscribe(middleware::MsgType type, middleware::MsgHandler handler) {
    auto sub = middleware::MsgPort::instance().subscribe(type,
        static_cast<middleware::ModuleId>(config_.module_id), std::move(handler));
    subscriptions_.push_back(std::move(sub));
}

} // namespace services
