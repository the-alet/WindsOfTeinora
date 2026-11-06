#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Teinora
{
	/** Fixed set of worker threads. Pending tasks are dropped on shutdown, running ones finish. */
	class ThreadPool
	{
	public:
		explicit ThreadPool(unsigned Count)
		{
			for (unsigned Index = 0; Index < Count; ++Index)
			{
				Workers.emplace_back([this] { Run(); });
			}
		}

		~ThreadPool()
		{
			{
				std::lock_guard Lock(Mutex);
				bStopping = true;
			}
			Wake.notify_all();
			for (std::thread& Worker : Workers)
			{
				Worker.join();
			}
		}

		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;

		void Enqueue(std::function<void()> Task)
		{
			{
				std::lock_guard Lock(Mutex);
				Tasks.push_back(std::move(Task));
			}
			Wake.notify_one();
		}

	private:
		void Run()
		{
			for (;;)
			{
				std::function<void()> Task;
				{
					std::unique_lock Lock(Mutex);
					Wake.wait(Lock, [this] { return bStopping || !Tasks.empty(); });
					if (bStopping)
					{
						return;
					}
					Task = std::move(Tasks.front());
					Tasks.pop_front();
				}
				Task();
			}
		}

		std::vector<std::thread> Workers;
		std::deque<std::function<void()>> Tasks;
		std::mutex Mutex;
		std::condition_variable Wake;
		bool bStopping = false;
	};
}
