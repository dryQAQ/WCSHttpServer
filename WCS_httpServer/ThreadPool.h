#pragma once
// ============================================================================
// ThreadPool.h — 高性能线程池（源自 WCSApp 项目）
//
// 特性:
//   - 支持变参函数/lambda/成员函数(bind)提交
//   - 通过 future 获取返回值
//   - 线程常驻，无创建销毁开销
//   - 空闲线程数统计
//
// 用法:
//   Hanchine::ThreadPool pool(20);                    // 20线程
//   auto fut = pool.commit([](int x) { return x*2; }, 5);
//   int result = fut.get();                           // 10
// ============================================================================

#include <vector>
#include <queue>
#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>

//线程池最大容量
#define THREADPOOL_MAX_NUM 100

namespace Hanchine
{
    class ThreadPool
    {
        using Task = std::function<void()>;
        std::vector<std::thread> _pool;
        std::queue<Task>         _tasks;
        std::mutex               _lock;
        std::condition_variable  _task_cv;
        std::atomic<bool>        _run{ true };
        std::atomic<int>         _idlThrNum{ 0 };

    public:
        inline ThreadPool(unsigned short size = 4) { addThread(size); }
        inline ~ThreadPool()
        {
            _run = false;
            _task_cv.notify_all();
            for (std::thread& thread : _pool) {
                if (thread.joinable())
                    thread.join();
            }
        }

        // 提交任务，返回 future 获取结果
        template<class F, class... Args>
        auto commit(F&& f, Args&&... args) -> std::future<decltype(f(args...))>
        {
            if (!_run)
                throw std::runtime_error("commit on ThreadPool is stopped.");

            using RetType = decltype(f(args...));
            auto task = std::make_shared<std::packaged_task<RetType()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...)
            );

            std::future<RetType> future = task->get_future();
            {
                std::lock_guard<std::mutex> lock{ _lock };
                _tasks.emplace([task] { (*task)(); });
            }
#ifdef THREADPOOL_AUTO_GROW
            if (_idlThrNum < 1 && _pool.size() < THREADPOOL_MAX_NUM)
                addThread(1);
#endif
            _task_cv.notify_one();
            return future;
        }

        // 无返回值任务提交（不阻塞）
        template<class F, class... Args>
        void commitNoWait(F&& f, Args&&... args)
        {
            if (!_run) return;

            auto task = std::make_shared<std::packaged_task<void()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...)
            );

            {
                std::lock_guard<std::mutex> lock{ _lock };
                _tasks.emplace([task] { (*task)(); });
            }
#ifdef THREADPOOL_AUTO_GROW
            if (_idlThrNum < 1 && _pool.size() < THREADPOOL_MAX_NUM)
                addThread(1);
#endif
            _task_cv.notify_one();
        }

        int idlCount()  const { return _idlThrNum.load(); }
        int thrCount()  const { return static_cast<int>(_pool.size()); }
        int taskCount() const
        {
            std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(_lock));
            return static_cast<int>(_tasks.size());
        }

#ifndef THREADPOOL_AUTO_GROW
    private:
#endif
        void addThread(unsigned short size)
        {
            for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size)
            {
                _pool.emplace_back([this] {
                    while (_run)
                    {
                        Task task;
                        {
                            std::unique_lock<std::mutex> lock{ _lock };
                            _task_cv.wait(lock, [this] {
                                return !_run || !_tasks.empty();
                            });
                            if (!_run && _tasks.empty())
                                return;
                            task = std::move(_tasks.front());
                            _tasks.pop();
                        }
                        _idlThrNum--;
                        task();
                        _idlThrNum++;
                    }
                });
                _idlThrNum++;
            }
        }
    };

    typedef std::shared_ptr<ThreadPool> g_ThreadPoolPtr;
};