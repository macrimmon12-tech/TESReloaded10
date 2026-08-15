// Image space shadows shader for Oblivion Reloaded
string PipelinePosition = "PreTonemapping";

# define viewshadows 0

float4 TESR_ReciprocalResolution
<
	string name = "Reciprocal Resolution";
	string description = "Per-frame render target metrics supplied by the engine: x = 1/width, y = 1/height, z = aspect ratio (width/height), w = reserved for FoV. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_WaterSettings
<
	string name = "Water Settings";
	string description = "WaterShaders' own registered constant (ShaderCollection, no annotatable constant table): x = water height in the cell, y = depthDarkness (Shaders.Water.Default.depthDarkness), z = isUnderwater, w = refractionPower.";
	float defaultValue = 0.0;
>;
// Comment previously read "z: nearmap resolution, w: farmap resolution" -- not
// actually set by ShadowsExteriorEffect::UpdateConstants() in the exterior
// branch (only x/y are); left undocumented rather than repeating a stale claim.
float4 TESR_ShadowData
<
	string name = "Shadow Data";
	string description = "This effect's own registered constant: x = Quality (Shaders.ShadowsExteriors.Main.Quality), y = Darkness (Shaders.ShadowsExteriors.Main.Darkness).";
	float defaultValue = 0.75;
>;
float4 TESR_ShadowFade
<
	string name = "Shadow Fade";
	string description = "This effect's own registered constant: x = sunset/sunrise (and moon phase) shadow attenuation, y = shadow maps enabled (Shaders.ShadowsExteriors.Main.Enabled), z = point light shadows enabled (UsePointShadowsDay/Night), w = point light shadow draw distance.";
	float defaultValue = 0.0;
>;
float4 TESR_SkyColor
<
	string name = "Sky Color";
	string description = "Top-of-sky color, supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunAmbient
<
	string name = "Sun Ambient";
	string description = "Current ambient sky light color (RGB), supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_SunColor
<
	string name = "Sun Color";
	string description = "Current directional sunlight color (RGB), supplied by the engine from the active weather. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_ShadowScreenSpaceData
<
	string name = "Shadow Screen Space Data";
	string description = "This effect's own registered constant (Shaders.ShadowsExteriors.ScreenSpace): x = Enabled, y = BlurRadius, z = RenderDistance, w = Intensity.";
	float defaultValue = 1.0;
>;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = ANISOTROPIC; MIPFILTER = LINEAR; };
sampler2D TESR_PointShadowBuffer : register(s2)  = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };


static const float DARKNESS = max(0.0,1-TESR_ShadowData.y);

struct VSOUT
{
	float4 vertPos : POSITION;
	float4 normal : TEXCOORD1;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/Shadows.hlsl"


VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

/*
 * Load Shadows Buffer and filter water surfaces 
 * returns a shadow value from darkness setting value (full shadow) to 1 (full light)
*/
float4 Shadow(VSOUT IN) : COLOR0
{
	float4 color = tex2D(TESR_RenderedBuffer, IN.UVCoord);
	float2 uv = IN.UVCoord;

	float depth = readDepth(uv);
	float3 camera_vector = toWorld(uv) * depth;
	float uniformDepth = length(camera_vector);
	float4 world_pos = float4(TESR_CameraPosition.xyz + camera_vector, 1.0f);
	float3 world_normal = GetWorldNormal(IN.UVCoord);

	// early out for underwater surface (if camera is underwater and surface to shade is close to water level with normal pointing downward)
	if (TESR_WaterSettings.z == 1 && world_pos.z < (TESR_WaterSettings.x + 2) && world_pos.z > (TESR_WaterSettings.x - 2) && dot(world_normal, float3(0, 0, -1)) > 0.999) return color;

	float2 Shadow = tex2D(TESR_PointShadowBuffer, IN.UVCoord).rg;
	Shadow.r = lerp(TESR_ShadowFade.x, 1.0f, Shadow.r); // fade shadows to light when sun is low

	// scale shadows strength to ambient before adding attenuation for pointlights (ShadowFade.z means point Lights are on)
	float ambient = lerp(1, luma(TESR_SunAmbient), DARKNESS * TESR_ShadowFade.z); // linearise
	Shadow.r = lerp(0, ambient, Shadow.r); //scale brightest areas to the ambient so it can be lit further with attenuation
	Shadow.r += Shadow.g; // Apply poing light attenuation (includes point light shadows)

	Shadow.r = lerp(DARKNESS, 1.0, Shadow.r); 	// brighten shadow value from 0 to darkness from config value

	Shadow.r = saturate(Shadow.r);

#if viewshadows == 1
	return Shadow;
#endif
    color.rgb = pows(color.rgb, 2.2); // linearise
    float4 skyColor = float4(pows(TESR_SkyColor.rgb, 2.2),TESR_SkyColor.w); // linearise
	// tint shadowed areas with Sky color before blending
	float4 colorShadow = luma(color.rgb) * Shadow.r * skyColor;
	colorShadow.rgb = lerp(colorShadow, color * Shadow.r, saturate(Shadow.r + 0.5)).rgb;// bias the transition between the 2 colors to make it less noticeable
    colorShadow.rgb = pows(max(0.0,colorShadow.rgb), 1.0/2.2); // delinearise
	return float4(colorShadow.rgb, 1.0); 
}


technique {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow();
	}
}
 