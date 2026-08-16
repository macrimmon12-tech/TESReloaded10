// In-air light shaft for the flashlight cone: a single scattering march along the view
// ray, rendered at half resolution into TESR_VolumetricBuffer. The Flashlight effect's
// Combine pass adds it to the scene, so the shaft is tonemapped and bloomed with
// everything else rather than pasted onto a finished frame.
//
// The march covers only the part of the view ray that is actually inside the cone. A
// march over the whole ray spends most of its steps in unlit air, and averaging those in
// buries the few samples that hit the beam.
//
// This is a first pass at occlusion: the march is clamped to scene depth, but there is no
// shadow map, so the shaft in the air is not occluded by geometry between the light and
// the sample. Adapted from Better Flashlight NVSE, which is itself a fork of this effect.

float4 TESR_SpotLightPosition;
float4 TESR_SpotLightColor;
float4 TESR_SpotLightDirection;
float4 TESR_SunColor;
float4 TESR_ReciprocalResolution;
float4 TESR_VolumetricControl;		// x strength, y anisotropy, z height falloff, w max distance
float4 TESR_VolumetricData;			// xyz march light position, w first person strength floor
float4x4 TESR_FlashLightViewProjTransform;

sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_SpotLightTexture : register(s1) < string ResourceName = "Effects\Flashlight.png"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Depth.hlsl"
#include "Includes/Helpers.hlsl"

#define VOL_STEPS 24

// Converts the scatter integral to something in the same range as the surface pool, so
// the Strength setting reads as a multiplier around 1 rather than an arbitrary number
static const float VOL_CALIBRATION = 0.0008;
// Distance over which a sample fades out as it approaches the surface the ray lands on
static const float VOL_SURFACE_FADE = 256.0;


struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN  { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN IN) {
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float4 ScreenCoordToTexCoord(float4 coord) {
	coord.xyz /= coord.w;
	coord.x = coord.x *  0.5f + 0.5f;
	coord.y = coord.y * -0.5f + 0.5f;
	return coord;
}

// Henyey-Greenstein phase function, forward scattering for g > 0
float HenyeyGreenstein(float cosTheta, float g) {
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1e-4, 1.0 + g2 - 2.0 * g * cosTheta), 1.5));
}

// Interleaved gradient noise, used to offset the first step so the fixed step count does
// not show up as concentric bands. The half resolution upsample resolves the noise.
float ign(float2 px) {
	return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
}

// [t0, t1] bracket of the view ray inside the cone volume: hits on the forward nappe of
// the cone within the light range, hits on the range sphere that are inside the cone, and
// the endpoints when the ray starts or ends inside. co is the ray origin relative to the
// apex. A slightly loose bracket is harmless - every sample still runs the exact cone and
// attenuation math, the bracket only keeps steps from being spent on empty air.
bool ConeSegment(float3 co, float3 rd, float3 axis, float cosA, float range, float tMax, out float t0, out float t1) {
	float cos2 = cosA * cosA;
	float da   = dot(rd, axis);
	float ca   = dot(co, axis);
	float dco  = dot(rd, co);
	float coco = dot(co, co);

	t0 = tMax; t1 = 0.0;
	float t, h;
	float3 p;

	float A = da * da - cos2;
	float B = 2.0 * (da * ca - dco * cos2);
	float C = ca * ca - coco * cos2;
	float disc = B * B - 4.0 * A * C;
	if (abs(A) > 1e-6 && disc > 0.0) {
		float sq = sqrt(disc);
		t = (-B - sq) / (2.0 * A); h = ca + t * da;
		if (t > 0.0 && t < tMax && h > 0.0 && h < range) { t0 = min(t0, t); t1 = max(t1, t); }
		t = (-B + sq) / (2.0 * A); h = ca + t * da;
		if (t > 0.0 && t < tMax && h > 0.0 && h < range) { t0 = min(t0, t); t1 = max(t1, t); }
	}

	float ds = dco * dco - (coco - range * range);
	if (ds > 0.0) {
		float sq = sqrt(ds);
		t = -dco - sq; p = co + t * rd; h = dot(p, axis);
		if (t > 0.0 && t < tMax && h > 0.0 && h * h > cos2 * dot(p, p)) { t0 = min(t0, t); t1 = max(t1, t); }
		t = -dco + sq; p = co + t * rd; h = dot(p, axis);
		if (t > 0.0 && t < tMax && h > 0.0 && h * h > cos2 * dot(p, p)) { t0 = min(t0, t); t1 = max(t1, t); }
	}

	if (ca > 0.0 && ca < range && ca * ca > cos2 * coco) t0 = 0.0;
	p = co + tMax * rd; h = dot(p, axis);
	if (h > 0.0 && dot(p, p) < range * range && h * h > cos2 * dot(p, p)) t1 = tMax;

	return t1 > t0;
}

float4 Volumetric(VSOUT IN) : COLOR0 {
	float strength = TESR_VolumetricControl.x;
	if (strength <= 0.0) return float4(0, 0, 0, 1);

	float g          = TESR_VolumetricControl.y;
	float heightFall = TESR_VolumetricControl.z;
	float maxDist    = TESR_VolumetricControl.w;

	float3 rayVec = toWorld(IN.UVCoord);
	float3 dir    = normalize(rayVec);
	float3 camPos = TESR_CameraPosition.xyz;

	float3 lightPos = TESR_VolumetricData.xyz;
	float fpBlend   = saturate(TESR_VolumetricData.w);
	float3 lightDir = TESR_SpotLightDirection.xyz;
	float radius    = TESR_SpotLightPosition.w;
	float angleCosMax = cos(radians(TESR_SpotLightDirection.w));
	float angleCosMin = cos(radians(TESR_SpotLightDirection.w * 0.5));
	float groundZ   = camPos.z - 128.0;		// roughly the player's feet; dust settles low

	float sceneDist = length(rayVec) * readDepth(IN.UVCoord);
	float tMax = min(sceneDist, maxDist);

	float tEnter, tExit;
	if (!ConeSegment(camPos - lightPos, dir, lightDir, max(angleCosMax, 0.1), radius, tMax, tEnter, tExit))
		return float4(0, 0, 0, 1);

	float stepLen = (tExit - tEnter) / VOL_STEPS;
	float t = tEnter + ign(IN.UVCoord / TESR_ReciprocalResolution.xy) * stepLen;

	float3 scatter = 0;
	float  transmittance = 1.0;
	[loop]
	for (int i = 0; i < VOL_STEPS; i++) {
		float3 p = camPos + dir * t;
		float3 toL = lightPos - p;
		float  distToL = length(toL);
		float3 Ldir = toL / max(distToL, 0.001);

		float cone = pow(invlerps(angleCosMax, angleCosMin, shades(lightDir, -Ldir)), 2.0);
		float s = saturate((distToL / radius) * (distToL / radius));
		float atten = saturate(((1 - s) * (1 - s)) / (1 + 5.0 * s));

		float4 lsc = ScreenCoordToTexCoord(mul(float4(p, 1), TESR_FlashLightViewProjTransform));
		float2 cookieUV = lsc.xy;
		// The cookie projects from the true light position, but in first person the march
		// runs from a virtual offset one, so blend it flat to hide the parallax mismatch
		float cookie = lerp(tex2D(TESR_SpotLightTexture, cookieUV).r, 1.0, fpBlend);

		float density = exp(-max(0.0, p.z - groundZ) * heightFall * 0.0005);
		float phase   = HenyeyGreenstein(dot(dir, -Ldir), g);

		float surfaceFade = smoothstep(0.0, VOL_SURFACE_FADE, sceneDist - t);
		float sigma = density * cone * atten * cookie * surfaceFade;
		scatter += transmittance * sigma * phase * stepLen;
		transmittance *= exp(-density * 0.5 * stepLen * 0.001);
		t += stepLen;
	}

	// Same energy model as the surface pool - colour, dimmer and the night boost - so the
	// shaft keeps a constant ratio to the pool as auto exposure moves through day and night
	float sunLuma = 1.0 / max(0.05, luma(TESR_SunColor));
	float3 lightColor = TESR_SpotLightColor.rgb * TESR_SpotLightColor.w * sunLuma;

	// Fade with the light to camera distance so the in-scatter cannot pile onto the surface
	// pool when the source sits on the eye. In first person the virtual offset supplies the
	// parallax and w floors this fade at the configured first person strength.
	float camFade = saturate((length(lightPos - camPos) - 40.0) / 60.0);
	camFade = max(camFade, fpBlend);

	float3 inScatter = scatter * (strength * VOL_CALIBRATION * camFade) * lightColor;
	return float4(inScatter, transmittance);
}

technique {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 Volumetric();
	}
}
