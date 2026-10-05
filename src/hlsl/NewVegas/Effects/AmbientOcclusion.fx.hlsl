// Ambient Occlusion fullscreen shader for Oblivion/Skyrim Reloaded

#define viewao 0
#define halfres 0
#define kernelSize 5
// Set to 0 to restore the legacy technique with the new DLL (A/B testing).
#define NVR_PACKED_AO 1
float4 NVR_AOLayout; // packed extent xy, inverse low-resolution dimensions zw

float4 TESR_AmbientOcclusionAOData;
float4 TESR_AmbientOcclusionData;
float4 TESR_ReciprocalResolution;
float4 TESR_FogData; // x: fog start, y: fog end, z: sun glare, w: fog power
float4 TESR_FogColor;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BlueNoiseSampler : register(s3) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D NVR_AOBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

static const float AOsamples = TESR_AmbientOcclusionAOData.x;
static const float AOstrength = TESR_AmbientOcclusionAOData.y;
static const float AOclamp = TESR_AmbientOcclusionAOData.z;
static const float AOrange = TESR_AmbientOcclusionAOData.w;
static const float AOangleBias = TESR_AmbientOcclusionData.x;
static const float AOlumThreshold = TESR_AmbientOcclusionData.y;
static const float blurDrop = TESR_AmbientOcclusionData.z;
static const float blurRadius = TESR_AmbientOcclusionData.w;
static const int startFade = 2000;
static const int endFade = 8000;
static const float2 io = float2(1.0f, 0.0f);
 
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
 
#include "Includes/Depth.hlsl"
#include "Includes/BlurDepth.hlsl"
#include "Includes/Helpers.hlsl"
#include "Includes/Normals.hlsl"


// returns a semi random float3 between 0 and 1 based on the given seed.
// tailored to return a different value for each uv coord of the screen.
float3 random(float2 seed)
{
	return tex2D(TESR_BlueNoiseSampler, (seed/256 + 0.5) / TESR_ReciprocalResolution.xy).xyz;
}

float fogCoeff(float depth){
	return saturate(invlerp(TESR_FogData.x, TESR_FogData.y, depth));
}

float4 SSAOValue(VSOUT IN, uniform float2 OffsetMask)
{
	float2 uv = IN.UVCoord.xy;

#if halfres
	clip ((IN.UVCoord.x < 0.5 && IN.UVCoord.y < 0.5)-1); // discard half the screen to render at half resolution
	uv *= 2;
#endif
	
	// generate the sampling kernel with random points in a hemisphere
	// int kernelSize = clamp(AOsamples, 0, 32);
	float uRadius = abs(AOrange);
	float bias = saturate(AOangleBias);

	float3 origin = reconstructPosition(uv);
	if (origin.z > endFade) return 1.0;

	//reorient our sample kernel along the origin's normal
	float3 normal = GetNormal(uv);

	float angle = -random(uv).x / 2 * PI; // random angle between 0 and 90degrees
	float3 kernelRotation = float3( -sin(angle), cos(angle), 0);
	float3 tangent = normalize(kernelRotation - normal * dot(kernelRotation, normal));
	float3 bitangent = cross(normal, tangent);
	float3x3 tbn = float3x3(tangent, bitangent, normal);

	// calculate occlusion by sampling depth of each point from the kernel
	float occlusion = 0.0;
	[unroll]
	for (int i = 0; i < kernelSize; ++i) {
		// generate random samples in a unit sphere (random vector coordinates from -1 to 1);
		float3 rand = random(uv + i * TESR_ReciprocalResolution.x);
		float3 sampleVector = float3 (expand(rand.xy), rand.z) * float3(OffsetMask, 1); // separate kernel
		sampleVector = mul(normalize(sampleVector), tbn);

		//randomize points distance to sphere center, making them more concentrated towards the center
		sampleVector *= random(uv * i/2);
		float scale = 1 + float(i) / float(kernelSize);
		scale = lerp(bias, 1.0f, scale * scale);
		sampleVector *= scale; 

		// get sample positions around origin:
		sampleVector *= dot(normal, sampleVector) < 0.0 ? -1.0 : 1.0; // if our sample vector goes inside the geometry, we flip it
		float3 samplePoint = origin + sampleVector * uRadius;
		
		// compare depth of the projected sample with the value from depthbuffer
		float3 screenSpaceSample = projectPosition (samplePoint);
		float sampleDepth = readDepth(screenSpaceSample.xy);
		float actualDepth = samplePoint.z;

		// range check & accumulate:
		float distance = abs(actualDepth - sampleDepth);
		float rangeCheck = distance < uRadius ? 1.0 : 0.0;
		float influence = (sampleDepth < actualDepth ? 1.0 : 0.0 ) * rangeCheck;

		// stronger strength curve in close vectors (replacing an if statement with a lerp)
		influence *= lerp(1.0 - distance * distance/(uRadius * uRadius), 1.0 - distance /uRadius, i < kernelSize / 4);
		occlusion += influence;
	}
	
	occlusion = 1.0 - occlusion/kernelSize * AOstrength;

	float fogColor = luma(TESR_FogColor.rgb);
	float darkness = clamp(lerp(occlusion, fogColor, fogCoeff(origin.z)), occlusion, 1.0);

	darkness = lerp(darkness, 1.0, saturate(invlerp(startFade, endFade, origin.z)));

	return float2(darkness, 1.0).xxxy;
}

float4 SSAO(VSOUT IN, uniform float2 OffsetMask) : COLOR0
{
	float value = SSAOValue(IN, OffsetMask).r;
	if (OffsetMask.y) value *= tex2D(TESR_RenderedBuffer, IN.UVCoord).r;
	return float4(value, value, value, 1);
}

float4 Expand(VSOUT IN) : COLOR0
{
	float2 coord = IN.UVCoord * 0.5;
	return tex2D(TESR_RenderedBuffer, coord);
}

float4 Combine(VSOUT IN) : COLOR0
{
	float3 source = tex2D(TESR_SourceBuffer, IN.UVCoord).rgb;
	float3 color = pows(source,2.2); // linearise
	float ao = lerp(AOclamp, 1.0, tex2D(TESR_RenderedBuffer, IN.UVCoord).r);

	float luminance = luma(color);
	float lt = luminance - AOlumThreshold;
	luminance = saturate(lt * 3.0);
	ao = lerp(ao, 1.0, luminance);
    #if viewao
		return float4(ao, ao, ao, 1.0f);
	#endif

	// (source^2.2 * ao)^(1/2.2) == source * ao^(1/2.2). Keep the
	// luminance test in linear space, but do the inverse gamma once for AO
	// instead of once per color channel.
	return float4(source * pow(ao, 1.0 / 2.2), 1.0f);
}
 

// perform depth aware 12 taps blur along the direction of the offsetmask
float4 NormalBlurRChannel(VSOUT IN, uniform float2 OffsetMask, uniform float blurRadius,uniform float depthDrop,uniform float endFade) : COLOR0
{
	float WeightSum = 0.114725602f;
	float4 color1 = tex2D(TESR_RenderedBuffer, IN.UVCoord) * WeightSum;
	float3 normal = GetNormal(IN.UVCoord);
	float depth = projectedDepthFromLinear(tex2D(TESR_DepthBuffer, IN.UVCoord).x);
	
    if (invertedDepth) {
        depth = 1 - depth;
    }

    float depth1 = readDepth(IN.UVCoord);
	clip(endFade - depth1);

	// coeff for blurring to increase blur depthDrop on surfaces facing away from the camera
	float normalCoeff = (0.5 + 2 * compress(dot(normal, float3(0, 0, 1))));

    for (int i = 0; i < cKernelSize; i++)
    {
		float2 uvOff = (BlurOffsets[i] * OffsetMask) * blurRadius/depth;
		float4 color2 = tex2D(TESR_RenderedBuffer, IN.UVCoord + uvOff).r;
		float depth2 = readDepth(IN.UVCoord + uvOff);
		float3 normal2 = GetNormal(IN.UVCoord + uvOff);

		float diff = abs(depth1 - depth2);

		int useForBlur = (diff <= depthDrop * normalCoeff);
		color1.r += BlurWeights[i] * color2.r * useForBlur;
		WeightSum += BlurWeights[i] * useForBlur;
    }
	
	color1.r /= WeightSum;
    return float4(color1.rgb, 1);
}


technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SSAO(io.xy);
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SSAO(io.yx);
	}

#if halfres
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Expand();
	}
#endif
	
	pass
	{ 
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 NormalBlurRChannel(io.xy, blurRadius, blurDrop, endFade);
	}
	
	pass
	{ 
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 NormalBlurRChannel(io.yx, blurRadius, blurDrop, endFade);
	}
	
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Combine();
	}
}

#if NVR_PACKED_AO
// The CPU sets a half-size viewport, and copies only that rectangle after
// each intermediate pass. No additional render-target allocations are needed.
VSOUT PackedVS(VSIN IN)
{
	VSOUT OUT = FrameVS(IN);
	OUT.UVCoord += 0.5 * (NVR_AOLayout.zw - TESR_ReciprocalResolution.xy);
	return OUT;
}

float2 PackedUV(float2 uv)
{
	// Keep depth and AO paired at a low-resolution texel, including near edges.
	uv = (floor(uv / NVR_AOLayout.zw) + 0.5) * TESR_ReciprocalResolution.xy;
	return clamp(uv, 0.5 * TESR_ReciprocalResolution.xy,
		NVR_AOLayout.xy - 0.5 * TESR_ReciprocalResolution.xy);
}

float2 PackedSSAOValues(float2 uv)
{
	float uRadius = abs(AOrange);
	float bias = saturate(AOangleBias);
	float3 origin = reconstructPosition(uv);
	if (origin.z > endFade) return 1;

	float3 normal = GetNormal(uv);
	float angle = -random(uv).x / 2 * PI;
	float3 kernelRotation = float3(-sin(angle), cos(angle), 0);
	float3 tangent = normalize(kernelRotation - normal * dot(kernelRotation, normal));
	float3 bitangent = cross(normal, tangent);
	float3x3 tbn = float3x3(tangent, bitangent, normal);
	float2 occlusion = 0;

	[unroll] for (int i = 0; i < kernelSize; ++i) {
		// The horizontal and vertical kernels use the same random vectors. Evaluate both
		// together so each pair shares its two blue-noise reads and all basis setup.
		float3 rand = random(uv + i * TESR_ReciprocalResolution.x);
		float3 raw = float3(expand(rand.xy), rand.z);
		float3 sampleX = mul(normalize(raw * float3(1, 0, 1)), tbn);
		float3 sampleY = mul(normalize(raw * float3(0, 1, 1)), tbn);
		float3 randomScale = random(uv * i / 2);
		sampleX *= randomScale;
		sampleY *= randomScale;
		float scale = lerp(bias, 1.0f, pows(1 + float(i) / float(kernelSize), 2));
		sampleX *= scale * (dot(normal, sampleX) < 0 ? -1 : 1);
		sampleY *= scale * (dot(normal, sampleY) < 0 ? -1 : 1);

		float3 pointX = origin + sampleX * uRadius;
		float3 pointY = origin + sampleY * uRadius;
		float2 sampleDepth = float2(readDepth(projectPosition(pointX).xy), readDepth(projectPosition(pointY).xy));
		float2 actualDepth = float2(pointX.z, pointY.z);
		float2 distance = abs(actualDepth - sampleDepth);
		float2 influence = (sampleDepth < actualDepth) * (distance < uRadius);
		influence *= lerp(1 - distance * distance / (uRadius * uRadius), 1 - distance / uRadius, i < kernelSize / 4);
		occlusion += influence;
	}

	float2 values = 1 - occlusion / kernelSize * AOstrength;
	float fogColor = luma(TESR_FogColor.rgb);
	values = clamp(lerp(values, fogColor, fogCoeff(origin.z)), values, 1);
	return lerp(values, 1, saturate(invlerp(startFade, endFade, origin.z)));
}

float4 PackedEstimate(VSOUT IN) : COLOR0
{
	// Same two five-sample kernels, evaluated together without duplicate setup/noise reads.
	float2 values = PackedSSAOValues(IN.UVCoord);
	float ao = values.x * values.y;
	return float4(ao, min(readDepth(IN.UVCoord), (float)endFade), 0, 1);
}

float4 PackedBlur(VSOUT IN, uniform float2 axis) : COLOR0
{
	float2 center = tex2D(TESR_RenderedBuffer, PackedUV(IN.UVCoord)).rg;
	if (center.y >= endFade) return float4(1, center.y, 0, 1);
	float rawDepth = projectedDepthFromLinear(tex2D(TESR_DepthBuffer, IN.UVCoord).x);
	if (invertedDepth) rawDepth = 1 - rawDepth;
	float depthTolerance = blurDrop * (0.5 + 2 * compress(GetNormal(IN.UVCoord).z));
	float sum = center.x * 0.114725602f;
	float weights = 0.114725602f;
	[unroll] for (int i = 0; i < cKernelSize; ++i) {
		// Preserve the original screen-space blur radius, not twice its width.
		float2 uv = IN.UVCoord + BlurOffsets[i] * axis * blurRadius / max(rawDepth, 1.0e-6);
		float2 sample = tex2D(TESR_RenderedBuffer, PackedUV(uv)).rg;
		float weight = BlurWeights[i] * (abs(center.y - sample.y) <= depthTolerance);
		sum += sample.x * weight;
		weights += weight;
	}
	return float4(sum / weights, center.y, 0, 1);
}

float4 PackedCombine(VSOUT IN) : COLOR0
{
	float2 position = IN.UVCoord / NVR_AOLayout.zw - 0.5;
	float2 base = floor(position);
	float2 fraction = frac(position);
	float depth = readDepth(IN.UVCoord);
	if (depth >= endFade) return float4(tex2D(TESR_SourceBuffer, IN.UVCoord).rgb, 1);
	float sum = 0, weights = 0;
	[unroll] for (int y = 0; y < 2; ++y) {
		[unroll] for (int x = 0; x < 2; ++x) {
			float2 uv = (base + float2(x, y) + 0.5) * NVR_AOLayout.zw;
			float2 sample = tex2D(TESR_RenderedBuffer, PackedUV(uv)).rg;
			float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
			weight /= 1 + abs(sample.y - depth) / max(blurDrop, 0.001);
			sum += sample.x * weight;
			weights += weight;
		}
	}
	float ao = lerp(AOclamp, 1, sum / max(weights, 1.0e-6));
	float3 source = tex2D(TESR_SourceBuffer, IN.UVCoord).rgb;
	float3 color = pows(source, 2.2);
	ao = lerp(ao, 1, saturate((luma(color) - AOlumThreshold) * 3));
	#if viewao
		return float4(ao, ao, ao, 1);
	#endif
	return float4(source * pow(ao, 1.0 / 2.2), 1);
}

technique PackedAO
{
	pass Estimate { VertexShader = compile vs_3_0 PackedVS(); PixelShader = compile ps_3_0 PackedEstimate(); }
	pass BlurX { VertexShader = compile vs_3_0 PackedVS(); PixelShader = compile ps_3_0 PackedBlur(io.xy); }
	pass BlurY { VertexShader = compile vs_3_0 PackedVS(); PixelShader = compile ps_3_0 PackedBlur(io.yx); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PackedCombine(); }
}

float2 DedicatedUV(float2 uv)
{
	return clamp(uv, 0.5 * NVR_AOLayout.zw, 1 - 0.5 * NVR_AOLayout.zw);
}

float4 DedicatedBlur(VSOUT IN, uniform float2 axis) : COLOR0
{
	float2 center = tex2D(NVR_AOBuffer, DedicatedUV(IN.UVCoord)).rg;
	if (center.y >= endFade) return float4(1, center.y, 0, 1);
	float rawDepth = projectedDepthFromLinear(tex2D(TESR_DepthBuffer, IN.UVCoord).x);
	if (invertedDepth) rawDepth = 1 - rawDepth;
	float depthTolerance = blurDrop * (0.5 + 2 * compress(GetNormal(IN.UVCoord).z));
	float sum = center.x * 0.114725602f;
	float weights = 0.114725602f;
	[unroll] for (int i = 0; i < cKernelSize; ++i) {
		float2 uv = IN.UVCoord + BlurOffsets[i] * axis * blurRadius / max(rawDepth, 1.0e-6);
		float2 sample = tex2D(NVR_AOBuffer, DedicatedUV(uv)).rg;
		float weight = BlurWeights[i] * (abs(center.y - sample.y) <= depthTolerance);
		sum += sample.x * weight;
		weights += weight;
	}
	return float4(sum / weights, center.y, 0, 1);
}

float4 DedicatedCombine(VSOUT IN) : COLOR0
{
	float2 position = IN.UVCoord / NVR_AOLayout.zw - 0.5;
	float2 base = floor(position);
	float2 fraction = frac(position);
	float depth = readDepth(IN.UVCoord);
	if (depth >= endFade) return float4(tex2D(TESR_SourceBuffer, IN.UVCoord).rgb, 1);
	float sum = 0, weights = 0;
	[unroll] for (int y = 0; y < 2; ++y) {
		[unroll] for (int x = 0; x < 2; ++x) {
			float2 uv = (base + float2(x, y) + 0.5) * NVR_AOLayout.zw;
			float2 sample = tex2D(NVR_AOBuffer, DedicatedUV(uv)).rg;
			float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
			weight /= 1 + abs(sample.y - depth) / max(blurDrop, 0.001);
			sum += sample.x * weight;
			weights += weight;
		}
	}
	float ao = lerp(AOclamp, 1, sum / max(weights, 1.0e-6));
	float3 source = tex2D(TESR_SourceBuffer, IN.UVCoord).rgb;
	float3 linearColor = pows(source, 2.2);
	ao = lerp(ao, 1, saturate((luma(linearColor) - AOlumThreshold) * 3));
	#if viewao
		return float4(ao, ao, ao, 1);
	#endif
	return float4(source * pow(ao, 1.0 / 2.2), 1);
}

technique DedicatedAO
{
	pass Estimate { VertexShader = compile vs_3_0 PackedVS(); PixelShader = compile ps_3_0 PackedEstimate(); }
	pass BlurX { VertexShader = compile vs_3_0 PackedVS(); PixelShader = compile ps_3_0 DedicatedBlur(io.xy); }
	pass BlurY { VertexShader = compile vs_3_0 PackedVS(); PixelShader = compile ps_3_0 DedicatedBlur(io.yx); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 DedicatedCombine(); }
}
#endif
