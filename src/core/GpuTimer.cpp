#include "GpuTimer.h"

namespace {
	// Frames in flight before a slot is reused and read. Timestamps are usually resolved within
	// two frames; four leaves room without ever waiting.
	constexpr int kFrames = 4;
	constexpr int kMaxQueries = 128;	// per frame: two per span
	constexpr float kSmoothing = 0.05f;	// weight of each new frame in the average

	struct Span {
		const char*			Label;
		IDirect3DQuery9*	Begin;
		IDirect3DQuery9*	End;
		int					Depth;
	};

	struct Frame {
		IDirect3DQuery9*				Disjoint = nullptr;
		IDirect3DQuery9*				Freq = nullptr;
		IDirect3DQuery9*				Start = nullptr;
		IDirect3DQuery9*				Finish = nullptr;
		std::vector<IDirect3DQuery9*>	Pool;
		int								PoolUsed = 0;
		std::vector<Span>				Spans;
		bool							Pending = false;	// issued and not yet read
	};

	Frame						s_frames[kFrames];
	int							s_current = -1;
	bool						s_enabled = false;
	bool						s_unavailable = false;
	std::vector<GpuTimer::Stat>	s_stats;
	float						s_frameMs = -1.0f;

	IDirect3DQuery9* Create(D3DQUERYTYPE type) {
		IDirect3DQuery9* query = nullptr;
		if (FAILED(TheRenderManager->device->CreateQuery(type, &query))) {
			if (!s_unavailable) Logger::Log("[WARNING] GPU timings: the device cannot create timestamp queries (type %i), so the panel stays empty.", (int)type);
			s_unavailable = true;
			return nullptr;
		}
		return query;
	}

	IDirect3DQuery9* NextTimestamp(Frame& frame) {
		if (frame.PoolUsed >= kMaxQueries) return nullptr;
		if (frame.PoolUsed == (int)frame.Pool.size()) {
			IDirect3DQuery9* query = Create(D3DQUERYTYPE_TIMESTAMP);
			if (!query) return nullptr;
			frame.Pool.push_back(query);
		}
		return frame.Pool[frame.PoolUsed++];
	}

	bool Read(IDirect3DQuery9* query, UINT64& value) {
		return query && query->GetData(&value, sizeof(UINT64), 0) == S_OK;
	}

	void Accumulate(const char* label, float ms, int depth) {
		for (GpuTimer::Stat& stat : s_stats) {
			if (stat.Label == label) {
				stat.LastMs = ms;
				stat.AverageMs = (stat.AverageMs < 0.0f) ? ms : stat.AverageMs + (ms - stat.AverageMs) * kSmoothing;
				return;
			}
		}
		s_stats.push_back({ label, ms, ms, depth });
	}

	// Reads a finished frame. Never waits: if anything is not ready, the frame is dropped.
	void Collect(Frame& frame) {
		frame.Pending = false;

		BOOL disjoint = TRUE;
		UINT64 frequency = 0, start = 0, finish = 0;
		if (!frame.Disjoint || frame.Disjoint->GetData(&disjoint, sizeof(BOOL), 0) != S_OK || disjoint) return;
		if (!frame.Freq || frame.Freq->GetData(&frequency, sizeof(UINT64), 0) != S_OK || frequency == 0) return;
		if (!Read(frame.Start, start) || !Read(frame.Finish, finish)) return;

		// Sum repeats of a label first, so a stage rendered twice in a frame counts once, in full.
		struct Total { const char* Label; double Ms; int Depth; };
		std::vector<Total> totals;
		for (const Span& span : frame.Spans) {
			UINT64 begin = 0, end = 0;
			if (!span.End || !Read(span.Begin, begin) || !Read(span.End, end) || end < begin) continue;
			double ms = (double)(end - begin) * 1000.0 / (double)frequency;
			auto it = std::find_if(totals.begin(), totals.end(), [&](const Total& t) { return strcmp(t.Label, span.Label) == 0; });
			if (it == totals.end()) totals.push_back({ span.Label, ms, span.Depth });
			else it->Ms += ms;
		}
		for (const Total& t : totals) Accumulate(t.Label, (float)t.Ms, t.Depth);

		float frameMs = (float)((double)(finish - start) * 1000.0 / (double)frequency);
		s_frameMs = (s_frameMs < 0.0f) ? frameMs : s_frameMs + (frameMs - s_frameMs) * kSmoothing;
	}
}


void GpuTimer::SetEnabled(bool Enabled) {
	if (Enabled == s_enabled) return;
	s_enabled = Enabled;
	if (!Enabled) {
		// Abandon whatever is in flight; the queries themselves are kept for the next session.
		for (Frame& frame : s_frames) { frame.Pending = false; frame.Spans.clear(); frame.PoolUsed = 0; }
		s_current = -1;
	}
}

bool GpuTimer::IsEnabled() { return s_enabled; }
bool GpuTimer::IsAvailable() { return !s_unavailable; }


void GpuTimer::FrameBoundary() {
	if (!s_enabled || s_unavailable) return;

	// Close the frame that is running.
	if (s_current >= 0) {
		Frame& frame = s_frames[s_current];
		if (frame.Finish) frame.Finish->Issue(D3DISSUE_END);
		if (frame.Freq) frame.Freq->Issue(D3DISSUE_END);
		if (frame.Disjoint) frame.Disjoint->Issue(D3DISSUE_END);
		frame.Pending = true;
	}

	// Reuse the oldest slot, reading it first.
	s_current = (s_current + 1) % kFrames;
	Frame& frame = s_frames[s_current];
	if (frame.Pending) Collect(frame);

	if (!frame.Disjoint) frame.Disjoint = Create(D3DQUERYTYPE_TIMESTAMPDISJOINT);
	if (!frame.Freq) frame.Freq = Create(D3DQUERYTYPE_TIMESTAMPFREQ);
	if (!frame.Start) frame.Start = Create(D3DQUERYTYPE_TIMESTAMP);
	if (!frame.Finish) frame.Finish = Create(D3DQUERYTYPE_TIMESTAMP);
	if (s_unavailable) return;

	frame.Spans.clear();
	frame.PoolUsed = 0;
	frame.Disjoint->Issue(D3DISSUE_BEGIN);
	frame.Start->Issue(D3DISSUE_END);
}


void GpuTimer::Begin(const char* Label) {
	if (!s_enabled || s_current < 0) return;
	Frame& frame = s_frames[s_current];
	IDirect3DQuery9* query = NextTimestamp(frame);
	if (!query) return;
	query->Issue(D3DISSUE_END);
	int depth = 0;
	for (const Span& span : frame.Spans) if (!span.End) depth++;
	frame.Spans.push_back({ Label, query, nullptr, depth });
}


void GpuTimer::End(const char* Label) {
	if (!s_enabled || s_current < 0) return;
	Frame& frame = s_frames[s_current];
	// The most recent open span with this label.
	for (auto it = frame.Spans.rbegin(); it != frame.Spans.rend(); ++it) {
		if (!it->End && strcmp(it->Label, Label) == 0) {
			IDirect3DQuery9* query = NextTimestamp(frame);
			if (!query) return;
			query->Issue(D3DISSUE_END);
			it->End = query;
			return;
		}
	}
}


const std::vector<GpuTimer::Stat>& GpuTimer::Stats() { return s_stats; }
float GpuTimer::FrameAverageMs() { return s_frameMs; }

void GpuTimer::Reset() {
	s_stats.clear();
	s_frameMs = -1.0f;
}
