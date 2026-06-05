#include "thread_pool.hpp"
#include "rs_log.hpp"

namespace middleware {

ThreadPool::ThreadPool(size_t num_threads) {
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back();
        Worker& worker = workers_.back();
        worker.thread = std::thread([this]() {
            auto pop_task_locked = [this](TaskFunc& task) -> bool {
                if (!urgent_queue_.empty()) {
                    task = std::move(urgent_queue_.front());
                    urgent_queue_.pop();
                    return true;
                }
                if (!normal_queue_.empty()) {
                    task = std::move(normal_queue_.front());
                    normal_queue_.pop();
                    return true;
                }
                return false;
            };

            while (running_.load()) {
                TaskFunc task;

                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    cv_.wait(lock, [this]() {
                        return !running_.load() ||
                               !urgent_queue_.empty() ||
                               !normal_queue_.empty();
                    });

                    if (!running_.load() && urgent_queue_.empty() && normal_queue_.empty()) {
                        break;
                    }

                    if (!pop_task_locked(task)) {
                        continue;
                    }
                }

                active_count_.fetch_add(1);
                task();
                active_count_.fetch_sub(1);
            }

            while (true) {
                TaskFunc task;
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    if (!pop_task_locked(task)) {
                        break;
                    }
                }
                active_count_.fetch_add(1);
                task();
                active_count_.fetch_sub(1);
            }
        });
    }
    UPC_LOG_THREAD_POOL(Info, "init threads=%zu queue_depth=%d",
        num_threads, THREAD_POOL_QUEUE_DEPTH);
}

ThreadPool::~ThreadPool() {
    shutdown();
}

bool ThreadPool::submit(TaskFunc task) {
    if (!task || !running_.load()) return false;
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (normal_queue_.size() >= THREAD_POOL_QUEUE_DEPTH) {
            UPC_LOG_THREAD_POOL(Error, "submit FAIL (queue full depth=%zu)",
                normal_queue_.size());
            return false;
        }
        normal_queue_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

bool ThreadPool::submit_urgent(TaskFunc task) {
    if (!task || !running_.load()) return false;
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (urgent_queue_.size() >= THREAD_POOL_QUEUE_DEPTH / 4) {
            UPC_LOG_THREAD_POOL(Error, "submit_urgent FAIL (urgent queue full)");
            return false;
        }
        urgent_queue_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

size_t ThreadPool::queue_depth() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return normal_queue_.size() + urgent_queue_.size();
}

size_t ThreadPool::active_count() const {
    return active_count_.load();
}

size_t ThreadPool::thread_count() const noexcept {
    return workers_.size();
}

void ThreadPool::shutdown() {
    UPC_LOG_THREAD_POOL(Info, "shutdown threads=%zu queued=%zu active=%zu",
        workers_.size(), queue_depth(), active_count());
    running_.store(false);
    cv_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
    workers_.clear();
}

} // namespace middleware
