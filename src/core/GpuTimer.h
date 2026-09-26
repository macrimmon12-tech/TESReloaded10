#pragma once

// GPU time per stage of the frame, from D3D9 timestamp queries.
//
// The per-effect times the menu shows in debug mode are CPU times: they measure how long the
// commands took to submit, and D3D9 runs them later, so they say nothing about GPU cost. This
// measures the GPU itself. Every call returns at once unless SetEnabled(true) - the GPU Timings
// panel does that while it is open - so it costs nothing otherwise.
//
// Results are read back several frames after they were issued and never waited on, so measuring
// does not stall the pipeline. A frame whose results are not ready yet, or whose timestamps the
// driver reports as disjoint (a clock change mid frame), is dropped rather than guessed at.
class GpuTimer {
public:
	struct Stat {
		std::string	Label;
		float		AverageMs;
		float		LastMs;
		int			Depth;		// spans open around it when first seen, for indenting
	};

	static void		SetEnabled(bool Enabled);
	static bool		IsEnabled();
	static bool		IsAvailable();

	// The end of one frame's work and the start of the next. Called once per frame, after the
	// last post process effect.
	static void		FrameBoundary();

	// A span. Labels must outlive the frame they are issued in: string literals or effect names.
	// Spans may nest and a label may repeat within a frame; repeats are summed.
	static void		Begin(const char* Label);
	static void		End(const char* Label);

	static const std::vector<Stat>& Stats();
	static float	FrameAverageMs();
	static void		Reset();
};
