// Temporal anti-aliasing, phase 1: temporal accumulation with reprojection, no sub-pixel jitter.
//
// Ported from Oblivion Reloaded E3 (arafuse/tes-reloaded, OblivionReloaded/Shaders/TAA/TAA.fx.hlsl),
// which builds on:
//   https://github.com/TheRealMJP/MSAAFilter (Catmull-Rom history filter, MIT licence)
//   https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/
// Variance clipping in YCoCg follows Salvi, "An Excursion in Temporal Supersampling" (GDC 2016),
// and the AABB clip follows Playdead's INSIDE TAA (Pedersen, GDC 2016).
//
// Each frame blends the current image with the previous TAA result, fetched from where this
// pixel's surface was on screen last frame. Without jitter every frame samples the same pixel
// centres, so a still image gains nothing -- the history converges to the current frame. What
// this does fix is instability: edges and fine detail crawl as the camera moves because each
// frame lands them at a different sub-pixel offset, and averaging those frames is exactly the
// sub-pixel coverage that crawl is missing. Temporally noisy effects settle for the same reason.
//
// What changed from the ORL original, and why:
//   - Reconstruction. ORL decodes its own depth format. This uses NVR's, and reprojects by
//     inverting the exact reconstruction NVR's shipping effects use (CameraPosition +
//     toWorld(uv) * readDepth(uv)), so a static camera maps every pixel onto itself.
//   - Last frame's camera goes in as a rotation, two projection scales and a position DELTA.
//     The view matrix's translation is deliberately not used: it is built from the game global
//     CameraLocation, while TESR_CameraPosition comes from the camera node's world translate,
//     and nothing here establishes that those agree. The delta is also computed on the CPU, so
//     no absolute world coordinate -- which can run to six figures in FNV worldspaces -- ever
//     reaches the shader to lose precision in a subtraction.
//   - The first-person weapon. It moves with the camera, so reprojecting it as world geometry
//     smears it every time the view turns -- the classic TAA complaint in first-person games.
//     Its pixels are identified with the same test CombineDepth uses (a non-zero viewmodel depth,
//     reversed depth so zero is cleared) and reprojected with zero motion.
//   - Neighbourhood clamp. ORL clamps to an RGB min/max box whose radius shrinks to 0.15 px near
//     the camera. This clips to a variance box in YCoCg, which rejects stale history more
//     precisely and shifts colour less, with integer loops -- ORL's float loop counters are the
//     construct that has crashed the D3DX compiler in this codebase before.
//   - History validity. ORL blends whatever the history buffer holds. On the first frame, after
//     a cell change or after the effect is re-enabled, that is garbage or a different place;
//     TESR_TAAData.z tells this shader to ignore it.

float4 TESR_ReciprocalResolution;      // x: 1 / width, y: 1 / height
float4 TESR_TAAData;                   // x: history weight, y: clip gamma, z: history valid (0/1), w: unused
float4 TESR_TAAPrevProjection;         // x: last frame's projection _11, y: its _22
float4 TESR_TAACameraDelta;            // xyz: camera position this frame minus last frame's
float4x4 TESR_TAAPrevViewTransform;    // last frame's TESR_ViewTransform; only its rotation is read

// POINT: the neighbourhood below reads exact texels.
sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// LINEAR: the Catmull-Rom filter gets its middle taps from bilinear fetches.
sampler2D TESR_TAAHistoryBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferViewModel : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "Includes/Depth.hlsl"

static const float historyWeight = TESR_TAAData.x;
static const float clipGamma = TESR_TAAData.y;
static const float historyValid = TESR_TAAData.z;

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float3 RGBToYCoCg(float3 c)
{
	return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b,
	              0.5f * c.r - 0.5f * c.b,
	             -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}

float3 YCoCgToRGB(float3 c)
{
	return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Where this pixel's surface was on screen last frame.
//
// This inverts NVR's reconstruction. toWorld(uv) returns F + a*R + b*U, with R, U, F the camera's
// right, up and forward axes, a = (2u - 1) / P._11 and b = -(2v - 1) / P._22, so the surface sits
// at depth * toWorld(uv) from the camera. Expressing that point against last frame's axes and
// running the same relations backwards gives last frame's uv. When nothing moved, that is uv.
float2 Reproject(float2 uv, out float inFront)
{
	float3 cameraVector = toWorld(uv) * readDepth(uv);
	float3 fromPrevCamera = cameraVector + TESR_TAACameraDelta.xyz;

	// Same axis layout toWorld reads out of TESR_ViewTransform.
	float3 prevRight   = float3(TESR_TAAPrevViewTransform[0][0], TESR_TAAPrevViewTransform[1][0], TESR_TAAPrevViewTransform[2][0]);
	float3 prevUp      = float3(TESR_TAAPrevViewTransform[0][1], TESR_TAAPrevViewTransform[1][1], TESR_TAAPrevViewTransform[2][1]);
	float3 prevForward = float3(TESR_TAAPrevViewTransform[0][2], TESR_TAAPrevViewTransform[1][2], TESR_TAAPrevViewTransform[2][2]);

	float viewZ = dot(fromPrevCamera, prevForward);
	inFront = viewZ > 0.0f ? 1.0f : 0.0f;

	float2 ndc = float2(dot(fromPrevCamera, prevRight) * TESR_TAAPrevProjection.x,
	                    dot(fromPrevCamera, prevUp) * TESR_TAAPrevProjection.y) / max(viewZ, 1e-4f);
	return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

// Catmull-Rom filtered history fetch in 9 taps instead of 16. After MJP (MIT licence):
// https://gist.github.com/TheRealMJP/bc503b0b87b643d3505d41eab8b332ae (the link ORL credits)
//
// Reprojected uvs almost never land on texel centres, and plain bilinear resampling blurs the
// history a little every frame -- compounded over the ~10 frames a 0.9 weight averages, that is
// visible softening in motion. Catmull-Rom keeps it sharp. It can overshoot at hard edges, which
// the clip in Resolve absorbs.
float3 SampleHistory(float2 uv)
{
	float2 texelSize = TESR_ReciprocalResolution.xy;
	float2 samplePos = uv / texelSize;
	float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
	float2 f = samplePos - texPos1;

	float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	float2 w3 = f * f * (-0.5f + 0.5f * f);

	float2 w12 = w1 + w2;
	float2 offset12 = w2 / w12;

	float2 texPos0 = (texPos1 - 1.0f) * texelSize;
	float2 texPos3 = (texPos1 + 2.0f) * texelSize;
	float2 texPos12 = (texPos1 + offset12) * texelSize;

	float3 result = 0.0f;
	result += tex2D(TESR_TAAHistoryBuffer, float2(texPos0.x,  texPos0.y)).rgb  * w0.x  * w0.y;
	result += tex2D(TESR_TAAHistoryBuffer, float2(texPos12.x, texPos0.y)).rgb  * w12.x * w0.y;
	result += tex2D(TESR_TAAHistoryBuffer, float2(texPos3.x,  texPos0.y)).rgb  * w3.x  * w0.y;

	result += tex2D(TESR_TAAHistoryBuffer, float2(texPos0.x,  texPos12.y)).rgb * w0.x  * w12.y;
	result += tex2D(TESR_TAAHistoryBuffer, float2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y;
	result += tex2D(TESR_TAAHistoryBuffer, float2(texPos3.x,  texPos12.y)).rgb * w3.x  * w12.y;

	result += tex2D(TESR_TAAHistoryBuffer, float2(texPos0.x,  texPos3.y)).rgb  * w0.x  * w3.y;
	result += tex2D(TESR_TAAHistoryBuffer, float2(texPos12.x, texPos3.y)).rgb  * w12.x * w3.y;
	result += tex2D(TESR_TAAHistoryBuffer, float2(texPos3.x,  texPos3.y)).rgb  * w3.x  * w3.y;

	return max(result, 0.0f);
}

// Pull the history toward the box centre until it lies inside the box. Clipping along the line
// to the centre, rather than clamping each channel on its own, keeps the history's hue instead of
// snapping it toward a box corner.
float3 ClipToBox(float3 history, float3 boxMin, float3 boxMax)
{
	float3 center = 0.5f * (boxMax + boxMin);
	float3 extents = 0.5f * (boxMax - boxMin) + 1e-4f;
	float3 offset = history - center;
	float3 units = abs(offset / extents);
	float maxUnit = max(units.x, max(units.y, units.z));
	return maxUnit > 1.0f ? center + offset / maxUnit : history;
}

float4 Resolve(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float2 texelSize = TESR_ReciprocalResolution.xy;

	// Current-frame 3x3 neighbourhood in YCoCg: its range, and its mean and spread.
	float3 current = 0.0f;
	float3 moment1 = 0.0f;
	float3 moment2 = 0.0f;
	float3 neighbourMin = 1e5f;
	float3 neighbourMax = -1e5f;
	[unroll]
	for (int y = -1; y <= 1; y++) {
		[unroll]
		for (int x = -1; x <= 1; x++) {
			float3 c = RGBToYCoCg(tex2D(TESR_SourceBuffer, uv + float2(x, y) * texelSize).rgb);
			if (x == 0 && y == 0) current = c;
			moment1 += c;
			moment2 += c * c;
			neighbourMin = min(neighbourMin, c);
			neighbourMax = max(neighbourMax, c);
		}
	}
	moment1 /= 9.0f;
	moment2 /= 9.0f;
	float3 sigma = sqrt(max(moment2 - moment1 * moment1, 0.0f));

	// Variance box, never wider than the range actually present. The mean always sits inside
	// both, so the intersection cannot come out inverted.
	float3 boxMin = max(neighbourMin, moment1 - clipGamma * sigma);
	float3 boxMax = min(neighbourMax, moment1 + clipGamma * sigma);

	float inFront;
	float2 prevUV = Reproject(uv, inFront);

	// The weapon is fixed to the camera, so it was at this same pixel last frame.
	float isViewModel = tex2D(TESR_DepthBufferViewModel, uv).x > 0.0f ? 1.0f : 0.0f;
	prevUV = lerp(prevUV, uv, isViewModel);
	inFront = max(inFront, isViewModel);

	float onScreen = all(prevUV == saturate(prevUV)) ? 1.0f : 0.0f;

	float3 history = ClipToBox(RGBToYCoCg(SampleHistory(prevUV)), boxMin, boxMax);

	// A surface that was off screen or behind the camera last frame has no history to use, and
	// neither does anything on the first frame after a reset.
	float weight = historyWeight * historyValid * onScreen * inFront;

	return float4(YCoCgToRGB(lerp(current, history, weight)), 1.0f);
}

// Copies the resolved frame, which lives in the FP16 history buffer, back to the screen.
float4 Output(VSOUT IN) : COLOR0
{
	return float4(tex2D(TESR_TAAHistoryBuffer, IN.UVCoord).rgb, 1.0f);
}

technique Resolve
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Resolve();
	}
}

technique Output
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Output();
	}
}
