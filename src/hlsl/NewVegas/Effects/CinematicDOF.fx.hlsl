// Cinematic depth of field: a thin-lens circle of confusion, gathered bokeh with separate near and
// far fields, and autofocus on world depth.
//
// The gather follows the scatter-as-gather design Unity's Post-Processing Stack v2 shipped
// (Keijiro Takahashi's Kino Bokeh lineage), itself in the family of Jimenez, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014). It is written from the algorithm,
// not copied: the kernel here is a golden-angle spiral generated in the loop rather than a table.
//
// Pipeline (CinematicDOFEffect::Render drives it; half = half resolution):
//   Focus       1x1   autofocus distance, eased over time         FocusA/B ping-pong
//   Prefilter   half  2x2 downsample: colour + signed CoC          -> HalfA
//   Bokeh       half  disc gather, near and far fields apart       HalfA -> HalfB
//   Postfilter  half  tent filter to hide the gather's sampling    HalfB -> HalfA
//   Combine     full  blend the blurred layers over the sharp frame
//
// What makes it read as a camera rather than a blur filter:
//
//  - Thin-lens CoC. Blur is f^2 / (N (zf - f)) * (z - zf) / z, from a focal length, f-stop and focus
//    distance. Behind the focus plane it levels off toward a maximum; in front it keeps growing. That
//    asymmetry is what an eye reads as optics. The CoC is signed: negative near, positive far.
//
//  - Near field spreads past its own outline. The previous NVR DoF blended each pixel by its OWN
//    near-blur amount, so an out-of-focus foreground object ended in a sharp edge. Here every pixel
//    gathers over the full kernel, and a sample from a near-field object contributes wherever its
//    CoC reaches -- so the blur of a foreground object bleeds over the background behind it, as it
//    does through a real lens. The far field is the opposite: a sample only counts if it AND the
//    centre are out of focus (min of the two CoCs), so a sharp foreground never haloes the blur
//    behind it.
//
//  - Bokeh from highlights. This runs after tonemapping, so there is no HDR to make bright points
//    swell into discs. HighlightBoost inverts a Reinhard curve on the way in -- c / (1 - k*max(c))
//    -- so bright pixels outweigh their neighbours through the gather, and the exact inverse,
//    c / (1 + k*max(c)), puts the range back afterwards. Dark and mid tones are nearly untouched.
//
//  - Autofocus on WORLD depth. The combined depth buffer contains the first-person weapon, and when
//    aiming down sights the sights sit dead centre -- autofocus on that would focus on the gun and
//    blur the world. The world-only depth buffer is decoded with CombineDepth's own conversion.

#define PI 3.14159265f

float4 TESR_ReciprocalResolution;   // x: 1 / width, y: 1 / height
float4 TESR_GameTime;               // w: frame time, seconds
float4 TESR_CinematicDOFLens;       // x: lens coefficient f^2 * aspect / (N * 36mm), y: max CoC (screen heights), z: highlight boost, w: weapon blur
float4 TESR_CinematicDOFFocus;      // x: manual focus distance (units), y: autofocus (0/1), z: focus easing this frame, w: previous focus valid (0/1)
float4 TESR_CinematicDOFData;       // x: effect strength 0-1 (fades in and out), y: min focus distance (units), z: focal length (mm), w: debug view

sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferWorld : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferViewModel : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// s4-s6 are bound explicitly per pass by CinematicDOFEffect::Render, so no pass ever samples the
// texture it is rendering into. The names are what SetCT binds them to initially.
sampler2D TESR_CinematicDOFFocusA : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_CinematicDOFHalfA : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_CinematicDOFHalfB : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

#include "Includes/Depth.hlsl"

static const float lensCoeff = TESR_CinematicDOFLens.x;
static const float maxCoC = TESR_CinematicDOFLens.y;
static const float highlightBoost = TESR_CinematicDOFLens.z;
static const float weaponBlur = TESR_CinematicDOFLens.w;
static const float manualFocus = TESR_CinematicDOFFocus.x;
static const float autoFocus = TESR_CinematicDOFFocus.y;
static const float focusEasing = TESR_CinematicDOFFocus.z;
static const float focusValid = TESR_CinematicDOFFocus.w;
static const float strength = TESR_CinematicDOFData.x;
static const float minFocus = TESR_CinematicDOFData.y;
static const float focalLength = TESR_CinematicDOFData.z;
static const float debugView = TESR_CinematicDOFData.w;

// One game unit is 0.5625 in, 14.2875 mm (roughly 70 units to the metre).
static const float UNIT_MM = 14.2875f;

// Golden-angle spiral: sample i sits at radius sqrt((i + 0.5) / N) and angle i * 137.5 degrees, which
// covers a disc evenly for any N. The direction is advanced by a fixed rotation, so the loop needs no
// sin or cos. Compile-time count: a runtime loop bound is the construct that has crashed the D3DX
// compiler in this codebase before.
static const int SAMPLE_COUNT = 48;
static const float GOLDEN_COS = -0.7373688f;   // cos(2.3999632)
static const float GOLDEN_SIN = 0.6754903f;    // sin(2.3999632)

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

float Max3(float3 c)
{
	return max(c.r, max(c.g, c.b));
}

// Inverse Reinhard on the way in, and its exact inverse on the way out. See the header.
float3 ExpandHighlights(float3 c)
{
	return c / max(1.0f - highlightBoost * Max3(c), 0.05f);
}

float3 CompressHighlights(float3 c)
{
	return c / (1.0f + highlightBoost * Max3(c));
}

float FocusDistance()
{
	return tex2D(TESR_CinematicDOFFocusA, float2(0.5f, 0.5f)).x;
}

// World-only view depth, decoded exactly as CombineDepth does it (reversed depth: 0 is the far plane).
float WorldViewZ(float2 uv)
{
	float d = tex2D(TESR_DepthBufferWorld, uv).x;
	return nearZ * farZ / (nearZ + d * (farZ - nearZ));
}

// Signed circle of confusion in screen heights: negative in front of the focus plane, positive behind.
float CircleOfConfusion(float depth, float focus)
{
	float focusMM = focus * UNIT_MM;
	float coc = lensCoeff * (depth - focus) / (max(depth, 1.0f) * max(focusMM - focalLength, focalLength));
	return clamp(coc * strength, -maxCoC, maxCoC);
}

// CoC at a full-resolution pixel, with the first-person weapon scaled by WeaponBlur.
float PixelCoC(float2 uv, float focus)
{
	float coc = CircleOfConfusion(readDepth(uv), focus);
	float isViewModel = tex2D(TESR_DepthBufferViewModel, uv).x > 0.0f ? 1.0f : 0.0f;
	return coc * lerp(1.0f, weaponBlur, isViewModel);
}

// ---- Focus (1x1) ----
// The nearest world surface among five taps around the screen centre, so what is under the crosshair
// wins over the background behind it, eased toward over time the way a real autofocus hunts.
float4 FocusPS(VSOUT IN) : COLOR0
{
	// Sampled unconditionally and selected after: texture fetches that need gradients are not
	// allowed inside dynamic flow control in ps_3_0.
	float nearest = WorldViewZ(float2(0.5f, 0.5f));
	nearest = min(nearest, WorldViewZ(float2(0.48f, 0.5f)));
	nearest = min(nearest, WorldViewZ(float2(0.52f, 0.5f)));
	nearest = min(nearest, WorldViewZ(float2(0.5f, 0.48f)));
	nearest = min(nearest, WorldViewZ(float2(0.5f, 0.52f)));
	float target = max(autoFocus > 0.5f ? nearest : manualFocus, minFocus);

	float previous = FocusDistance();
	float focus = focusValid > 0.5f ? lerp(previous, target, focusEasing) : target;
	return float4(focus, focus, 0.0f, 1.0f);
}

// ---- Prefilter (half) ----
// The quad's UVs carry a half-full-resolution-texel offset (ShaderManager::CreateFrameVertex), so at
// half resolution IN.UVCoord lands on the centre of the first full-res texel of this pixel's 2x2
// block, and the other three are one texel right and/or down.
float4 PrefilterPS(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float2 texel = TESR_ReciprocalResolution.xy;
	float focus = FocusDistance();

	float2 uv0 = uv;
	float2 uv1 = uv + float2(texel.x, 0.0f);
	float2 uv2 = uv + float2(0.0f, texel.y);
	float2 uv3 = uv + texel;

	float3 c0 = ExpandHighlights(tex2D(TESR_SourceBuffer, uv0).rgb);
	float3 c1 = ExpandHighlights(tex2D(TESR_SourceBuffer, uv1).rgb);
	float3 c2 = ExpandHighlights(tex2D(TESR_SourceBuffer, uv2).rgb);
	float3 c3 = ExpandHighlights(tex2D(TESR_SourceBuffer, uv3).rgb);

	float coc0 = PixelCoC(uv0, focus);
	float coc1 = PixelCoC(uv1, focus);
	float coc2 = PixelCoC(uv2, focus);
	float coc3 = PixelCoC(uv3, focus);

	// Weight by blur, so in-focus pixels do not leak into the blurred average of their block.
	float w0 = abs(coc0) + 1e-5f;
	float w1 = abs(coc1) + 1e-5f;
	float w2 = abs(coc2) + 1e-5f;
	float w3 = abs(coc3) + 1e-5f;
	float3 color = (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / (w0 + w1 + w2 + w3);

	// Keep the strongest blur in the block, near or far, with its sign.
	float cocMin = min(min(coc0, coc1), min(coc2, coc3));
	float cocMax = max(max(coc0, coc1), max(coc2, coc3));
	float coc = -cocMin > cocMax ? cocMin : cocMax;

	// Fade in-focus colour to black here: the gather must not pick up sharp pixels, and the Combine
	// pass takes in-focus areas from the full-resolution frame anyway.
	color *= smoothstep(0.0f, texel.y * 2.0f, abs(coc));
	return float4(color, coc);
}

// ---- Bokeh (half) ----
float4 BokehPS(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord + 0.5f * TESR_ReciprocalResolution.xy;   // half-res texel centre
	float halfTexel = TESR_ReciprocalResolution.y * 2.0f;           // one half-res texel, screen heights
	float margin = halfTexel * 2.0f;
	float heightToU = TESR_ReciprocalResolution.x / TESR_ReciprocalResolution.y;   // height / width

	float4 center = tex2D(TESR_CinematicDOFHalfA, uv);
	float4 farAcc = 0.0f;
	float4 nearAcc = 0.0f;
	float2 direction = float2(1.0f, 0.0f);

	[loop]
	for (int i = 0; i < SAMPLE_COUNT; i++) {
		float radius = sqrt(((float)i + 0.5f) / (float)SAMPLE_COUNT) * maxCoC;
		float2 offset = direction * radius;
		float4 s = tex2Dlod(TESR_CinematicDOFHalfA, float4(uv + float2(offset.x * heightToU, offset.y), 0.0f, 0.0f));

		// Far field: only as much blur as both this sample and the centre have, so an in-focus
		// foreground never spreads into the background's blur.
		float farCoC = max(min(center.a, s.a), 0.0f);
		float farWeight = saturate((farCoC - radius + margin) / margin);

		// Near field: the sample's own blur decides its reach, so it spreads past its outline.
		float nearWeight = saturate((-s.a - radius + margin) / margin);
		nearWeight *= step(halfTexel, -s.a);

		farAcc += float4(s.rgb, 1.0f) * farWeight;
		nearAcc += float4(s.rgb, 1.0f) * nearWeight;

		direction = float2(direction.x * GOLDEN_COS - direction.y * GOLDEN_SIN,
		                   direction.x * GOLDEN_SIN + direction.y * GOLDEN_COS);
	}

	farAcc.rgb /= farAcc.a + (farAcc.a == 0.0f ? 1.0f : 0.0f);
	nearAcc.rgb /= nearAcc.a + (nearAcc.a == 0.0f ? 1.0f : 0.0f);

	// How much of this pixel the near field covers: the weights' total, as a fraction of the disc.
	float nearAlpha = saturate(nearAcc.a * PI / (float)SAMPLE_COUNT);
	return float4(lerp(farAcc.rgb, nearAcc.rgb, nearAlpha), nearAlpha);
}

// ---- Postfilter (half) ----
// Four bilinear taps half a texel out make a 9-tap tent, smoothing the gather's sampling pattern.
float4 PostfilterPS(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord + 0.5f * TESR_ReciprocalResolution.xy;
	float2 d = TESR_ReciprocalResolution.xy;   // half a half-res texel
	float4 acc = tex2D(TESR_CinematicDOFHalfB, uv + float2(-d.x, -d.y));
	acc += tex2D(TESR_CinematicDOFHalfB, uv + float2(d.x, -d.y));
	acc += tex2D(TESR_CinematicDOFHalfB, uv + float2(-d.x, d.y));
	acc += tex2D(TESR_CinematicDOFHalfB, uv + d);
	acc *= 0.25f;
	return float4(CompressHighlights(acc.rgb), acc.a);
}

// ---- Combine (full) ----
float4 CombinePS(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float coc = PixelCoC(uv, FocusDistance());
	float3 color = tex2D(TESR_SourceBuffer, uv).rgb;
	float4 dof = tex2D(TESR_CinematicDOFHalfA, uv);

	// Far field from this pixel's own full-resolution CoC; near field from how much the gather says
	// foreground blur covers it. Combined as two layers of coverage.
	float texel = TESR_ReciprocalResolution.y;
	float farAlpha = smoothstep(texel * 2.0f, texel * 4.0f, coc);
	float alpha = farAlpha + dof.a - farAlpha * dof.a;
	float3 result = lerp(color, dof.rgb, alpha);

	// DebugView 1: red = far blur, blue = near blur, full brightness at MaxBlur; black = in focus.
	float3 cocView = float3(saturate(coc / maxCoC), 0.0f, saturate(-coc / maxCoC));
	result = debugView > 0.5f ? cocView : result;
	return float4(result, 1.0f);
}

technique Focus
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 FocusPS();
	}
}

technique Prefilter
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 PrefilterPS();
	}
}

technique Bokeh
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BokehPS();
	}
}

technique Postfilter
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 PostfilterPS();
	}
}

technique Combine
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombinePS();
	}
}
