#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace middleware {

using TaskFunc = std::function<void()>;

// 任务包装 (支持返回值)
template<typename T>
using TaskFuture = std::future<T>;

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = THREAD_POOL_MAX_THREADS);
    ~ThreadPool();
    
    // 禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    
    // 提交无返回值任务
    bool submit(TaskFunc task);
    
    // 提交有返回值任务
    template<typename F, typename... Args>
    auto submit_future(F&& f, Args&&... args) 
        -> std::future<decltype(f(args...))>;
    
    // 紧急任务 (插队)
    bool submit_urgent(TaskFunc task);
    
    // 状态
    [[nodiscard]] size_t queue_depth() const;
    [[nodiscard]] size_t active_count() const;
    [[nodiscard]] size_t thread_count() const noexcept;
    
    void shutdown();
    
private:
    struct Worker {
        std::thread thread;
    };
    
    std::vector<Worker> workers_;
    
    std::queue<TaskFunc> normal_queue_;
    std::queue<TaskFunc> urgent_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    
    std::atomic<bool> running_{true};
    std::atomic<size_t> active_count_{0};
};

// ============================================
// 模板实现
// ============================================
template<typename F, typename... Args>
auto ThreadPool::submit_future(F&& f, Args&&... args) 
    -> std::future<decltype(f(args...))> {
    
    using ReturnType = decltype(f(args...));
    
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    std::future<ReturnType> result = task->get_future();
    
    bool ok = submit([task]() { (*task)(); });
    if (!ok) {
        // 提交失败，返回无效 future
        return std::future<ReturnType>();
    }
    
    return result;
}

} // namespace middleware

#endif