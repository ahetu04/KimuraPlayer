//
// Copyright (c) Alexandre Hetu.
// Licensed under the MIT License.
//
// https://github.com/ahetu04
//

#include <iostream>
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

#include "Kimura.h"

int main(int argc, char* argv[])
{

    if (argc != 2)
    {
		std::printf("Requires one argument (name of .k file)\n");
        return -1;
    }

	std::string inputFile = argv[1];

    Kimura::PlayerOptions options;
    std::shared_ptr<Kimura::IPlayer> player = Kimura::CreatePlayer(inputFile, options);

    Kimura::uint32 iFrame = 0;

    while (true)
    {
        std::shared_ptr<Kimura::IFrame> pFrame = nullptr;
        if (player->GetStatus() == Kimura::PlayerStatus::Ready)
		{
			pFrame = player->GetFrameAt(iFrame, false);
            if (pFrame)
			{
                const Kimura::uint32* pIndices = pFrame->GetIndicesU32(0);

                const void* pMipmapData = nullptr;
                Kimura::uint32 mipmapDataSize = 0;
                pFrame->GetImageData(0, 0, &pMipmapData, mipmapDataSize);

				const void* pMipmapData1 = nullptr;
				Kimura::uint32 mipmapDataSize1 = 0;
				pFrame->GetImageData(0, 1, &pMipmapData1, mipmapDataSize1);

				const void* pMipmapData2 = nullptr;
				Kimura::uint32 mipmapDataSize2 = 0;
				pFrame->GetImageData(0, 2, &pMipmapData2, mipmapDataSize2);

                const Kimura::int8* t = pFrame->GetTangentsI8(0);


				iFrame = ++iFrame % player->GetNumFrames();
			}

            Kimura::PlayerStats stats;
            player->CollectStats(stats);

            std::printf("stats: bytes read: %d, mem used: %d, avg read time=%f, avg process time=%f\n", (int)stats.BytesReadInLastSecond, (int)stats.MemoryUsageForFrames, (float)stats.AvgTimeSpentOnReadingFromDiskPerFrame, (float)stats.AvgTimeSpentOnProcessingPerFrames);

//             if (iFrame > 30)
//             {
//                 break;
//             }
		}

		std::this_thread::sleep_for(33ms);

    }

    player = nullptr;

    std::cin.get();

}

