#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <vector>

// Exercise the real collector against deterministic asynchronous query results.
namespace ProfilerTest {
enum { D3DQUERYTYPE_TIMESTAMP, D3DQUERYTYPE_TIMESTAMPFREQ, D3DQUERYTYPE_TIMESTAMPDISJOINT };
enum { D3DISSUE_BEGIN = 2, D3DISSUE_END = 1 };
static HRESULT TimestampStatus = S_OK;
static HRESULT DisjointStatus = S_OK;
static HRESULT FrequencyStatus = S_OK;
static BOOL DisjointValue = FALSE;
static UINT64 FrequencyValue = 1000000;
static bool CannotCreate[3] = {};           // per query type: CreateQuery fails, as on a layer without it
static int LiveQueries = 0;
static std::vector<UINT64> IssueTicks;		// when set, each timestamp Issue records the next value (timeline tests)
static size_t IssueTickIndex = 0;
struct IDirect3DQuery9 {
	int Type;
	UINT64 Value;
	HRESULT Issue(DWORD) {
		if (Type == D3DQUERYTYPE_TIMESTAMP && IssueTickIndex < IssueTicks.size()) Value = IssueTicks[IssueTickIndex++];
		return S_OK;
	}
	HRESULT GetData(void* data, DWORD, DWORD flags) {
		assert(flags == 0);
		HRESULT status = Type == D3DQUERYTYPE_TIMESTAMP ? TimestampStatus :
			Type == D3DQUERYTYPE_TIMESTAMPDISJOINT ? DisjointStatus : FrequencyStatus;
		if (status != S_OK) return status;
		if (Type == D3DQUERYTYPE_TIMESTAMPDISJOINT) *static_cast<BOOL*>(data) = DisjointValue;
		else if (Type == D3DQUERYTYPE_TIMESTAMPFREQ) *static_cast<UINT64*>(data) = FrequencyValue;
		else *static_cast<UINT64*>(data) = Value;
		return S_OK;
	}
	void Release() { --LiveQueries; delete this; }
};
struct IDirect3DDevice9 {
	UINT64 Tick = 100;
	HRESULT CreateQuery(int type, IDirect3DQuery9** query) {
		*query = nullptr;
		if (CannotCreate[type]) return E_FAIL; // D3DERR_NOTAVAILABLE on a real device
		*query = new IDirect3DQuery9{type, Tick++};
		++LiveQueries;
		return S_OK;
	}
};
#define NVR_GPU_PROFILER_TEST
#include "../src/core/GpuProfiler.h"
#include "../src/core/GpuTimeline.h"
}

static void ResetMock() {
	using namespace ProfilerTest;
	TimestampStatus = DisjointStatus = FrequencyStatus = S_OK;
	DisjointValue = FALSE;
	FrequencyValue = 1000000;
	CannotCreate[0] = CannotCreate[1] = CannotCreate[2] = false;
}

void CheckGpuTimeline();

void CheckGpuProfiler() {
	using namespace ProfilerTest;
	IDirect3DDevice9 device;
	ResetMock();
	{
		GpuTimer timer("regression");
		assert(timer.Begin(&device)); timer.End();
		TimestampStatus = S_FALSE;
		assert(timer.Begin(&device)); timer.End();
		assert(timer.GetSampleCount() == 0);
		TimestampStatus = S_OK;
		assert(timer.Begin(&device)); timer.End();
		assert(timer.GetSampleCount() == 2);
		// A failing DISJOINT query no longer kills the timer (a D3D9 layer may not implement it):
		// timing continues without it. It used to switch the whole timer off.
		DisjointStatus = E_FAIL;
		assert(timer.Begin(&device)); timer.End();
		assert(timer.Begin(&device)); timer.End();
		assert(!timer.DisjointTrusted());
		assert(timer.GetSampleCount() >= 3);
	}
	assert(LiveQueries == 0);

	ResetMock();
	{
		GpuTimer timer("ring");
		TimestampStatus = S_FALSE;
		for (int i = 0; i < 12; ++i) { assert(timer.Begin(&device)); timer.End(); }
		assert(!timer.Begin(&device));
		TimestampStatus = S_OK;
		assert(timer.Begin(&device)); timer.End();
		assert(timer.GetSampleCount() == 12);
		TimestampStatus = E_FAIL; // a failing TIMESTAMP query is fatal for the timer, and frees its queries
		assert(!timer.Begin(&device));
		assert(LiveQueries == 0);
		assert(!timer.Begin(&device));
	}

	// A layer with no TIMESTAMPFREQ / TIMESTAMPDISJOINT queries at all still gets timings.
	ResetMock();
	CannotCreate[D3DQUERYTYPE_TIMESTAMPFREQ] = CannotCreate[D3DQUERYTYPE_TIMESTAMPDISJOINT] = true;
	{
		GpuTimer timer("no optional queries");
		for (int i = 0; i < 5; ++i) { assert(timer.Begin(&device)); timer.End(); }
		assert(timer.Begin(&device)); timer.End();
		assert(timer.GetSampleCount() >= 5);
		assert(timer.UsingFallbackClock() && !timer.DisjointTrusted());
	}
	assert(LiveQueries == 0);

	// The two timestamp queries are mandatory: without them the timer is unavailable and holds nothing.
	ResetMock();
	CannotCreate[D3DQUERYTYPE_TIMESTAMP] = true;
	{
		GpuTimer timer("no timestamps");
		assert(!timer.Begin(&device));
		assert(!timer.Begin(&device));
	}
	assert(LiveQueries == 0);

	// A frequency query that starts failing switches to the 1 GHz fallback instead of dropping samples.
	ResetMock();
	{
		GpuTimer timer("frequency fails");
		assert(timer.Begin(&device)); timer.End();
		FrequencyStatus = E_FAIL;
		for (int i = 0; i < 4; ++i) { assert(timer.Begin(&device)); timer.End(); }
		assert(timer.UsingFallbackClock());
		assert(timer.GetSampleCount() >= 3);
	}
	assert(LiveQueries == 0);

	// A disjoint result of TRUE, or a zero frequency, discards the sample and counts it as rejected.
	ResetMock();
	DisjointValue = TRUE;
	{
		GpuTimer timer("disjoint true");
		for (int i = 0; i < 6; ++i) { assert(timer.Begin(&device)); timer.End(); }
		assert(timer.Begin(&device)); timer.End();
		assert(timer.GetSampleCount() == 0 && timer.GetRejectedCount() > 0);
	}
	ResetMock();
	FrequencyValue = 0;
	{
		GpuTimer timer("zero frequency");
		for (int i = 0; i < 6; ++i) { assert(timer.Begin(&device)); timer.End(); }
		assert(timer.Begin(&device)); timer.End();
		assert(timer.GetSampleCount() == 0 && timer.GetRejectedCount() > 0);
	}
	assert(LiveQueries == 0);
	ResetMock();
	std::puts("PASS: actual GPU collector: pending results, ring saturation/recovery, fatal timestamp failure, optional disjoint/frequency queries (missing, failing, rejected).");
	CheckGpuTimeline();
}

// World-scene split (src/core/GpuTimeline.h): one timestamp per key change, intervals charged to the
// key that was current.
namespace TimelineCapture {
static unsigned Reports = 0, Frames = 0;
static double Avg[ProfilerTest::GpuTimeline::KeyCount], Max[ProfilerTest::GpuTimeline::KeyCount];
static unsigned Dropped = 0, Rejected = 0, MostMarks = 0;
static void Capture(const ProfilerTest::GpuTimeline& timeline) {
	Reports++;
	Frames = timeline.WindowFrames();
	for (unsigned k = 0; k < ProfilerTest::GpuTimeline::KeyCount; ++k) { Avg[k] = timeline.AverageMs(k); Max[k] = timeline.MaxMs(k); }
	Dropped = timeline.DroppedFrames();
	Rejected = timeline.RejectedFrames();
	MostMarks = timeline.MostMarks();
}
}

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

void CheckGpuTimeline() {
	using namespace ProfilerTest;
	using namespace TimelineCapture;
	const unsigned char none = GpuTimeline::NoKey;

	// The pure charging rule.
	{
		const UINT64 ticks[] = { 100, 150, 400, 1000 };
		const unsigned char keys[] = { 0, 1, 0, none };
		double sums[3] = {};
		assert(ChargeTimelineIntervals(ticks, keys, 4, 0.001, sums, 3, none));
		assert(Near(sums[0], 0.650) && Near(sums[1], 0.250) && sums[2] == 0.0);
		const UINT64 backwards[] = { 100, 90, 400 };
		double untouched[3] = {};
		assert(!ChargeTimelineIntervals(backwards, keys, 3, 0.001, untouched, 3, none));
		assert(untouched[0] == 0.0 && untouched[1] == 0.0);
		double single[3] = {};
		assert(ChargeTimelineIntervals(ticks, keys, 1, 0.001, single, 3, none) && single[0] == 0.0);
	}

	IDirect3DDevice9 device;
	ResetMock();
	Reports = 0;
	{
		// Two frames, window of two, 1 MHz clock (1 tick = 0.001 ms).
		GpuTimeline timeline("test", 2);
		timeline.OnReport = &Capture;
		timeline.Mark(7); // outside a frame: ignored
		IssueTicks = { 1000, 3000, 4000, 10000, 12000 };
		IssueTickIndex = 0;
		assert(timeline.BeginFrame(&device));
		timeline.Mark(3); timeline.Mark(5); timeline.Mark(none);
		timeline.EndFrame();
		assert(timeline.BeginFrame(&device));
		timeline.Mark(3); timeline.Mark(none);
		timeline.EndFrame();
		assert(timeline.BeginFrame(&device)); // collects both frames and reports
		timeline.EndFrame();
		assert(Reports == 1 && Frames == 2);
		assert(Near(Avg[3], 2.0) && Near(Avg[5], 0.5) && Near(Max[3], 2.0) && Near(Max[5], 1.0) && Avg[7] == 0.0);
		assert(MostMarks == 3 && Dropped == 0 && Rejected == 0);
		IssueTicks.clear();
	}
	assert(LiveQueries == 0);

	// Results not back yet: frames stay pending, the timeline skips frames once all slots are in flight,
	// then collects everything when the GPU catches up.
	ResetMock();
	Reports = 0;
	{
		GpuTimeline timeline("pending", 4);
		timeline.OnReport = &Capture;
		TimestampStatus = S_FALSE;
		for (unsigned i = 0; i < GpuTimeline::FramesInFlight; ++i) {
			assert(timeline.BeginFrame(&device));
			timeline.Mark(1); timeline.Mark(none);
			timeline.EndFrame();
		}
		assert(timeline.PendingFrames() == GpuTimeline::FramesInFlight);
		assert(!timeline.BeginFrame(&device));
		timeline.Mark(1); // not in a frame
		timeline.EndFrame();
		TimestampStatus = S_OK;
		assert(timeline.BeginFrame(&device));
		timeline.EndFrame();
		assert(timeline.PendingFrames() == 0 && Reports == 1);
	}
	assert(LiveQueries == 0);

	// A frame with more than MaxMarks changes is dropped, and ticks that run backwards are rejected.
	ResetMock();
	Reports = 0;
	{
		GpuTimeline timeline("overflow", 1);
		timeline.OnReport = &Capture;
		assert(timeline.BeginFrame(&device));
		for (unsigned i = 0; i <= GpuTimeline::MaxMarks; ++i) timeline.Mark((unsigned char)(i & 1));
		timeline.EndFrame();
		assert(timeline.PendingFrames() == 0 && timeline.DroppedFrames() == 1);

		IssueTicks = { 500, 400, 900 };
		IssueTickIndex = 0;
		assert(timeline.BeginFrame(&device));
		timeline.Mark(1); timeline.Mark(2); timeline.Mark(none);
		timeline.EndFrame();
		assert(timeline.BeginFrame(&device));
		timeline.EndFrame();
		assert(Reports == 0 && timeline.RejectedFrames() == 1);
		IssueTicks.clear();
	}
	assert(LiveQueries == 0);

	// A disjoint clock rejects the frame; a failing timestamp GetData switches the timeline off.
	ResetMock();
	Reports = 0;
	DisjointValue = TRUE;
	{
		GpuTimeline timeline("disjoint", 1);
		timeline.OnReport = &Capture;
		assert(timeline.BeginFrame(&device));
		timeline.Mark(1); timeline.Mark(none);
		timeline.EndFrame();
		assert(timeline.BeginFrame(&device));
		timeline.EndFrame();
		assert(Reports == 0 && timeline.RejectedFrames() == 1);
		DisjointValue = FALSE;
		assert(timeline.BeginFrame(&device));
		timeline.Mark(1); timeline.Mark(none);
		timeline.EndFrame();
		TimestampStatus = E_FAIL;
		assert(!timeline.BeginFrame(&device));
		assert(timeline.IsUnavailable() && LiveQueries == 0);
	}
	assert(LiveQueries == 0);
	ResetMock();
	std::puts("PASS: GPU timeline split: charging rule, two-frame window, pending frames and slot saturation, overflow drop, backwards/disjoint rejection, fatal GetData.");
}
