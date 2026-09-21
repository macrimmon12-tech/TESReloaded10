// GodRays full screen shader for Oblivion/Skyrim Reloaded

float4 TESR_ReciprocalResolution;
float4 TESR_GameTime;
float4 TESR_SunColor;
float4 TESR_GodRaysRay; // x: intensity, y:length, z: density, w: visibility -- Classic only
float4 TESR_GodRaysRayColor; // x:r, y:g, z:b, w:saturate
float4 TESR_GodRaysData; // x: passes amount, y: luminance, z:multiplier, w: time enabled
// Enhanced-only tuning. Kept separate from TESR_GodRaysRay rather than sharing its slots: Enhanced's
// raymarch parameters (a per-step decay factor, a step spacing) don't share sensible value ranges
// with Classic's RayLength/RayDensity (a blur step multiplier, an unused legacy slot), so reusing
// those would make tuning one technique fight the other.
float4 TESR_GodRaysEnhanced; // x: RayDecay, y: RayStepScale, z: BlurStrength, w: GlareStrength
float4 TESR_ViewSpaceLightDir; // view space light vector
float4 TESR_SunDirection; // worldspace sun light vector
float4 TESR_SunPosition; // worldspace sundisk position
float4 TESR_ShadowFade; // attenuation factor of sunsets/sunrises and moon phases
float4 TESR_SunAmount;
float4 TESR_SunsetColor;
float4 TESR_DebugVar;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_AvgLumaBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Sky.hlsl"

static const float raspect = 1.0f / TESR_ReciprocalResolution.z;
static const float samples = 10;
static const float stepLength = 1/samples;
static const float scale = 0.5;
static const float4x4 ditherMat = {{0.0588, 0.5294, 0.1765, 0.6471},
									{0.7647, 0.2941, 0.8824, 0.4118},
									{0.2353, 0.7059, 0.1176, 0.5882},
									{0.9412, 0.4706, 0.8235, 0.3259}};

static const float lumTreshold = TESR_GodRaysData.y;
static const float multiplier = TESR_GodRaysData.z;
static const float intensity = TESR_GodRaysRay.x;
static const float stepLengthMult = TESR_GodRaysRay.y;
static const float glareReduction = TESR_GodRaysRay.z;
static const float godrayCurve = TESR_GodRaysRay.w;
static const float sunHeight = 1 - shade(TESR_SunPosition.xyz, blue.xyz);

// Enhanced technique tuning -- step count reuses TESR_GodRaysData.x (same field Classic leaves
// unused; see its TOML doc), everything else comes from the dedicated TESR_GodRaysEnhanced vector.
static const int GodRaysPasses = max(1, int(TESR_GodRaysData.x));
static const float RayDecay = saturate(TESR_GodRaysEnhanced.x);
static const float RayStepScale = max(0, TESR_GodRaysEnhanced.y);
static const float BlurStrength = max(0, TESR_GodRaysEnhanced.z);
static const float GlareStrength = max(0, TESR_GodRaysEnhanced.w);

struct VSOUT {
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};
 
struct VSIN {
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};
 
VSOUT FrameVS(VSIN IN) {
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float4 SkyMask(VSOUT IN) : COLOR0 {
	
	float2 uv = IN.UVCoord / scale;
	clip((uv <= 1) - 1);

	float sunset = pows(sunHeight, 8);
    float3 sunColor = linearize(TESR_SunColor).rgb + lerp(linearize(TESR_SunsetColor.rgb), 0, sunset); // linearise

	float glarePower = lerp(0.1, 8.0, sunset); // increase flare boost during sunrise/sunset

	float depth = (readDepth(uv) / farZ) > 0.9; //only pixels belonging to the sky will register
	float3 sunGlare = pows(dot(TESR_ViewSpaceLightDir.xyz, normalize(reconstructPosition(uv))), 180) * glarePower; // fake sunglare computed from light direction
	float3 color = linearize(tex2D(TESR_SourceBuffer, uv)).rgb;
	color = (color + sunGlare * sunColor) * depth * smoothstep(0, 0.01, sunHeight);

	return float4(color, 1.0f);
}


float4 LightMask(VSOUT IN) : COLOR0 {
	// isolates the brightest parts of the sky to only use those for radial blur
	
	float2 uv = IN.UVCoord;
	clip((uv <= scale) - 1);

	// quick average lum with 4 samples at corner pixels
	float3 color = tex2D(TESR_RenderedBuffer, uv).rgb;
	color += tex2D(TESR_RenderedBuffer, uv + float2(-1, -1) * TESR_ReciprocalResolution.xy).rgb;
	color += tex2D(TESR_RenderedBuffer, uv + float2(-1, 1) * TESR_ReciprocalResolution.xy).rgb;
	color += tex2D(TESR_RenderedBuffer, uv + float2(1, -1) * TESR_ReciprocalResolution.xy).rgb;
	color += tex2D(TESR_RenderedBuffer, uv + float2(1, 1) * TESR_ReciprocalResolution.xy).rgb;
	color /= 5;

	// extract bright pixels
	float treshold = lerp(2.0, 0.0, pow(abs(sunHeight), 8)); // scale the bloom power with sunsets/sunrises
	float bloom = smoothstep(treshold, treshold + lumTreshold * 15, luma(color));

	color = saturate(bloom * color * 100 * intensity);

	return float4(color.rgb, 1.0f);
}


float4 RadialBlur(VSOUT IN, uniform float step) : COLOR0 {
	float2 uv = IN.UVCoord;
	clip((uv <= scale) - 1);
	uv /= scale; // restore uv scale to do calculations in [0, 1] space
	uv -= 0.5 * TESR_ReciprocalResolution.xy;

	// calculate vector from pixel to sun along which we'll sample
	float2 sunPos = projectPosition(TESR_ViewSpaceLightDir.xyz * farZ).xy;

	// vector from the given pixel to the sun position
	float2 blurDirection = (sunPos.xy - uv) * float2(1.0f, raspect); // apply aspect ratio correction
	float distance = length(blurDirection); // distance from pixel to radial blur center

	float2 dir = blurDirection/distance;

	float stepSize = step * stepLengthMult;
	float maxStep = distance/stepSize;

	// sample the light clamped image from the pixel to the sun for the given amount of samples
	float2 samplePos = uv;
	float4 color = float4(0, 0, 0, 1);
	float total = 1;
	for (float i=0; i < samples; i++){
		float length = min(stepSize * i, distance); // clamp sampling vector to the distance from the pixel to the sun
		samplePos = saturate(uv + (dir * length / float2(1, raspect))); // apply aspect ratio correction

		float doStep = (i <= maxStep && samplePos.x > 0 && samplePos.y > 0 && samplePos.x < 1 && samplePos.y < 1); // check if we haven't overshot the sun position or exited the screen
		color += tex2D(TESR_RenderedBuffer, samplePos * scale) * doStep;
		total += doStep;
	}
	color /= total;

	return float4(color.rgb, 1);
}


float4 Combine(VSOUT IN) : COLOR0
{
	float scale = 0.5; // godrays were rendered at smaller res
	float4 color = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	float2 uv = IN.UVCoord;
	float3 eyeDir = normalize(reconstructPosition(uv));
	
	// calculate vector from pixel to sun to get the distance
	float2 sunPos = projectPosition(TESR_ViewSpaceLightDir.xyz * farZ).xy;
	float2 blurDirection = (sunPos.xy - uv) * float2(1.0f, raspect); // apply aspect ratio correction
	float distance = length(blurDirection);

	uv *= scale;
	float4 rays = tex2D(TESR_RenderedBuffer, uv);

	// attentuate intensity with distance from sun to fade the edges and reduce sunglare
	float heightAttenuation = TESR_GodRaysData.w?lerp(0.2, 4.0, pows(sunHeight, 4)):1.0; // if timeEnabled is on, godrays strength is reduced when the sun is high
	float glareAttenuation = 1.0;
	// float glareAttenuation = smoothstep(0, glareReduction, distance);
	float attenuation = pow(compress(shade(TESR_ViewSpaceLightDir.xyz, eyeDir)), 2.5) * glareAttenuation * heightAttenuation * (sunHeight < 1);

	// calculate sun color
    float3 sunColor = GetSunColor(shade(TESR_SunDirection.xyz, blue.xyz), 1, TESR_SunAmount.x, TESR_SunColor.rgb, TESR_SunsetColor.rgb);
    float3 godRayColor = linearize(TESR_GodRaysRayColor).rgb;

	//rays = pows(rays, godrayCurve); // increase response curve to extract more definition from godray pass
	rays.rgb *= multiplier * lerp(sunColor, godRayColor, TESR_GodRaysRayColor.w);
	rays.rgb *= attenuation;

	// reduce banding by dithering areas impacted by the rays
	//float maxDitherLuma = 0.05; // 0.2 ^ 2.2, rounded down
	//bool useDither = (rays.r + rays.g + rays.b > 0) && (pows(tex2D(TESR_AvgLumaBuffer, float2(0.5, 0.5)),2.2).x < maxDitherLuma); // only dither when there is some ray & when average luma is low
	//uv /= TESR_ReciprocalResolution.xy;
	//rays.rgb += (ditherMat[(uv.x)%4 ][ (uv.y)%4 ] / 255) * useDither;

	color += max(rays, 0) * 5 * color + max(rays, 0) * 0.2;
	color = delinearize(color);
	return float4(color.rgb, 1);
}


// ================= Enhanced: decayed-raymarch shaft accumulation =================
// Adapted from an older NVR-era GodRays shader (real Kenny Mitchell/GPU Gems 3 "Volumetric Light
// Scattering": per-step exponential illumination decay, not an averaged blur), reusing this file's
// existing sun-screen-projection idiom (TESR_ViewSpaceLightDir + projectPosition, same as Classic's
// RadialBlur/Combine above) instead of the old shader's own manual view/projection matrix math.

float4 RayMaskEnhanced(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord / scale;
	clip((uv <= 1) - 1);

	// Graduated by raw depth rather than a hard sky-only cutoff, so near-horizon terrain silhouettes
	// contribute proportionally to the seed instead of an all-or-nothing sky mask.
	float depth = readDepth(uv) / farZ;
	float3 color = linearize(tex2D(TESR_SourceBuffer, uv)).rgb;
	return float4(color * depth, 1.0f);
}

float4 LightShaftEnhanced(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	clip((uv <= scale) - 1);
	uv /= scale;

	float2 sunPos = projectPosition(TESR_ViewSpaceLightDir.xyz * farZ).xy;
	float2 blurDirection = (sunPos.xy - uv) * float2(1.0f, raspect);
	float distanceToSun = length(blurDirection);
	float2 dir = blurDirection / max(distanceToSun, 0.0001);

	float stepSize = min(0.3, distanceToSun) * RayStepScale / GodRaysPasses;
	float2 samplePos = uv;
	float3 color = tex2D(TESR_RenderedBuffer, uv * scale).rgb;
	float illuminationDecay = 1.0;

	[unroll(64)]
	for (int i = 0; i < GodRaysPasses; i++) {
		samplePos -= (dir * stepSize) / float2(1.0f, raspect); // undo aspect correction to step in real UV space
		float3 s = tex2D(TESR_RenderedBuffer, saturate(samplePos) * scale).rgb;
		color += s * illuminationDecay;
		illuminationDecay *= RayDecay;
	}
	color *= intensity / GodRaysPasses;

	return float4(color, 1.0f);
}

float4 BlurEnhanced(VSOUT IN) : COLOR0 {
	// Tangential (perpendicular-to-sun-direction) blur to hide the raymarch's step banding.
	float2 uv = IN.UVCoord;
	clip((uv <= scale) - 1);

	float2 sunPos = projectPosition(TESR_ViewSpaceLightDir.xyz * farZ).xy * scale;
	float2 tangent = normalize(uv - sunPos).yx * float2(TESR_ReciprocalResolution.y, -TESR_ReciprocalResolution.x) * BlurStrength;

	float4 col = tex2D(TESR_RenderedBuffer, uv);
	col += 0.67f * tex2D(TESR_RenderedBuffer, uv + tangent);
	col += 0.67f * tex2D(TESR_RenderedBuffer, uv - tangent);
	col += 0.33f * tex2D(TESR_RenderedBuffer, uv + 2.0f * tangent);
	col += 0.33f * tex2D(TESR_RenderedBuffer, uv - 2.0f * tangent);

	return float4(col.rgb * 0.333f, 1.0f);
}

float3 BlendSoftLight(float3 a, float3 b) {
	float3 c = 2.0f * a * b * (1.0f + a * (1.0f - b));
	float3 a_sqrt = sqrt(a);
	float3 d = (a + b * (a_sqrt - a)) * 2.0f - a_sqrt;
	return (b < 0.5f) ? c : d;
}

float4 CombineEnhanced(VSOUT IN) : COLOR0 {
	float4 ori = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	float2 uv = IN.UVCoord * scale;
	float4 rays = tex2D(TESR_RenderedBuffer, uv);

	float3 eyeDir = normalize(reconstructPosition(IN.UVCoord));
	float heightAttenuation = TESR_GodRaysData.w ? lerp(0.2, 4.0, pows(sunHeight, 4)) : 1.0;
	float attenuation = pow(compress(shade(TESR_ViewSpaceLightDir.xyz, eyeDir)), 2.5) * heightAttenuation * (sunHeight < 1);

	float3 sunColor = GetSunColor(shade(TESR_SunDirection.xyz, blue.xyz), 1, TESR_SunAmount.x, TESR_SunColor.rgb, TESR_SunsetColor.rgb);
	float3 godRayColor = linearize(TESR_GodRaysRayColor).rgb;
	float3 rayTint = lerp(sunColor, godRayColor, TESR_GodRaysRayColor.w);

	// Darkness-weighted: rays read weaker over already-bright pixels, stronger over dark/shadowed
	// ones, instead of a flat additive boost -- avoids blowing out highlights the way Classic's
	// `color += rays * 5 * color + rays * 0.2` can. Soft-light composite instead of a plain add.
	rays.rgb *= multiplier * rayTint * attenuation * saturate(1.0 - ori.rgb);

	float4 color = ori + rays;
	color.rgb = BlendSoftLight(color.rgb, rayTint * multiplier + 0.5f);
	color.rgb = delinearize(color.rgb);
	return float4(color.rgb, 1.0f);
}


// ================= GlareOnly: sun corona/glare, no streak passes =================
// Extracted from Classic's SkyMask so it can render on its own, at full resolution, with none of
// the multi-pass streak machinery -- used when the Volumetric (fog-integrated) light-shafts tier
// is the active choice elsewhere, so the corona doesn't disappear just because streak duty moved
// to the fog shader. Enhanced doesn't need this pass itself: unlike Classic, it has no separate
// synthetic glare term -- its glow comes from the raymarch of the sun disc's own rendered brightness.

float4 Glare(VSOUT IN) : COLOR0 {
	float4 ori = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	if (sunHeight >= 1 || GlareStrength <= 0) return float4(delinearize(ori.rgb), 1.0f);

	float sunset = pows(sunHeight, 8);
	float3 sunColor = linearize(TESR_SunColor).rgb + lerp(linearize(TESR_SunsetColor.rgb), 0, sunset);
	float glarePower = lerp(0.1, 8.0, sunset);

	float3 eyeDir = normalize(reconstructPosition(IN.UVCoord));
	float3 glare = pows(dot(TESR_ViewSpaceLightDir.xyz, eyeDir), 180) * glarePower;
	glare *= smoothstep(0, 0.01, sunHeight) * GlareStrength;

	float3 color = ori.rgb + glare * sunColor * multiplier;
	return float4(delinearize(color), 1.0f);
}


technique Classic
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SkyMask();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 LightMask();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 RadialBlur(stepLength);
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 RadialBlur(stepLength * stepLength);
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 RadialBlur(stepLength * stepLength * stepLength);
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		Pixelshader = compile ps_3_0 Combine();
	}
}

technique Enhanced
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 RayMaskEnhanced();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 LightShaftEnhanced();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurEnhanced();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombineEnhanced();
	}
}

technique GlareOnly
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Glare();
	}
}