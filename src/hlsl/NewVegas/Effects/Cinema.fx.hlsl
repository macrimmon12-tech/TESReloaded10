// Cinema Shader For TESReloaded
//--------------------------------------------------
// Boomstick was h3r3

string PipelinePosition = "PostTonemapping";

float4 TESR_GameTime
<
	string widget = "hidden";
	string name = "Game Time";
	string description = "Per-frame game clock supplied by the engine: x = time in milliseconds, y = time in hours (0-24), z = frame time counter (drives the film-grain/noise animation), w = elapsed time in seconds since last frame. Not user-configurable.";
	float defaultValue = 0.0;
>;
float4 TESR_ReciprocalResolution
<
	string widget = "hidden";
	string name = "Reciprocal Resolution";
	string description = "Per-frame render target metrics supplied by the engine: x = 1/width, y = 1/height, z = aspect ratio (width/height), w = reserved for FoV. Not user-configurable.";
	float defaultValue = 0.0;
>;
// x (AspectRatio) is deliberately left out of componentKeys below: its live
// value is computed each frame from the setting plus Mode/dialog state, not
// a direct passthrough, so exposing it here would show a real editable
// range where the value on screen isn't a simple reflection of it.
float4 TESR_CinemaData
<
	string widget = "packed";
	string name = "Cinema Data";
	string description = "Packed letterbox/vignette parameters: x = AspectRatio (computed each frame from Shaders.Cinema.Main.AspectRatio and Mode), y = VignetteRadius, z = VignetteDarkness, w = overlayStrength.";
	string componentKeys     = "VignetteRadius,VignetteDarkness,OverlayStrength";
	string componentDefaults = "0.6,1.2,0.1";
	string componentMins     = "0,0,0";
	string componentMaxs     = "1,3,1";
	string componentSteps    = "0.01,0.01,0.01";
	float defaultValue = 1.0;
>;
// x (dirt lens opacity) is declared but not currently written by
// CinemaEffect::UpdateSettings() -- left as-is (metadata-only pass, no
// behavior change); y/z/w are, and are the only ones in componentKeys below.
float4 TESR_CinemaSettings
<
	string widget = "packed";
	string name = "Cinema Settings";
	string description = "Packed film-grain/aberration/letterbox parameters (Shaders.Cinema.Main): x = dirt lens opacity (unused, not currently wired to a setting), y = FilmGrainAmount, z = ChromaticAberration, w = LetterBoxDepth.";
	string componentKeys     = "FilmGrainAmount,ChromaticAberration,LetterBoxDepth";
	string componentDefaults = "0.3,1.0,0.0";
	string componentMins     = "0,0,0";
	string componentMaxs     = "1,3,1";
	string componentSteps    = "0.01,0.01,0.01";
	float defaultValue = 0.3;
>;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BlueNoiseSampler : register(s2) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };

static const float time = TESR_GameTime.z * 25; // Simulate cinema noise by using cinema framerate

// Letter box parameters
static const float aspectRatio = TESR_CinemaData.x; // Ratio of the visible image (Width/Height)

// Vignette parameters  
static const float softness = TESR_CinemaData.y; // Softness of the vignette transition
static const float intensity = TESR_CinemaData.z; // Intensity of the vignette effect

// Photoshop overlay parameters  
static const float overlayStrength = TESR_CinemaData.w; // Intensity of the overlay amount

// Dirt lens parameters
static const float dirtAmount = max(0, TESR_CinemaSettings.x); // The overall intensity of the dirt texture

// Film grain parameters
static const float grainAmount = TESR_CinemaSettings.y * 0.01; // Controls the amount of grain to add (scaled for more subtle control)

// Chromatic aberration parameters
static const float chromaStrength = TESR_CinemaSettings.z; // Controls the amount of grain to add


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

#include "Includes/Helpers.hlsl"
#include "Includes/Blending.hlsl"
#include "Includes/Depth.hlsl"

float3 random(float2 seed)
{
	return tex2D(TESR_BlueNoiseSampler, (seed/256 + 0.5) / TESR_ReciprocalResolution.xy).xyz;
}


float4 Cinema(VSOUT IN) : COLOR0 
{
    // Sample the input texture at the current texCoord
	float2 uv = IN.UVCoord;

	//Letter box 
	//--------------------------------------------------
    // Height as a ratio between wanted letterbox aspect ratio and actual aspect ratio
	// cancel out if aspect ratio is set to 0 for some reason, to avoid division by 0
	float letterboxHeight = lerp(TESR_ReciprocalResolution.z, (1 - TESR_ReciprocalResolution.z / aspectRatio) / 2, aspectRatio != 0);
	float depth = readDepth(IN.UVCoord);

    // Check if the current pixel is within the letterbox region
    if ((uv.y < letterboxHeight || uv.y > 1 - letterboxHeight) && (depth > TESR_CinemaSettings.w))
        return float4(0, 0, 0, 1); // Early out to return black if in letterbox area;

	// Chromatic aberration
	//--------------------------------------------------
	float2 chromaShift = TESR_ReciprocalResolution.xy * chromaStrength;
	float2 posToCenter = float2(0, 0) - expand(IN.UVCoord);

	// shift each channel with an offset based on vector to center to simulate lens distortion
    float4 color = tex2D(TESR_RenderedBuffer, IN.UVCoord);
    color.r = tex2D(TESR_RenderedBuffer, IN.UVCoord + posToCenter * chromaShift).r;
    color.b = tex2D(TESR_RenderedBuffer, IN.UVCoord - posToCenter * chromaShift).b;

	//Film grain 
	//--------------------------------------------------
    // Add the grain by multiplying the color by a value between 1 and 1 + grainAmount while preserving brightness
	float3 noise1 = random(uv + time * TESR_ReciprocalResolution.xy);
	float3 noise2 = random(uv - time * TESR_ReciprocalResolution.xy);
    color = BlendMode_Overlay(0.5 + grainAmount * (expand(noise1.r * noise2.b * 2)), color);

	//Photoshop overlay
	//--------------------------------------------------
    // Multiply the input color by the overlay color, then blend with the original input color
    color = lerp(color, BlendMode_Overlay(color, color), overlayStrength);
    
	//Vignette
	//--------------------------------------------------
    float dist = length(posToCenter)/2; // Calculate the distance from the center of the vignette effect
    float vignette = smoothstep(1, 1 - softness, dist);  // Calculate the vignette intensity based on the distance
		
    color.rgb *= vignette * intensity + (1 - intensity); // Darken vignette zone

    return float4(color.rgb, 1);
}


technique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 Cinema();
    }
}