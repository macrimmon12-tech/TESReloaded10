#pragma once
#include <chrono>
#include <map>
#include <deque>
#include <string>
#include <mutex>

typedef std::map<const char*, int> stateMap;

class Logger {
public:
	static void Initialize(const char* FileName);
	static void Log(char* Message, ...);
	static void Log(const char* Message, ...);
	static void Debug(char* Message, ...);
	static void Debug(const char* Message, ...);
	static void TraceRenderState();

	// In-memory mirror of everything written via Log() (not Debug()), for the
	// in-game log window (docs/preset-manager-design.md § "Debug/authoring
	// tooling"). Log() is called from many places in the codebase with no
	// documented threading guarantee, so this is mutex-guarded -- copies out
	// under the lock and returns, rather than handing back a live reference,
	// so the caller never holds the lock while rendering.
	static void GetRecentLines(std::deque<std::string>& OutLines);

//	static char			MessageBuffer[8192];
	static FILE*		LogFile;

private:
	static void PushRingBuffer(const char* Line);

	static constexpr size_t	kRingBufferCap = 500;
	static std::deque<std::string>	s_ringBuffer;
	static std::mutex				s_ringBufferMutex;
};

class TimeLogger {
public:
	std::chrono::system_clock::time_point start;
	std::chrono::system_clock::time_point end;
	TimeLogger();
	~TimeLogger();

	float LogTime(const char* Name);
};
