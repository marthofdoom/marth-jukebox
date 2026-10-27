#pragma once

// Recorder behind StubApp.cpp (test target only).
#include "app/AppApi.h"

#include <string>
#include <vector>

namespace MJ::App::Stub
{
	struct SearchCall
	{
		std::string kind;
		std::string text;
		bool        streaming;
	};

	struct Recorder
	{
		int                      testConnection = 0;
		int                      applied        = 0;
		int                      clearCache     = 0;
		std::vector<SearchCall>  searches;
		std::vector<std::string> resets;
		std::vector<std::string> peeks;
		Connection               connection;
		SearchState              search;
		std::vector<EnvStatus>   status;
		std::uint64_t            cacheBytes = 0;
		std::vector<std::string> peek;
	};

	Recorder& Rec();
	void      Reset();
}
