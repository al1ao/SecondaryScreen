#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <utility>
namespace pds {
template <typename T> class BoundedQueue {
public:
    explicit BoundedQueue(size_t cap) : cap_(cap) {
        if (cap == 0) throw std::invalid_argument("zero queue capacity");
    }
    size_t Push(T item) {
        std::lock_guard lock(m_);
        if (stopped_) return 1;
        size_t dropped = 0;
        if (q_.size() >= cap_) { q_.pop_front(); ++dropped; }
        q_.push_back(std::move(item));
        cv_.notify_one();
        return dropped;
    }
    bool Pop(T& out, std::stop_token st) {
        std::unique_lock lock(m_);
        if (!cv_.wait(lock, st, [this] { return stopped_ || !q_.empty(); })) return false;
        if (stopped_ || st.stop_requested()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void Stop() {
        std::lock_guard lock(m_);
        stopped_ = true;
        q_.clear();
        cv_.notify_all();
    }
    size_t Size() { std::lock_guard lock(m_); return q_.size(); }
private:
    const size_t cap_;
    std::deque<T> q_;
    std::mutex m_;
    std::condition_variable_any cv_;
    bool stopped_ = false;
};
}
