// CPU model of the interior point-light shadow changes (PointShadows.fx / ShadowsInteriors.fx).
//
//  1. Point lights: GetPointLightAtten() is exactly 0 for a normalised distance >= 1, so skipping the
//     cubemap read there (and treating a zero radius, i.e. INF/NaN distance, the same way) is exact.
//  2. Shadow blur: the original interior "Shadow apply" ran DepthBlur twice on the HDR frame (fp16
//     intermediates, clip() leaving a black cleared destination beyond the draw distance). The
//     dedicated path runs InteriorShadowBlur on G16R16 scratch targets (unorm16 intermediates, an
//     early-out when all taps equal the centre, black beyond the distance). Both are modelled on
//     synthetic scenes; the difference in the blurred mask must stay far below one 8-bit step.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static float saturatef(float v) { return std::min(1.0f, std::max(0.0f, v)); }

// ---- 1. point light attenuation (Shadows.hlsl GetPointLightAtten) ----
static float PointLightAtten(float distance, float ndotl)
{
	const float s = saturatef(distance * distance);
	const float atten = saturatef(((1 - s) * (1 - s)) / (1 + 5.0f * s));
	const float t = saturatef(distance / 0.2f), smooth = t * t * (3 - 2 * t);
	const float diffuse = 1 + (ndotl - 1) * smooth;
	return saturatef(diffuse * atten);
}

// ---- 2. the two blur chains ----
static const int kKernel = 12;
static const float kWeights[kKernel] = { 0.057424882f, 0.058107773f, 0.061460144f, 0.071020611f, 0.088092873f, 0.106530916f,
	0.106530916f, 0.088092873f, 0.071020611f, 0.061460144f, 0.058107773f, 0.057424882f };
static const int kOffsets[kKernel] = { -6, -5, -4, -3, -2, -1, 1, 2, 3, 4, 5, 6 };

static float RoundHalf(float x) // fp16 storage of a value in [0, 1]
{
	if (x == 0.0f) return 0.0f;
	int e;
	std::frexp(x, &e);
	const double q = (e >= -13) ? std::ldexp(1.0, e - 11) : std::ldexp(1.0, -24);
	return (float)(std::nearbyint(x / q) * q);
}
static float RoundUnorm16(float x) { return std::nearbyint(saturatef(x) * 65535.0f) / 65535.0f; }

struct Image {
	int w, h;
	std::vector<float> v;
	Image(int w_, int h_, float fill = 0) : w(w_), h(h_), v((size_t)w_ * h_, fill) {}
	float at(int x, int y) const { x = std::max(0, std::min(w - 1, x)); y = std::max(0, std::min(h - 1, y)); return v[(size_t)y * w + x]; } // CLAMP
};

static const float kFarZ = 100000.0f, kMaxDistance = 4000.0f, kDepthDrop = 3500.0f;

// DepthBlur(): every pixel does the depth-weighted blur; pixels beyond endFade are clip()ped, i.e. keep the cleared 0.
static Image OriginalBlur(const Image& src, const Image& depth, bool horizontal, float (*store)(float))
{
	Image out(src.w, src.h, 0.0f);
	for (int y = 0; y < src.h; y++)
		for (int x = 0; x < src.w; x++) {
			const float depth1 = depth.at(x, y);
			if (!(kMaxDistance - depth1 >= 0)) continue; // clip
			float weightSum = 0.114725602f, color = src.at(x, y) * weightSum;
			const float drop = kDepthDrop * (depth1 / kFarZ);
			for (int i = 0; i < kKernel; i++) {
				const int sx = x + (horizontal ? kOffsets[i] : 0), sy = y + (horizontal ? 0 : kOffsets[i]);
				const bool use = std::fabs(depth1 - depth.at(sx, sy)) <= drop;
				color += kWeights[i] * src.at(sx, sy) * use;
				weightSum += kWeights[i] * use;
			}
			out.v[(size_t)y * src.w + x] = store(color / weightSum);
		}
	return out;
}

// InteriorShadowBlur(): black beyond the distance, early-out when all taps equal the centre.
static Image DedicatedBlur(const Image& src, const Image& depth, bool horizontal, float (*store)(float))
{
	Image out(src.w, src.h, 0.0f);
	for (int y = 0; y < src.h; y++)
		for (int x = 0; x < src.w; x++) {
			const float center = src.at(x, y), depth1 = depth.at(x, y);
			if (depth1 > kMaxDistance) continue; // return black
			float taps[kKernel], spread = 0;
			for (int i = 0; i < kKernel; i++) {
				taps[i] = src.at(x + (horizontal ? kOffsets[i] : 0), y + (horizontal ? 0 : kOffsets[i]));
				spread = std::max(spread, std::fabs(taps[i] - center));
			}
			if (spread < 0.0001f) { out.v[(size_t)y * src.w + x] = store(center); continue; }
			float weightSum = 0.114725602f, color = center * weightSum;
			const float drop = kDepthDrop * (depth1 / kFarZ);
			for (int i = 0; i < kKernel; i++) {
				const int sx = x + (horizontal ? kOffsets[i] : 0), sy = y + (horizontal ? 0 : kOffsets[i]);
				const bool use = std::fabs(depth1 - depth.at(sx, sy)) <= drop;
				color += kWeights[i] * taps[i] * use;
				weightSum += kWeights[i] * use;
			}
			out.v[(size_t)y * src.w + x] = store(color / weightSum);
		}
	return out;
}

static void MakeScene(std::mt19937& rng, Image& depth, Image& shadow)
{
	std::uniform_real_distribution<float> unit(0, 1);
	const int w = depth.w, h = depth.h;
	// depth: a floor gradient with a few rectangular steps, some regions beyond the draw distance
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			depth.v[(size_t)y * w + x] = 300.0f + 2500.0f * (float)y / h + 400.0f * std::sin(x * 0.05f);
	for (int r = 0; r < 12; r++) {
		const int x0 = (int)(unit(rng) * w), y0 = (int)(unit(rng) * h), x1 = std::min(w, x0 + 10 + (int)(unit(rng) * 80)), y1 = std::min(h, y0 + 10 + (int)(unit(rng) * 60));
		const float d = (r % 4 == 0) ? 4200.0f + unit(rng) * 1500.0f : 150.0f + unit(rng) * 3500.0f; // every 4th: beyond kMaxDistance
		for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) depth.v[(size_t)y * w + x] = d;
	}
	// shadow: smooth light blobs (values 0..1), flat zero and flat one areas, all as unorm16 (the real input format)
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float s = 0;
			for (int b = 0; b < 4; b++) {
				const float cx = (b * 97 % w), cy = (b * 53 % h), d = std::hypot(x - cx, y - cy) / (40.0f + 15 * b);
				const float sd = saturatef(d * d);
				s += saturatef(((1 - sd) * (1 - sd)) / (1 + 5.0f * sd));
			}
			if (x > w * 3 / 4) s = 1.0f;   // fully lit strip
			if (y > h * 3 / 4) s = 0.0f;   // outside every light
			shadow.v[(size_t)y * w + x] = RoundUnorm16(s);
		}
	// hard-edged blockers inside the lit area
	for (int r = 0; r < 8; r++) {
		const int x0 = (int)(unit(rng) * w), y0 = (int)(unit(rng) * h);
		for (int y = y0; y < std::min(h, y0 + 12); y++) for (int x = x0; x < std::min(w, x0 + 12); x++) shadow.v[(size_t)y * w + x] = RoundUnorm16(shadow.v[(size_t)y * w + x] * 0.3f);
	}
}

int main()
{
	// 1. atten is exactly zero from distance 1 upward, including huge values, INF, and NaN treated as "skip"
	bool exactZero = true;
	std::mt19937 rng(42);
	std::uniform_real_distribution<float> u(0, 1);
	for (int i = 0; i < 200000; i++) {
		const float d = 1.0f + std::pow(10.0f, u(rng) * 30.0f - 1.0f) * u(rng); // 1 .. ~1e29
		exactZero &= PointLightAtten(d, u(rng) * 2 - 1) == 0.0f;
	}
	exactZero &= PointLightAtten(1.0f, 0.5f) == 0.0f;
	exactZero &= PointLightAtten(std::numeric_limits<float>::infinity(), 0.5f) == 0.0f;
	CHECK(exactZero, "point light attenuation is exactly 0 for any normalised distance >= 1 (the cubemap read cannot matter)");
	CHECK(!(std::numeric_limits<float>::quiet_NaN() < 1.0f) && !(std::numeric_limits<float>::infinity() < 1.0f) && (0.999f < 1.0f),
		"!(distance < 1) skips NaN and INF (zero-radius light) and keeps distances inside the radius");

	// 2. blur chains on random scenes
	float worstMask = 0, worstEarlyOnly = 0;
	long pixels = 0, skipped = 0;
	for (int scene = 0; scene < 12; scene++) {
		Image depth(320, 180), shadow(320, 180);
		MakeScene(rng, depth, shadow);
		Image o0 = OriginalBlur(shadow, depth, true, RoundHalf), o1 = OriginalBlur(o0, depth, false, RoundHalf);
		Image n0 = DedicatedBlur(shadow, depth, true, RoundUnorm16), n1 = DedicatedBlur(n0, depth, false, RoundUnorm16);
		// the same dedicated chain but with fp16 stores: isolates the early-out from the storage format
		Image e0 = DedicatedBlur(shadow, depth, true, RoundHalf), e1 = DedicatedBlur(e0, depth, false, RoundHalf);
		for (size_t i = 0; i < o1.v.size(); i++) {
			worstMask = std::max(worstMask, std::fabs(o1.v[i] - n1.v[i]));
			worstEarlyOnly = std::max(worstEarlyOnly, std::fabs(o1.v[i] - e1.v[i]));
			pixels++;
			if (depth.v[i] > kMaxDistance) skipped++;
		}
	}
	std::printf("      %ld pixels over 12 scenes (%ld beyond the draw distance)\n", pixels, skipped);
	std::printf("      worst |original - dedicated| blurred mask: %.6f (1/255 = %.6f)\n", worstMask, 1.0f / 255.0f);
	std::printf("      of which the early-out alone (same fp16 storage): %.6f\n", worstEarlyOnly);
	CHECK(worstMask < 1.0f / 1020.0f, "dedicated blur chain matches the original within a quarter of an 8-bit step");
	CHECK(worstEarlyOnly < 3.0e-4f, "the uniform early-out alone changes the mask by less than 3e-4");

	std::printf(failures ? "\n%d check(s) FAILED\n" : "\nAll interior shadow math checks passed\n", failures);
	return failures ? 1 : 0;
}
