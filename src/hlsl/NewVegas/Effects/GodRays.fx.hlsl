// GodRays full screen shader for Oblivion/Skyrim Reloaded

float4 TESR_ReciprocalResolution;
float4 TESR_GameTime;
float4 TESR_SunColor;
float4 TESR_GodRaysRay; // x: intensity, y:length, z: density, w: visibility
float4 TESR_GodRaysRayColor; // x:r, y:g, z:b, w:saturate
float4 TESR_GodRaysData; // x: passes amount, y: luminance, z:multiplier, w: time enabled
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
static const float2 sunPos = projectPosition(TESR_ViewSpaceLightDir.xyz * farZ).xy;
static const float heightAttenuation = TESR_GodRaysData.w ? lerp(0.2, 4.0, pows(sunHeight, 4)) : 1.0;

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

	float depth = tex2D(TESR_DepthBuffer, uv).x > 0.9; //only pixels belonging to the sky will register
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

	// vector from the given pixel to the sun position
	float2 blurDirection = (sunPos - uv) * float2(1.0f, raspect); // apply aspect ratio correction
	float distance = length(blurDirection); // distance from pixel to radial blur center

	float2 dir = blurDirection/distance;

	float stepSize = step * stepLengthMult;
	float maxStep = distance/stepSize;

	// sample the light clamped image from the pixel to the sun for the given amount of samples
	float2 samplePos = uv;
	float4 color = float4(0, 0, 0, 1);
	float total = 1;
	for (int i = 0; i < samples; i++){
		if (i > maxStep) break;
		float stepDist = stepSize * i;
		samplePos = saturate(uv + (dir * stepDist / float2(1, raspect))); // apply aspect ratio correction
		color += tex2D(TESR_RenderedBuffer, samplePos * scale);
		total += 1;
	}
	color /= total;

	return float4(color.rgb, 1);
}


float4 Combine(VSOUT IN) : COLOR0
{
	float4 color = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	float2 uv = IN.UVCoord;
	float3 eyeDir = normalize(reconstructPosition(uv));

	uv *= scale;
	float4 rays = tex2D(TESR_RenderedBuffer, uv);

	float attenuation = pow(compress(shade(TESR_ViewSpaceLightDir.xyz, eyeDir)), 2.5) * heightAttenuation * (sunHeight < 1);

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

	float4 raysPos = max(rays, 0);
	color += raysPos * 5 * color + raysPos * 0.2;
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