#pragma once

#include <QMutex>
#include <QQueue>
#include <QWaitCondition>

/**
 * @brief Thread-safe bounded/unbounded queue used between one RawFileReader producer and frame consumers.
 *
 * The queue supports blocking producers when a max size is configured, blocking consumers while the queue is empty,
 * and a close flag that wakes all waiters when a task finishes or fails. It intentionally has no global state so each
 * task can own independent stream queues.
 */
template <typename T>
class ThreadSafeQueue
{
public:
    /**
     * @brief Creates a queue.
     * @param max_size Maximum queued items. A value <= 0 means unbounded.
     */
    explicit ThreadSafeQueue(int max_size = 0)
        : max_size_(max_size)
    {
    }

    /**
     * @brief Pushes an item, blocking if the bounded queue is full.
     * @return false if the queue has been closed before the item was accepted.
     */
    bool push(const T &item)
    {
        QMutexLocker locker(&mutex_);
        while (!closed_ && max_size_ > 0 && queue_.size() >= max_size_) {
            not_full_.wait(&mutex_);
        }

        if (closed_) {
            return false;
        }

        queue_.enqueue(item);
        not_empty_.wakeOne();
        return true;
    }

    /**
     * @brief Attempts to pop without blocking.
     * @return false when the queue is empty.
     */
    bool pop(T *item)
    {
        QMutexLocker locker(&mutex_);
        if (queue_.isEmpty()) {
            return false;
        }

        *item = queue_.dequeue();
        not_full_.wakeOne();
        return true;
    }

    /**
     * @brief Waits for an item until timeout or queue close.
     * @param timeout_ms Timeout in milliseconds. A negative value waits forever.
     * @return true when an item was returned, false on timeout or closed empty queue.
     */
    bool waitAndPop(T *item, int timeout_ms = -1)
    {
        QMutexLocker locker(&mutex_);
        if (timeout_ms < 0) {
            while (queue_.isEmpty() && !closed_) {
                not_empty_.wait(&mutex_);
            }
        } else if (queue_.isEmpty() && !closed_) {
            not_empty_.wait(&mutex_, static_cast<unsigned long>(timeout_ms));
        }

        if (queue_.isEmpty()) {
            return false;
        }

        *item = queue_.dequeue();
        not_full_.wakeOne();
        return true;
    }

    /**
     * @brief Closes the queue and wakes all waiting producer/consumer threads.
     */
    void close()
    {
        QMutexLocker locker(&mutex_);
        closed_ = true;
        not_empty_.wakeAll();
        not_full_.wakeAll();
    }

    /**
     * @brief Returns whether the queue has been closed.
     */
    bool isClosed() const
    {
        QMutexLocker locker(&mutex_);
        return closed_;
    }

    /**
     * @brief Returns the current item count.
     */
    int size() const
    {
        QMutexLocker locker(&mutex_);
        return queue_.size();
    }

private:
    mutable QMutex mutex_;
    QWaitCondition not_empty_;
    QWaitCondition not_full_;
    QQueue<T> queue_;
    int max_size_ = 0;
    bool closed_ = false;
};
