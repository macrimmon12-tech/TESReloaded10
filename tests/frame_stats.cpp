// Checks the frame-time statistics used by the F10 profiler (src/core/FrameTimeStats.h).
#include <cstdio>
#include <cmath>
#include <vector>
#include "../src/core/FrameTimeStats.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

int main()
{
	// 1000 frames of exactly 10 ms: every statistic is 10 ms / 100 fps.
	{
		std::vector<double> ms(1000, 10.0);
		FrameTimeSummary s = SummarizeFrameTimes(ms);
		CHECK(s.frames == 1000 && Near(s.avgMs, 10) && Near(s.p50Ms, 10) && Near(s.p99Ms, 10) && Near(s.maxMs, 10), "a steady 10 ms stream has 10 ms everywhere");
		CHECK(Near(s.low1Fps, 100) && Near(s.low01Fps, 100), "a steady stream has 100 fps 1%% and 0.1%% lows");
		CHECK(CountFrameSpikes(ms, s.p50Ms) == 0, "a steady stream has no spikes");
	}

	// 990 frames of 10 ms plus 10 frames of 60 ms (exactly 1%): 1% low is the mean of those 10 = 60 ms.
	{
		std::vector<double> ms(990, 10.0);
		for (int i = 0; i < 10; i++) ms.push_back(60.0);
		FrameTimeSummary s = SummarizeFrameTimes(ms);
		CHECK(Near(s.p50Ms, 10) && Near(s.p95Ms, 10), "p50 and p95 ignore a 1%% tail");
		CHECK(Near(s.p99Ms, 10), "p99 is the 990th of 1000 sorted frames (still 10 ms with exactly 1%% slow)");
		CHECK(Near(s.p999Ms, 60) && Near(s.maxMs, 60), "p99.9 and max see the slow frames");
		CHECK(Near(s.low1Fps, 1000.0 / 60.0, 1e-6), "1%% low fps is 1000/60 for a 1%% tail of 60 ms frames (got %.4f)", s.low1Fps);
		CHECK(Near(s.low01Fps, 1000.0 / 60.0, 1e-6), "0.1%% low fps is also 16.7 fps (worst single frame)");
		CHECK(Near(s.avgMs, (990 * 10.0 + 10 * 60.0) / 1000.0, 1e-9), "average includes the slow frames");
		CHECK(CountFrameSpikes(ms, s.p50Ms) == 10, "the ten 60 ms frames are spikes (threshold %.1f ms)", FrameSpikeThresholdMs(10));
	}

	// One slow frame in 1000: 0.1% low is that frame; the 1% low averages it with nine 10 ms frames.
	{
		std::vector<double> ms(999, 10.0);
		ms.push_back(110.0);
		FrameTimeSummary s = SummarizeFrameTimes(ms);
		CHECK(Near(s.low01Fps, 1000.0 / 110.0, 1e-6), "0.1%% low is the single worst frame (%.3f fps)", s.low01Fps);
		CHECK(Near(s.low1Fps, 1000.0 / ((9 * 10.0 + 110.0) / 10.0), 1e-6), "1%% low averages the worst 10 frames (%.3f fps)", s.low1Fps);
	}

	// Order does not matter; a shuffled ramp gives the same summary as the sorted one.
	{
		std::vector<double> a, b;
		for (int i = 1; i <= 200; i++) a.push_back(i * 0.1);
		for (int i = 200; i >= 1; i--) b.push_back(i * 0.1);
		FrameTimeSummary sa = SummarizeFrameTimes(a), sb = SummarizeFrameTimes(b);
		CHECK(Near(sa.p50Ms, sb.p50Ms) && Near(sa.p99Ms, sb.p99Ms) && Near(sa.low1Fps, sb.low1Fps), "the summary does not depend on frame order");
		CHECK(Near(sa.p50Ms, 10.0), "p50 of a 0.1..20.0 ms ramp is 10.0 ms (nearest rank)");
	}

	// Edge cases.
	{
		std::vector<double> empty;
		FrameTimeSummary s = SummarizeFrameTimes(empty);
		CHECK(s.frames == 0 && s.low1Fps == 0, "an empty window summarises to zeros");
		std::vector<double> one(1, 8.0);
		s = SummarizeFrameTimes(one);
		CHECK(s.frames == 1 && Near(s.p99Ms, 8) && Near(s.low1Fps, 125), "a single frame is its own percentile and low");
	}

	// The spike threshold behaves sensibly at both ends of the frame-rate range.
	{
		CHECK(Near(FrameSpikeThresholdMs(6.0), 15.0), "at 6 ms a spike needs 15 ms (2.5x)");
		CHECK(Near(FrameSpikeThresholdMs(30.0), 75.0), "at 30 ms a spike needs 75 ms");
		CHECK(Near(FrameSpikeThresholdMs(2.0), 10.0), "at 2 ms the +8 ms floor applies");
	}

	std::printf(failures ? "\n%d check(s) FAILED\n" : "\nAll frame statistics checks passed\n", failures);
	return failures ? 1 : 0;
}
