#pragma once

/**
 * @file thread_pool.hpp
 * @author Barak Shoshany (baraksh@gmail.com) (http://baraksh.com)
 * @version 2.0.0
 * @date 2021-08-14
 * @copyright Copyright (c) 2021 Barak Shoshany. Licensed under the MIT license. If you use this library in published research, please cite it as follows:
 *  - Barak Shoshany, "A C++17 Thread Pool for High-Performance Scientific Computing", doi:10.5281/zenodo.4742687, arXiv:2105.00613 (May 2021)
 *
 * @brief A C++17 thread pool for high-performance scientific computing.
 * @details A modern C++17-compatible thread pool implementation, built from scratch with high-performance scientific computing in mind. The thread pool is implemented as a single lightweight and self-contained class, and does not have any dependencies other than the C++17 standard library, thus allowing a great degree of portability. In particular, this implementation does not utilize OpenMP or any other high-level multithreading APIs, and thus gives the programmer precise low-level control over the details of the parallelization, which permits more robust optimizations. The thread pool was extensively tested on both AMD and Intel CPUs with up to 40 cores and 80 threads. Other features include automatic generation of futures and easy parallelization of loops. Two helper classes enable synchronizing printing to an output stream by different threads and measuring execution time for benchmarking purposes. Please visit the GitHub repository at https://github.com/bshoshany/thread-pool for documentation and updates, or to submit feature requests and bug reports.
 */

#define THREAD_POOL_VERSION "v2.0.0 (2021-08-14)"

#include <atomic>      // std::atomic
#include <chrono>      // std::chrono
#include <cstdint>     // std::int_fast64_t, std::uint_fast32_t
#include <functional>  // std::function
#include <future>      // std::future, std::promise
#include <iostream>    // std::cout, std::ostream
#include <memory>      // std::shared_ptr, std::unique_ptr
#include <mutex>       // std::mutex, std::scoped_lock
#include <thread>      // std::this_thread, std::thread
#include <tuple>       // std::tuple, std::apply
#include <type_traits> // std::common_type_t, std::decay_t, std::enable_if_t, std::is_void_v, std::invoke_result_t
#include <utility>     // std::move

#include "conqueue.hpp"
#include "thread_pool_debug.h"

namespace thread_pool {

// ============================================================================================= //
//                                    Begin class thread_pool                                    //

/**
 * @brief A C++17 thread pool class. The user submits tasks to be executed into a queue. Whenever a thread becomes available, it pops a task from the queue and executes it. Each task is automatically assigned a future, which can be used to wait for the task to finish executing and/or obtain its eventual return value.
 */
class thread_pool
{
    using i32 = std::int_fast32_t;
    using ui32 = std::uint_fast32_t;
    using ui64 = std::uint_fast64_t;

    using worker_id_t = i32;

    /**
     * @brief Task to run on worker thread.
     */
    struct Task {
        Task() = default;

        template <typename F,
                typename = typename std::enable_if_t<!std::is_same<Task, typename std::decay<F>::type>::value>>
        Task(F&& _task, int _worker_id): func(std::forward<decltype(_task)>(_task)), worker_id(_worker_id) {}

        Task(const Task&) = default;
        Task(Task&&) = default;
        Task& operator=(const Task&) = default;
        Task& operator=(Task&&) = default;

        /**
         * @brief Task function to run.
         */
        std::function<void()> func;

        /**
         * @brief If worker_id is not -1, the func must run on the corresponding worker thread. It can't be stole by other worker threads.
         */
        worker_id_t worker_id;
    };

public:
    // ============================
    // Constructors and destructors
    // ============================

    /**
     * @brief Construct a new thread pool.
     *
     * @param _thread_count The number of threads to use. Value 0 means the total number of hardware threads available, as reported by the implementation. With a hyperthreaded CPU, this will be twice the number of CPU cores. If the argument is zero, the default value will be used instead.
     */
    thread_pool(const ui32 _thread_count)
        : thread_count((_thread_count != 0) ? _thread_count : std::thread::hardware_concurrency())
    {
        create_threads();
    }

    /**
     * @brief Destruct the thread pool. Waits for all tasks to complete, then destroys all threads. Note that if the variable paused is set to true, then any tasks still in the queue will never be executed.
     */
    ~thread_pool()
    {
        destroy_threads();
    }

    // =======================
    // Public member functions
    // =======================

    /**
     * @brief Get the number of tasks currently waiting in the queue to be executed by the threads.
     *
     * @return The number of queued tasks.
     */
    ui64 get_tasks_queued() const
    {
        ui64 size = 0;
        for (const auto &t : tasks)
            size += t.size();
        return size;
    }

    /**
     * @brief Get the number of tasks currently being executed by the threads.
     *
     * @return The number of running tasks.
     */
    ui32 get_tasks_running() const
    {
        return tasks_total - (ui32)get_tasks_queued();
    }

    /**
     * @brief Get the total number of unfinished tasks - either still in the queue, or running in a thread.
     *
     * @return The total number of tasks.
     */
    ui32 get_tasks_total() const
    {
        return tasks_total;
    }

    /**
     * @brief Get the number of threads in the pool.
     *
     * @return The number of threads.
     */
    ui32 get_thread_count() const
    {
        return thread_count;
    }

    /**
     * @brief Parallelize a loop by splitting it into blocks, submitting each block separately to the thread pool, and waiting for all blocks to finish executing. The user supplies a loop function, which will be called once per block and should iterate over the block's range.
     *
     * @tparam T1 The type of the first index in the loop. Should be a signed or unsigned integer.
     * @tparam T2 The type of the index after the last index in the loop. Should be a signed or unsigned integer. If T1 is not the same as T2, a common type will be automatically inferred.
     * @tparam F The type of the function to loop through.
     * @param first_index The first index in the loop.
     * @param index_after_last The index after the last index in the loop. The loop will iterate from first_index to (index_after_last - 1) inclusive. In other words, it will be equivalent to "for (T i = first_index; i < index_after_last; i++)". Note that if first_index == index_after_last, the function will terminate without doing anything.
     * @param loop The function to loop through. Will be called once per block. Should take exactly two arguments: the first index in the block and the index after the last index in the block. loop(start, end) should typically involve a loop of the form "for (T i = start; i < end; i++)".
     * @param num_blocks The maximum number of blocks to split the loop into. The default is to use the number of threads in the pool.
     */
    template <typename T1, typename T2, typename F>
    void parallelize_loop(const T1 &first_index, const T2 &index_after_last, const F &loop, ui32 num_blocks = 0)
    {
        typedef std::common_type_t<T1, T2> T;
        T the_first_index = (T)first_index;
        T last_index = (T)index_after_last;
        if (the_first_index == last_index)
            return;
        if (last_index < the_first_index)
        {
            T temp = last_index;
            last_index = the_first_index;
            the_first_index = temp;
        }
        last_index--;
        if (num_blocks == 0)
            num_blocks = thread_count;
        ui64 total_size = (ui64)(last_index - the_first_index + 1);
        ui64 block_size = (ui64)(total_size / num_blocks);
        if (block_size == 0)
        {
            block_size = 1;
            num_blocks = (ui32)total_size > 1 ? (ui32)total_size : 1;
        }
        std::atomic<ui32> blocks_running = 0;
        for (ui32 t = 0; t < num_blocks; t++)
        {
            T start = ((T)(t * block_size) + the_first_index);
            T end = (t == num_blocks - 1) ? last_index + 1 : ((T)((t + 1) * block_size) + the_first_index);
            blocks_running++;
            push_task([start, end, &loop, &blocks_running]
                      {
                          loop(start, end);
                          blocks_running--;
                      });
        }
        while (blocks_running != 0)
        {
            sleep_or_yield();
        }
    }

    /**
     * @brief Push a function with no arguments or return value into the task queue.
     *
     * @tparam F The type of the function.
     * @param task The function to push.
     */
    template <typename F>
    void push_task(const F &&task)
    {
        tasks_total++;
        push_count++;
        worker_id_t worker_id = push_count % thread_count;
        DBG_THREAD_POOL("push_task add task:" + std::to_string(push_count) + " to worker:" + std::to_string(worker_id));
        tasks[worker_id].emplace(std::forward<decltype(task)>(task), -1);
    }

    /**
     * @brief Push a function with arguments, but no return value, into the task queue.
     * @details The function is wrapped inside a lambda in order to hide the arguments, as the tasks in the queue must be of type std::function<void()>, so they cannot have any arguments or return value. If no arguments are provided, the other overload will be used, in order to avoid the (slight) overhead of using a lambda.
     *
     * @tparam F The type of the function.
     * @tparam A The types of the arguments.
     * @param task The function to push.
     * @param args The arguments to pass to the function.
     */
    template <typename F, typename... Args>
    void push_task(const F &&task, const Args &&...args)
    {
        // Perfect lambda capture in C++17.
        // From https://stackoverflow.com/a/49902823/
        push_task([task = std::forward<decltype(task)>(task), args = std::make_tuple(std::forward<decltype(args)>(args) ...)]() mutable {
            std::apply([&task](auto &&...args) {
                task(std::forward<decltype(args)>(args)...);
            }, std::move(args));
        });
    }

    /**
     * @brief Same as push_task, the difference is that the task binds to a specific worker thread.
     * @details Tasks with same worker_selector are executed on the same worker thread, in the same order as they are pushed.
     *
     * @tparam F The type of the function.
     * @param worker_selector An integer used to select worker thread: worker_id = worker_selector % thread_count.
     * @param task The function to push.
     */
    template <typename F>
    void push_bind_task(int worker_selector, const F &&task)
    {
        tasks_total++;
        auto worker_id = static_cast<worker_id_t>(worker_selector % thread_count);
        tasks[worker_id].emplace(std::forward<decltype(task)>(task), worker_id);
    }

    /**
     * @brief Same as push_task, the difference is that the task binds to a specific worker thread.
     * @details Tasks with same worker_selector are executed on the same worker thread, in the same order as they are pushed.
     *
     * @tparam F The type of the function.
     * @tparam Args The types of the arguments.
     * @param worker_selector An integer used to select worker thread: worker_id = worker_selector % thread_count.
     * @param task The function to push.
     * @param args The arguments to pass to the function.
     */
    template <typename F, typename... Args>
    void push_bind_task(int worker_selector, const F &&task, const Args &&...args)
    {
        // Perfect lambda capture in C++17.
        // From https://stackoverflow.com/a/49902823/
        push_bind_task(worker_selector, [task = std::forward<decltype(task)>(task), args = std::make_tuple(std::forward<decltype(args)>(args) ...)]() mutable {
            std::apply([&task](auto &&...args) {
                task(std::forward<decltype(args)>(args)...);
            }, std::move(args));
        });
    }

    /**
     * @brief Reset the number of threads in the pool. Waits for all currently running tasks to be completed, then destroys all threads in the pool and creates a new thread pool with the new number of threads. Any tasks that were waiting in the queue before the pool was reset will then be executed by the new threads. If the pool was paused before resetting it, the new pool will be paused as well.
     *
     * @param _thread_count The number of threads to use. Value 0 means the total number of hardware threads available, as reported by the implementation. With a hyperthreaded CPU, this will be twice the number of CPU cores. If the argument is zero, the default value will be used instead.
     */
    void reset(const ui32 &_thread_count)
    {
        destroy_threads();

        thread_count = (_thread_count != 0) ? _thread_count : std::thread::hardware_concurrency();
        create_threads();
    }

    /**
     * @brief Submit a function with zero or more arguments and no return value into the task queue, and get an std::future<bool> that will be set to true upon completion of the task.
     *
     * @tparam F The type of the function.
     * @tparam A The types of the zero or more arguments to pass to the function.
     * @param task The function to submit.
     * @param args The zero or more arguments to pass to the function.
     * @return A future to be used later to check if the function has finished its execution.
     */
    template <typename F, typename... A, typename = std::enable_if_t<std::is_void_v<std::invoke_result_t<std::decay_t<F>, std::decay_t<A>...>>>>
    std::future<bool> submit(const F &task, const A &...args)
    {
        std::shared_ptr<std::promise<bool>> task_promise(new std::promise<bool>);
        std::future<bool> future = task_promise->get_future();
        push_task([task, args..., task_promise]
                  {
                      try
                      {
                          task(args...);
                          task_promise->set_value(true);
                      }
                      catch (...)
                      {
                          try
                          {
                              task_promise->set_exception(std::current_exception());
                          }
                          catch (...)
                          {
                          }
                      }
                  });
        return future;
    }

    /**
     * @brief Submit a function with zero or more arguments and a return value into the task queue, and get a future for its eventual returned value.
     *
     * @tparam F The type of the function.
     * @tparam A The types of the zero or more arguments to pass to the function.
     * @tparam R The return type of the function.
     * @param task The function to submit.
     * @param args The zero or more arguments to pass to the function.
     * @return A future to be used later to obtain the function's returned value, waiting for it to finish its execution if needed.
     */
    template <typename F, typename... A, typename R = std::invoke_result_t<std::decay_t<F>, std::decay_t<A>...>, typename = std::enable_if_t<!std::is_void_v<R>>>
    std::future<R> submit(const F &task, const A &...args)
    {
        std::shared_ptr<std::promise<R>> task_promise(new std::promise<R>);
        std::future<R> future = task_promise->get_future();
        push_task([task, args..., task_promise]
                  {
                      try
                      {
                          task_promise->set_value(task(args...));
                      }
                      catch (...)
                      {
                          try
                          {
                              task_promise->set_exception(std::current_exception());
                          }
                          catch (...)
                          {
                          }
                      }
                  });
        return future;
    }

    /**
     * @brief Wait for tasks to be completed. Normally, this function waits for all tasks, both those that are currently running in the threads and those that are still waiting in the queue. However, if the variable paused is set to true, this function only waits for the currently running tasks (otherwise it would wait forever). To wait for a specific task, use submit() instead, and call the wait() member function of the generated future.
     */
    void wait_for_tasks()
    {
        while (true)
        {
            if (!paused)
            {
                // Because support for un-pause and reset, can't wait for queue become empty infinitely.
                // Besides, task queue becomes empty may still have one last task running.
                // So we always need to wait for tasks_total becomes 0.
                if (tasks_total == 0)
                    break;
            }
            else
            {
                if (get_tasks_running() == 0)
                    break;
            }
            sleep_or_yield();
        }
    }

    // ===========
    // Public data
    // ===========

    /**
     * @brief An atomic variable indicating to the workers to pause. When set to true, the workers temporarily stop popping new tasks out of the queue, although any tasks already executed will keep running until they are done. Set to false again to resume popping tasks.
     */
    std::atomic<bool> paused = false;

    /**
     * @brief The duration, in microseconds, that the worker function should sleep for when it cannot find any tasks in the queue. If set to 0, then instead of sleeping, the worker function will execute std::this_thread::yield() if there are no tasks in the queue. The default value is 1000.
     */
    ui32 sleep_duration = 1000;

    /**
     * @brief The duration, in microseconds, that pop tasks from queue could block for.
     * @detail Larger duration means:
     *
     * 1. Fewer unnecessary worker threads wake ups. This is good.
     * 2. Wait longer to steal work from other threads. This can be an issue.
     *    - Pop task blocks on local queue, so it's possible that all
     *      worker threads are waiting while only one is busy and still has
     *      pending tasks on queue.
     *    - But as tasks are round-robin pushed to task queues, the impact for
     *      this problem should be small.
     * 3. Wait longer for thread to exit after setting running to false. This should be fine for most use cases.
     * 4. Longer time for work thread from un-paused popping task blocked state to paused state.
     *    - But you should not rely on setting pause=true and hope adding task won't be executed anyway.
     *
     * The default value chosen here is according to to Linux default HZ 250.
     */
    ui32 pop_task_timeout = 4'000;

private:
    // ========================
    // Private member functions
    // ========================

    /**
     * @brief Create the threads in the pool and assign a worker to each thread.
     */
    void create_threads()
    {
        running = true;
        tasks_total = 0;
        push_count = 0;
        tasks.resize(thread_count);

        threads.reserve(thread_count);
        for (worker_id_t i = 0; i < static_cast<worker_id_t>(thread_count); i++)
        {
            threads.emplace_back(&thread_pool::worker, this, i);
        }
    }

    /**
     * @brief Destroy the threads in the pool by joining them.
     */
    void destroy_threads()
    {
        running = false;
        for (ui32 i = 0; i < thread_count; i++)
        {
            threads[i].join();
        }
        threads.clear();
        tasks.clear();
    }

    /**
     * @brief Try to pop a new task out of the queue.
     *
     * @param task A reference to the task. Will be populated with a Task if the queue is not empty.
     */
    bool pop_task(Task &task, worker_id_t worker_id)
    {
        if (tasks[worker_id].try_pop(task))
        {
            DBG_THREAD_POOL("pop_task: worker:" + std::to_string(worker_id) + " got task from local queue");
            return true;
        }

        if (steal_task(task, worker_id)) {
            return true;
        }

        // Wait on local task queue.
        DBG_THREAD_POOL("pop_task: worker:" + std::to_string(worker_id) + " waiting for task");
        return tasks[worker_id].wait_and_pop_for(task, pop_task_timeout);
    }

    /**
     * @brief Sleep for sleep_duration microseconds. If that variable is set to zero, yield instead.
     *
     */
    void sleep_or_yield()
    {
        if (sleep_duration)
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_duration));
        else
            std::this_thread::yield();
    }

    /**
     * @brief Steal task from other thread's task queue.
     *
     * @param task A reference to the task. Will be populated with a Task if the queue is not empty.
     * @param worker_id
     * @return true if got new task, false otherwise.
     */
    bool steal_task(Task& task, worker_id_t worker_id) {
        worker_id_t end_id = worker_id + static_cast<worker_id_t>(thread_count) - 1;
        for (worker_id_t i = worker_id + 1; i < end_id; ++i)
        {
            if (tasks[i % thread_count].try_pop_if(task, [](const Task& t) { return t.worker_id == -1; } ))
            {
                DBG_THREAD_POOL("pop_task: worker:" + std::to_string(worker_id) + " steal task from worker:" + std::to_string(i));
                return true;
            }
        }
        return false;
    }

    /**
     * @brief A worker function to be assigned to each thread in the pool. Continuously pops tasks out of the queue and executes them, as long as the atomic variable running is set to true.
     *
     * @param worker_id The ID of the worker.
     */
    void worker(ui32 worker_id)
    {
        while (running)
        {
            Task task;
            if (paused)
            {
                sleep_or_yield();
            }
            else if (pop_task(task, worker_id))
            {
                task.func();
                tasks_total--;
            }
        }
    }

    // ============
    // Private data
    // ============

    /**
     * @brief An atomic variable indicating to the workers to keep running. When set to false, the workers permanently stop working.
     */
    std::atomic<bool> running = true;

    /**
     * @brief The number of threads in the pool.
     */
    ui32 thread_count = 0;

    /**
     * @brief Total number of push_task executed. Used to round-robin distribute tasks to worker task queues.
     */
    ui32 push_count = 0;

    /**
     * @brief Task queues for each worker thread.
     *
     * Tasks in tasks[id] will be executed only by worker thread with worker_id=id;
     */
    std::vector<conqueue<Task>> tasks;

    /**
     * @brief All threads in the pool.
     */
    std::vector<std::thread> threads;

    /**
     * @brief An atomic variable to keep track of the total number of unfinished tasks - either still in the queue, or running in a thread.
     */
    std::atomic<ui32> tasks_total = 0;
};

//                                     End class thread_pool                                     //
// ============================================================================================= //

// ============================================================================================= //
//                                   Begin class synced_stream                                   //

/**
 * @brief A helper class to synchronize printing to an output stream by different threads.
 */
class synced_stream
{
public:
    /**
     * @brief Construct a new synced stream.
     *
     * @param _out_stream The output stream to print to. The default value is std::cout.
     */
    synced_stream(std::ostream &_out_stream = std::cout)
        : out_stream(_out_stream){};

    /**
     * @brief Print any number of items into the output stream. Ensures that no other threads print to this stream simultaneously, as long as they all exclusively use this synced_stream object to print.
     *
     * @tparam T The types of the items
     * @param items The items to print.
     */
    template <typename... T>
    void print(const T &...items)
    {
        const std::scoped_lock lock(stream_mutex);
        (out_stream << ... << items);
    }

    /**
     * @brief Print any number of items into the output stream, followed by a newline character. Ensures that no other threads print to this stream simultaneously, as long as they all exclusively use this synced_stream object to print.
     *
     * @tparam T The types of the items
     * @param items The items to print.
     */
    template <typename... T>
    void println(const T &...items)
    {
        print(items..., '\n');
    }

private:
    /**
     * @brief A mutex to synchronize printing.
     */
    mutable std::mutex stream_mutex = {};

    /**
     * @brief The output stream to print to.
     */
    std::ostream &out_stream;
};

//                                    End class synced_stream                                    //
// ============================================================================================= //

// ============================================================================================= //
//                                       Begin class timer                                       //

/**
 * @brief A helper class to measure execution time for benchmarking purposes.
 */
class timer
{
    typedef std::int_fast64_t i64;

public:
    /**
     * @brief Start (or restart) measuring time.
     */
    void start()
    {
        start_time = std::chrono::steady_clock::now();
    }

    /**
     * @brief Stop measuring time and store the elapsed time since start().
     */
    void stop()
    {
        elapsed_time = std::chrono::steady_clock::now() - start_time;
    }

    /**
     * @brief Get the number of milliseconds that have elapsed between start() and stop().
     *
     * @return The number of milliseconds.
     */
    i64 ms() const
    {
        return (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time)).count();
    }

private:
    /**
     * @brief The time point when measuring started.
     */
    std::chrono::time_point<std::chrono::steady_clock> start_time = std::chrono::steady_clock::now();

    /**
     * @brief The duration that has elapsed between start() and stop().
     */
    std::chrono::duration<double> elapsed_time = std::chrono::duration<double>::zero();
};

//                                        End class timer                                        //
// ============================================================================================= //

}  // namespace thread_pool
