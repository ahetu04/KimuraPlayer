//
// Copyright (c) Alexandre Hetu.
// Licensed under the MIT License.
//
// https://github.com/ahetu04
//

#include <iostream>
#include <thread>
#include <chrono>

#include "IKimuraConverter.h"
#include <vector>
#include <string>


int main(int argc, char* argv[])
{

	std::vector<std::string> theArgs;
	for (int iArg = 0; iArg < argc; iArg++)
	{
		theArgs.push_back(argv[iArg]);
	}

	Kimura::IKimuraConverter* pConverter = Kimura::CreateConverter(theArgs);

	if (!pConverter)
	{
		return -1;
	}

	pConverter->Start();

	while (pConverter->IsWorking())
	{
		using namespace std::chrono_literals;
		std::this_thread::sleep_for(0.2s);
	}

	if (pConverter->HasSucceeded())
	{
		std::printf("\nDone!");
	}
	else
	{
		std::string err = pConverter->GetErrorMessage();
		std::printf(err.c_str(), "");
	}

	delete pConverter;

}
