#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct preview_image {
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
};

// One joined worker, bounded queues, and one pending snapshot per asset. Work
// owns its inputs; it must never access ImGui, OpenGL, or mutable editor state.
class preview_worker {
public:
    struct result { std::string key; uint64_t revision; preview_image image; };
    using task = std::function<preview_image()>;
    static constexpr size_t capacity = 32;
    ~preview_worker() { stop(); }

    bool request(std::string key, uint64_t revision, task work, bool priority = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        for (auto& job : pending_) if (job.key == key) {
            job = {std::move(key),revision,std::move(work)};
            return true;
        }
        if (pending_.size() >= capacity) return false;
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
        job next{std::move(key),revision,std::move(work)};
        if (priority) pending_.push_front(std::move(next));
        else pending_.push_back(std::move(next));
        ready_.notify_one();
        return true;
    }
    std::vector<result> take_results() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<result> results; results.swap(completed_); ready_.notify_one(); return results;
    }
    void stop() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stopping_ = true; pending_.clear(); ready_.notify_one();
        }
        if (thread_.joinable()) thread_.join();
    }
private:
    struct job { std::string key; uint64_t revision; task work; };
    std::mutex mutex_;
    std::condition_variable ready_;
    std::thread thread_;
    bool stopping_ = false;
    std::deque<job> pending_;
    std::vector<result> completed_;
    void run() {
        for (;;) {
            job next;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock,[&] { return stopping_ || !pending_.empty(); });
                if (stopping_) return;
                next = std::move(pending_.front()); pending_.pop_front();
            }
            preview_image image;
            try { image = next.work(); } catch (...) { /* Keep the last valid preview. */ }
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopping_) return;
            for (auto it = completed_.begin(); it != completed_.end(); ++it)
                if (it->key == next.key) { completed_.erase(it); break; }
            // Backpressure the worker, never the caller. Do not silently drop a
            // result: its thumbnail might not become visible again for minutes.
            ready_.wait(lock,[&] { return stopping_ || completed_.size() < capacity; });
            if (stopping_) return;
            completed_.push_back({std::move(next.key),next.revision,std::move(image)});
        }
    }
};
