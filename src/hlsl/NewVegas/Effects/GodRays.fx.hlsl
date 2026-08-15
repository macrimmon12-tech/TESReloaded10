// GodRays full screen shader for Oblivion/Skyrim Reloaded

string PipelinePosition = "PreTonemapping";

float4 TESR_ReciprocalResolution
<
	string widget = "hidden";
	string name = "Reciprocal Resolution";
	string description = "Per-frame render target metrics supplied by the engine: x = 1/width, y = 1/height, z = aspect ratio (width/height), w = reserved for FoV. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_GameTime
<
	string widget = "hidden";
	string name = "Game Time";
	string description = "Per-frame game clock supplied by the engine: x = time in milliseconds, y = time in hours (0-24), z = frame time counter, w = elapsed time in seconds since last frame. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunColor
<
	string widget = "hidden";
	string name = "Sun Color";
	string description = "Current directional sunlight color (RGB), supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_GodRaysRay
<
	string widget = "packed";
	string name = "God Rays Ray";
	string description = "Packed ray-marching parameters (Shaders.GodRays.Main): x = RayIntensity, y = RayLength, z = RayDensity, w = RayVisibility scaled by SunGlareEnabled and the engine's current sun glare value.";
	string componentKeys     = "RayIntensity,RayLength,RayDensity,RayVisibility";
	string componentDefaults = "1.0,1.0,1.0,50.0";
	string componentMins     = "0,0,0,0";
	string componentMaxs     = "3,3,3,100";
	string componentSteps    = "0.01,0.01,0.01,0.1";
	float defaultValue = 1.0;
>;
float4 TESR_GodRaysRayColor
<
	string name = "God Rays Color";
	string description = "Custom ray tint color (Shaders.GodRays.Coloring.RayR/G/B) and its blend weight (w = Saturate: 0 = use the sky/sun color, 1 = use only this custom color).";
	string widget = "color";
	float3 defaultValue = {1.0, 1.0, 1.0};
>;
// x (LightShaftPasses) and z (the day/night multiplier, a blend of two
// different keys -- DayMultiplier and NightMultiplier -- rather than a
// direct reflection of either one) are deliberately left out of
// componentKeys below.
float4 TESR_GodRaysData
<
	string widget = "packed";
	string name = "God Rays Data";
	string description = "Packed god-rays parameters (Shaders.GodRays.Main): x = LightShaftPasses (currently unused), y = Luminance (minimum ray-casting luminosity threshold), z = day/night strength multiplier (DayMultiplier/NightMultiplier blended by the day/night transition curve), w = TimeEnabled (1 = strongest at sunset/sunrise).";
	string componentKeys     = "Luminance,TimeEnabled";
	string componentNames    = "Luminance,Time Enabled";
	string componentDefaults = "0.8,0";
	string componentMins     = "0,0";
	string componentMaxs     = "2,1";
	string componentSteps    = "0.01,1";
	float defaultValue = 1.0;
>;
float4 TESR_ViewSpaceLightDir
<
	string widget = "hidden";
	string name = "View Space Light Direction";
	string description = "Sun/moon light direction transformed into view space, supplied by the engine for screen-space ray marching. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunDirection
<
	string widget = "hidden";
	string name = "Sun Direction";
	string description = "World-space direction vector to the sun/moon light source, normalized, supplied by the engine. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunPosition
<
	string widget = "hidden";
	string name = "Sun Position";
	string description = "World-space position of the sun disk, normalized direction with w = 1, supplied by the engine. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_ShadowFade
<
	string widget = "hidden";
	string name = "Shadow Fade";
	string description = "ShadowsExteriorEffect's own registered constant (see ShadowsExteriors.fx.hlsl): x = sunset/sunrise (and moon phase) shadow attenuation, y = shadow maps enabled, z = point light shadows enabled, w = point light shadow draw distance. Read here for x, the sunset/sunrise attenuation factor.";
	float defaultValue = 0.0;
>;
float4 TESR_SunAmount
<
	string widget = "hidden";
	string name = "Sun Amount";
	string description = "Day/night blend amount supplied by the engine, used to fade effects across sunrise/sunset. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunsetColor
<
	string widget = "hidden";
	string name = "Sunset Color";
	string description = "Color boost applied to the sun near the horizon, supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_DebugVar
<
	string widget = "hidden";
	string name = "Debug Variable";
	string description = "Developer scratch variable (Main.Develop.Main.DebugVar1-4). Not intended for normal use.";
	float defaultValue = 0.0;
>;

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
 
technique
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