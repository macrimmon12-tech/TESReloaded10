// Depth of Field - MATSO-style implementation for New Vegas Reloaded
//
// Algorithm: four-axis directional blur (horizontal, +45, vertical, -45) with
// luminance-weighted accumulation so bright pixels punch through as shaped bokeh
// discs. Based on MATSO DOF (Matso, 2012) as seen in ENBSeries presets.
//
// Pass layout:
//   0  CoC    - compute per-pixel circle of confusion from depth, write to RGBA
//   1  Blur H - horizontal axis, init from source + CoC; carry CoC in alpha
//   2  Blur D - +45 diagonal axis
//   3  Blur V - vertical axis
//   4  Blur A - -45 diagonal axis
//   5  Combine - blend blurred result over original based on CoC

#define showCoC 0   // debug: visualise CoC instead of final image

float4 TESR_ReciprocalResolution;
float4 TESR_DepthOfFieldBlur;   // x: distant blur toggle, y: distant start, z: distant end, w: blur radius
float4 TESR_DepthOfFieldData;   // x: hyperfocal distance scale, y: (unused), z: (unused), w: near blur cutoff
float4 TESR_DOFMatsoData;       // x: BokehCurve, y: SampleCount, z: ChromaticAberration, w: AnamorphicRatio
float4 TESR_MotionBlurData;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer    : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer   : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_AvgLumaBuffer  : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"

// ---------------------------------------------------------------------------
// Unpack settings
// ---------------------------------------------------------------------------

static const bool  DistantBlur  = bool(TESR_DepthOfFieldBlur.x);
static const float DistantStart = TESR_DepthOfFieldBlur.y;
static const float DistantEnd   = TESR_DepthOfFieldBlur.z;
static const float BlurRadius   = TESR_DepthOfFieldBlur.w;   // pixels at max CoC

static const float HyperFocalDistance = max(0.0000001, TESR_DepthOfFieldData.x * 1000.0);
static const float NearBlurCutoff     = TESR_DepthOfFieldData.w;

// Animated focal distance written each frame to AvgLumaBuffer.b by AvgLuma.fx.hlsl
static const float focalDistance = tex2D(TESR_AvgLumaBuffer, float2(0.5, 0.5)).b * HyperFocalDistance;

static const float BokehCurve          = TESR_DOFMatsoData.x;  // luminance weighting exponent
static const float SampleCount         = TESR_DOFMatsoData.y;  // taps per direction per pass (PERF)
static const float ChromaticAberration = TESR_DOFMatsoData.z;  // R/B channel split strength
static const float AnamorphicRatio     = TESR_DOFMatsoData.w;  // <1 = wide oval, >1 = tall oval

// Four blur axes: horizontal, +45 diagonal, vertical, -45 diagonal
static const float2 axes[4] = {
    float2(1.0,        0.0      ),
    float2(0.7071068,  0.7071068),
    float2(0.0,        1.0      ),
    float2(0.7071068, -0.7071068)
};

// ---------------------------------------------------------------------------
// Vertex shader (shared by all passes)
// ---------------------------------------------------------------------------
struct VSOUT { float4 vertPos : POSITION;  float2 UVCoord : TEXCOORD0; };
struct VSIN  { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN IN)
{
    VSOUT OUT = (VSOUT)0.0f;
    OUT.vertPos = IN.vertPos;
    OUT.UVCoord = IN.UVCoord;
    return OUT;
}

// ---------------------------------------------------------------------------
// Pass 0 — Circle of Confusion
// Outputs float4(nearCoc, farCoc, 0, combinedCoc).
// combinedCoc (alpha) is carried through all blur passes so the combine pass
// can read it without re-accessing the depth buffer.
// ---------------------------------------------------------------------------
float4 DoF(VSOUT IN) : COLOR0
{
    float depth      = readDepth(IN.UVCoord);
    float3 camVec    = toWorld(IN.UVCoord) * depth;
    float4 worldPos  = float4(TESR_CameraPosition.xyz + camVec, 1.0);
    float  focalLen  = 0.01; // prevents div-by-zero; negligible optical effect

    float nearPlane = focalDistance * (HyperFocalDistance - focalLen)
                    / (HyperFocalDistance + focalDistance - 2.0 * focalLen);
    float farPlane  = focalDistance * (HyperFocalDistance - focalLen)
                    / max(HyperFocalDistance - focalDistance, 0.0001);

    float nearCoc = invlerps(nearPlane, 0.0, depth);
    float farCoc  = invlerps(farPlane, farPlane * 2.0, depth) * 0.8;

    // Suppress near blur within NearBlurCutoff to keep the gun/arms sharp
    nearCoc *= 0.5 * smoothstep(NearBlurCutoff * 0.5, 0.0, depth)
             + smoothstep(NearBlurCutoff * 0.5, NearBlurCutoff, depth);

    // Add constant far blur for distant LOD cover, excluding sky
    float skyMask = invlerps(100000.0, 5000.0, worldPos.z);
    farCoc = saturate(farCoc + invlerps(DistantStart, DistantEnd, depth) * DistantBlur * skyMask);

    float combinedCoc = max(nearCoc, farCoc);
    return float4(nearCoc, farCoc, 0.0, combinedCoc);
}

// ---------------------------------------------------------------------------
// Passes 1-4 — MATSO directional blur
//
// MatsoDOF_Init (pass 1): sources color from TESR_SourceBuffer (original scene);
//   CoC is read from TESR_RenderedBuffer.a (output of the CoC pass).
// MatsoDOF_Cont (passes 2-4): sources color from TESR_RenderedBuffer.rgb;
//   CoC preserved in .a from the previous blur pass.
//
// Each tap is weighted by pow(luminance, BokehCurve).  Higher BokehCurve means
// brighter pixels dominate the accumulation, producing visible bokeh disc shapes
// on specular highlights.  At BokehCurve = 1.0 the result is a plain average.
//
// PERFORMANCE: total texture fetches per pixel = (2 * SampleCount + 1) * 4 passes.
// Doubling SampleCount doubles cost; 4–6 is a good balance for modern GPUs.
// ---------------------------------------------------------------------------

// Shared inner loop — separated to avoid code duplication across two functions.
float4 MatsoDOFBlur(float2 uv, float4 center, float coc, sampler2D colorSampler, uniform int axisIndex)
{
    if (coc < 0.001)
        return float4(center.rgb, coc);

    float2 axis = axes[axisIndex];
    axis.y *= AnamorphicRatio;

    float  cw     = pow(max(luma(center.rgb), 0.001), BokehCurve);
    float4 accum  = center * cw;
    float  weight = cw;

    for (int i = 1; i <= int(SampleCount); i++) {
        float  t   = float(i) / SampleCount;
        float2 off = axis * TESR_ReciprocalResolution.xy * t * BlurRadius * coc;

        float4 tap1 = tex2D(colorSampler, uv + off);
        float4 tap2 = tex2D(colorSampler, uv - off);

        float w1 = pow(max(luma(tap1.rgb), 0.001), BokehCurve);
        float w2 = pow(max(luma(tap2.rgb), 0.001), BokehCurve);

        accum  += tap1 * w1 + tap2 * w2;
        weight += w1 + w2;
    }

    return float4((accum / max(weight, 0.0001)).rgb, coc);
}

// Pass 1: initialise blur from the original scene; CoC comes from the DoF pass stored in RenderedBuffer.a
float4 MatsoDOF_Init(VSOUT IN, uniform int axisIndex) : COLOR0
{
    float2 uv  = IN.UVCoord;
    float  coc = tex2D(TESR_RenderedBuffer, uv).a;
    float4 center = tex2D(TESR_SourceBuffer, uv);
    return MatsoDOFBlur(uv, center, coc, TESR_SourceBuffer, axisIndex);
}

// Passes 2-4: continue accumulation from the previous blur pass
float4 MatsoDOF_Cont(VSOUT IN, uniform int axisIndex) : COLOR0
{
    float2 uv  = IN.UVCoord;
    float4 prev = tex2D(TESR_RenderedBuffer, uv);
    float4 center = float4(prev.rgb, 1.0);
    return MatsoDOFBlur(uv, center, prev.a, TESR_RenderedBuffer, axisIndex);
}

// ---------------------------------------------------------------------------
// Pass 5 — Combine
// Blends the MATSO-blurred result over the original scene.
// Optional chromatic aberration splits R/B channels on blurred pixels, simulating
// the colour fringing real lenses produce in the out-of-focus region.
// ---------------------------------------------------------------------------
float4 Combine(VSOUT IN) : COLOR0
{
    float2 uv      = IN.UVCoord;
    float4 blurred = tex2D(TESR_RenderedBuffer, uv);  // final blur result + CoC in alpha
    float4 sharp   = tex2D(TESR_SourceBuffer,   uv);  // original unblurred scene
    float  coc     = blurred.a;

    float3 blurredColor;
    if (ChromaticAberration > 0.001) {
        // Shift red and blue channels in opposite horizontal directions.
        // Magnitude scales with both the CA setting and the per-pixel CoC so
        // in-focus areas are unaffected and the fringing peaks at max blur.
        float ca = ChromaticAberration * coc * TESR_ReciprocalResolution.x * 5.0;
        blurredColor = float3(
            tex2D(TESR_RenderedBuffer, uv + float2( ca, 0)).r,
            blurred.g,
            tex2D(TESR_RenderedBuffer, uv + float2(-ca, 0)).b
        );
    } else {
        blurredColor = blurred.rgb;
    }

    #if showCoC
        return float4(coc, coc, coc, 1);
    #endif

    return float4(lerp(sharp.rgb, blurredColor, coc), 1.0);
}


// ---------------------------------------------------------------------------
// Technique
// ---------------------------------------------------------------------------
technique
{
    // Pass 0: circle of confusion from depth
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 DoF();
    }

    // Pass 1: horizontal blur — init from original scene; CoC from DoF pass
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 MatsoDOF_Init(0);
    }

    // Passes 2-4: continue accumulation from previous blur pass
    pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 MatsoDOF_Cont(1); }
    pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 MatsoDOF_Cont(2); }
    pass { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 MatsoDOF_Cont(3); }

    // Pass 5: composite blurred result over original with optional chromatic aberration
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 Combine();
    }
}
