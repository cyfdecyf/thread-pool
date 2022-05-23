#pragma once

/**
 * @file conqueue.hpp
 */

#include <cstdint>     // std::int_fast64_t, std::uint_fast32_t
#include <condition_variable>  // std::condition_variable
#include <mutex>       // std::mutex, std::scoped_lock
#include <queue>       // std::queue

namespace thread_pool {

// ============================================================================================= //
//                                    Begin class conqueue                                       //

/**
 * @brief A rather simple concurrent queue.
 *
 * @tparam T The type of the elements in the queue.
 */
template <typename T>
class conqueue
{
    using ui32 = std::uint_fast32_t;
    using size_type = typename std::deque<T>::size_type;

public:
    // ============================
    // Constructors and destructors
    // ============================

    /**
     * @brief Construct a new conqueue.
     */
    conqueue() = default;

    ~conqueue() = default;

    // Those special member functions requires locking. No usage in thread_pool.
    conqueue(const conqueue<T>& rhs) = delete;
    conqueue(const conqueue<T>&& rhs) = delete;
    conqueue<T>& operator=(const conqueue<T>& rhs) = delete;

    // =======================
    // Public member functions
    // =======================

    /**
     * @brief Push an element to the back of the queue.
     *
     * @param value Element to be pushed.
     */
    void push(const T &value)
    {
        std::scoped_lock<std::mutex> lock(queue_mutex);
        data_queue.push(value);
        not_empty.notify_one();
    }

    /**
     * @brief Push an element to the back of the queue.
     *
     * @param value Element to be pushed.
     */
    void push(T &&value)
    {
        std::scoped_lock<std::mutex> lock(queue_mutex);
        data_queue.push(std::move(value));
        not_empty.notify_one();
    }

    /**
     * @brief Push an element to the back of the queue. Object is constructed in-place.
     */
    template <typename... Args>
    void emplace(Args&&... args) {
        std::scoped_lock<std::mutex> lock(queue_mutex);
        data_queue.emplace(std::forward<Args>(args)...);
        not_empty.notify_one();
    }

    /**
     * @brief Pop an element from the front of the queue, return immediately if the queue is empty.
     *
     * @param data A reference to data element. Move assigned with the element popped from the queue.
     * @return true if element get popped, false otherwise.
     */
    bool try_pop(T &data)
    {
        std::scoped_lock lock(queue_mutex);
        if (data_queue.empty())
        {
            return false;
        }
        pop_front(data);
        return true;
    }

    /**
     * @brief Pop an element from the front of the queue, but only when predicate on the front element is satisfied. Return immediately if the queue is empty.
     *
     * @param data A reference to data element. Move assigned with the element popped from the queue.
     * @return true if element get popped, false otherwise.
     */
    template <typename Pred>
    bool try_pop_if(T &data, Pred pred)
    {
        std::scoped_lock lock(queue_mutex);
        if (data_queue.empty() || !pred(data_queue.front()))
        {
            return false;
        }
        pop_front(data);
        return true;
    }

    /**
     * @brief Pop an element from the front of the queue, wait forever if the queue is empty.
     *
     * @param data A reference to data element. Move assigned with the element popped from the queue.
     */
    void wait_and_pop(T &data)
    {
        std::unique_lock lock(queue_mutex);
        not_empty.wait(lock, [this]() { return !data_queue.empty(); });
        pop_front(data);
    }

    /**
     * @brief Pop an element from the front of the queue, wait at most timeout_us if there's no data in the queue.
     *
     * @param timeout_us Wait timeout in microseconds.
     * @return true if element get poped, false otherwise.
     */
    bool wait_and_pop_for(T &data, ui32 timeout_us) {
        std::unique_lock lock(queue_mutex);
        not_empty.wait_for(lock, std::chrono::microseconds(timeout_us),
                           [this]() { return !data_queue.empty(); });
        if (data_queue.empty())
        {
            return false;
        }
        pop_front(data);
        return true;
    }

    /**
     * @brief Wait until queue is empty.
     */
    void wait_empty() const
    {
        std::unique_lock lock(queue_mutex);
        is_empty.wait(lock, [this]() { return data_queue.empty(); });
    }

    /**
     * @brief Wait queue become empty for at most timeout_us.
     */
    bool wait_empty_for(ui32 timeout_us) const
    {
        std::unique_lock lock(queue_mutex);
        is_empty.wait_for(lock, std::chrono::microseconds(timeout_us),
                          [this]() { return data_queue.empty(); });
        return data_queue.empty();
    }

    /**
     * @brief Get number of elements in queue.
     * @return
     */
    size_type size() const
    {
        std::scoped_lock lock(queue_mutex);
        return data_queue.size();
    }

    /**
     * @brief Check if queue is is_empty.
     * @return true if queue is is_empty, false otherwise.
     */
    bool empty() const
    {
        return size() == 0;
    }

private:
    // ========================
    // Private member functions
    // ========================

    /**
     * @brief Pop an element from the front of the queue, queue must be non-empty. Notify all threads waiting for queue to be empty.
     *
     * @param data A reference to data element. Move assigned with the element popped from the queue.
     */
    void pop_front(T& data)
    {
        data = std::move(data_queue.front());
        data_queue.pop();
        if (data_queue.empty())
        {
            is_empty.notify_all();
        }
    }

    // ============
    // Private data
    // ============

    /**
     * @brief Queue holding data.
     */
    std::queue<T> data_queue;

    /**
     * @brief A mutex to synchronize access to data_queue by different threads.
     */
    mutable std::mutex queue_mutex;

    /**
     * @brief A condition variable to signal that data_queue is is_empty.
     */
    mutable std::condition_variable is_empty;

    /**
     * @brief A condition variable to signal that data_queue it NOT is_empty.
     */
    mutable std::condition_variable not_empty;
};

//                                        End class timer                                        //
// ============================================================================================= //

}  // namespace thread_pool
