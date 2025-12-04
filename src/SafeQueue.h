#ifndef SAFEQUEUE_H
#define SAFEQUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

template<typename T>
class SafeQueue {
private:
    std::queue<T> q;
    mutable std::mutex mtx;
    std::condition_variable cv;

public:
    SafeQueue() = default;
    ~SafeQueue() = default;

    // 放入元素
    void push(const T& item) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            q.push(item);
        }
        cv.notify_one(); // 通知可能在等待的线程
    }

    // 取出元素，如果队列为空就阻塞等待
    T pop() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !q.empty(); }); // 等待队列非空
        T item = q.front();
        q.pop();
        return item;
    }

    T front() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !q.empty(); });
        return q.front();
    }

    // 尝试取出元素，不阻塞，返回是否成功
    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mtx);
        if (q.empty()) return false;
        item = q.front();
        q.pop();
        return true;
    }

    // 查看队列大小
    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return q.size();
    }

    // 判断是否为空
    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx);
        return q.empty();
    }
};

#endif
