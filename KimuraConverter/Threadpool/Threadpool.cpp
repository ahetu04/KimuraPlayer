//
// Copyright (c) Alexandre Hetu.
// Licensed under the MIT License.
//
// https://github.com/ahetu04
//

#include "Threadpool.h"

namespace Kimura
{
	void ThreadPoolWorker::Run()
	{
		std::unique_lock<std::mutex> threadLock(this->ThreadEventMutex);

		while (!this->StopExecution)
		{
			std::unique_ptr<IThreadPoolTask> t = this->Owner->GetNextTaskFromPool();

			if (t != nullptr)
			{
				t->Execute();
			}
			else
			{
				// make this worker available for work again
				this->Owner->ReturnWorkerToPool(this->Id);

				// and wait
				this->ThreadEvent.wait(threadLock);
			}
		}
	}
}