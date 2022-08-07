//
// Copyright (c) Alexandre Hetu.
// Licensed under the MIT License.
//
// https://github.com/ahetu04
//

#pragma once

#include <thread>
#include <vector>
#include <mutex>
#include <queue>
#include <functional>
#include <condition_variable>

namespace Kimura
{

	class IThreadPoolTask
	{
	public:

		virtual ~IThreadPoolTask(){}
		virtual void Execute() = 0;

	};

	class ThreadPoolWorker
	{

		friend class Threadpool;

	protected:


		ThreadPoolWorker(class Threadpool* InOwner, int InWorkerId)
			:
			Id(InWorkerId),
			Owner(InOwner)
		{
			this->Thread = new std::thread([this] {this->Run(); });
		}

		void Run();

		inline void GetBackToWorkYouLazyBum()
		{
			this->ThreadEvent.notify_one();
		}

		void Stop(bool InWaitToComplete)
		{
			this->StopExecution = true;
			this->ThreadEvent.notify_one();
			if (InWaitToComplete)
			{
				this->Thread->join();
			}

			delete this->Thread;

		}

		int									Id = -1;
		std::thread*						Thread = nullptr;

		class Threadpool*					Owner;

		bool								StopExecution = false;

		std::mutex							ThreadEventMutex;
		std::condition_variable				ThreadEvent;

	};


	class Threadpool
	{

		friend class ThreadPoolWorker;

		public:

			Threadpool(std::string name, int numThreads)
				:
				Name(name),
				NumThreads(numThreads)
			{
				for (int i = 0; i < numThreads; i++)
				{
					std::shared_ptr<ThreadPoolWorker> w = std::shared_ptr<ThreadPoolWorker>(new ThreadPoolWorker(this, i));
					this->AllWorkers.push_back(w);
					//this->AvailableWorkerIDs.push(i);
				}
			}

			~Threadpool()
			{
				this->Stop();
			}

			void Stop()
			{
				for (int i = 0; i < this->AllWorkers.size(); i++)
				{
					this->AllWorkers[i]->Stop(true);
				}
				this->AllWorkers.clear();

			}

			void AddTask(std::unique_ptr<IThreadPoolTask> t)
			{
				std::lock_guard<std::mutex> scopedGuard(this->TasksMutex);

				this->Tasks.push(std::move(t));

				// is there a worker ready to work?
				if (!this->AvailableWorkerIDs.empty())
				{
					int workerId = this->AvailableWorkerIDs.front();
					this->AvailableWorkerIDs.pop();

					this->AllWorkers[workerId]->GetBackToWorkYouLazyBum();

				}

			}

			inline bool HasAnyWorkLeft()
			{
				std::lock_guard<std::mutex> scopedGuard(this->TasksMutex);
				return !this->Tasks.empty();
			}

			inline std::unique_ptr<IThreadPoolTask> GetNextTaskFromPool()
			{
				std::unique_ptr<IThreadPoolTask> task;
				{
					std::lock_guard<std::mutex> scopedGuard(this->TasksMutex);

					if (this->Tasks.empty())
					{
						return nullptr;
					}

					task = std::move(this->Tasks.front());
					this->Tasks.pop();

					return std::move(task);
				}
			}

			inline void ReturnWorkerToPool(int InWorkerId)
			{
				std::lock_guard<std::mutex> scopedGuard(this->TasksMutex);
				this->AvailableWorkerIDs.push(InWorkerId);
			}

		protected:

			std::string	Name;
			int NumThreads = 1;

			std::vector<std::shared_ptr<ThreadPoolWorker>>	AllWorkers;
			std::queue<int>									AvailableWorkerIDs;

			std::mutex										TasksMutex;
			std::queue<std::unique_ptr<IThreadPoolTask>>	Tasks;

	};

	class ThreadPoolTask_Function : public IThreadPoolTask
	{
		ThreadPoolTask_Function(std::function<void(int)> func, int InNum)
		{
			this->fn = func;
			this->num = InNum;
		}

		virtual void Execute() override 
		{
			this->fn(this->num);
		}

		std::function<void(int)>	fn;
		int num;
	};

}