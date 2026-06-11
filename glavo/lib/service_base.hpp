#ifndef SERVICE_BASE_HPP
#define SERVICE_BASE_HPP

#include "msg_port.hpp"
#include "msg_types.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

namespace upc::glavo {

// Service 生命周期状态
enum class ServiceState {
    Idle,
    Initializing,
    Running,
    Stopping,
    Error
};

// Service 配置基类
struct ServiceConfig {
    std::string name;
    uint32_t module_id;
    bool use_worker_thread = false;  /**< 是否在独立工作线程中处理消息 */
};

// Service 抽象基类
class ServiceBase {
public:
    explicit ServiceBase(const ServiceConfig& config);
    virtual ~ServiceBase();

    // 禁止拷贝
    ServiceBase(const ServiceBase&) = delete;
    ServiceBase& operator=(const ServiceBase&) = delete;

    // 生命周期
    bool init();
    bool start();
    bool stop();
    
    // 状态查询
    [[nodiscard]] ServiceState state() const noexcept;
    [[nodiscard]] std::string name() const;
    [[nodiscard]] uint32_t module_id() const noexcept;
    [[nodiscard]] bool use_worker_thread() const noexcept;
    
    // 消息处理入口 (由 MsgBus 调用，运行于 LVGL/UI 线程)
    // 若 use_worker_thread 为 true，则将消息转发至内部队列，由工作线程异步处理
    void on_message(const middleware::Message& msg);

protected:
    // 子类实现
    virtual bool do_init() = 0;
    virtual bool do_start() = 0;
    virtual bool do_stop() = 0;
    virtual void handle_message(const middleware::Message& msg) = 0;

    // 工具方法
    void publish(const middleware::Message& msg);
    [[nodiscard]] bool publish_async(const middleware::Message& msg);
    void subscribe(middleware::MsgType type, middleware::MsgHandler handler);

private:
    ServiceConfig config_;
    std::atomic<ServiceState> state_{ServiceState::Idle};
    std::vector<middleware::Subscription> subscriptions_;

    // 工作线程相关（仅当 config_.use_worker_thread == true 时启用）
    std::thread worker_thread_;                    /**< 独立工作线程 */
    std::queue<middleware::Message> worker_queue_; /**< 工作线程内部消息队列 */
    std::mutex worker_mutex_;                      /**< 队列互斥锁 */
    std::condition_variable worker_cv_;            /**< 队列条件变量 */
    std::atomic<bool> worker_running_{false};      /**< 工作线程运行标志 */

    void worker_loop();  /**< 工作线程主循环 */
};

} // namespace upc::glavo

#endif
