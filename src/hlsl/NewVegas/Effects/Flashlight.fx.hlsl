float4x4 TESR_FlashLightViewProjTransform;
float4 TESR_CameraForward;
float4 TESR_SpotLightPosition;
float4 TESR_SpotLightColor;
float4 TESR_SpotLightDirection;
float4 TESR_SunColor;
float4 TESR_SunDirection;
float4 TESR_DebugVar;
float4 TESR_ReciprocalResolution;
float4 TESR_FlashLightTuning;	// x spot size, y intensity, z near fade, w soft edges
float4 TESR_FlashLightHotspot;	// x hotspot limit, y cookie strength
float4 TESR_VolumetricControl;	// x beam strength, 0 when the FlashlightBeam effect is off

#define FL_SIZE				TESR_FlashLightTuning.x
#define FL_INTENSITY		TESR_FlashLightTuning.y
#define FL_NEARFADE			TESR_FlashLightTuning.z
#define FL_SOFTEDGE			TESR_FlashLightTuning.w
#define FL_HOTSPOTLIMIT		TESR_FlashLightHotspot.x
#define FL_COOKIESTRENGTH	TESR_FlashLightHotspot.y
#define FL_LINEARSOURCE		TESR_FlashLightHotspot.z

sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_RenderedBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = ANISOTROPIC; MIPFILTER = LINEAR; };
sampler2D TESR_PointShadowBuffer : register(s3)  = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowSpotlightBuffer0 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SpotLightTexture : register(s6) < string ResourceName = "Effects\Flashlight.png"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_VolumetricBuffer : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };


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
#include "Includes/Helpers.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/BlurDepth.hlsl"

float4 displayBuffer(float4 color, float2 uv, float2 bufferPosition, float2 bufferSize, sampler2D buffer){
	float2 lowerCorner = bufferPosition + bufferSize;
	if ((uv.x < bufferPosition.x || uv.y < bufferPosition.y) || (uv.x > lowerCorner.x || uv.y > lowerCorner.y )) return color;
	return tex2D(buffer, float2(invlerp(bufferPosition, lowerCorner, uv)))/TESR_DebugVar.y;
}


float4 ScreenCoordToTexCoord(float4 coord){
	// apply perspective (perspective division) and convert from -1/1 to range to 0/1 (shadowMap range);
	coord.xyz /= coord.w;
	coord.x = coord.x * 0.5f + 0.5f;
	coord.y = coord.y * -0.5f + 0.5f;

	return coord;
}


float4 Shadows(VSOUT IN) : COLOR0
{
	float depth = readDepth(IN.UVCoord);
    float3 eyeVector = toWorld(IN.UVCoord);
    float4 worldPos = float4(TESR_CameraPosition.xyz + eyeVector * depth, 1);
    float3 lightToWorld = TESR_SpotLightPosition.xyz - worldPos.xyz;

    float radius = TESR_SpotLightPosition.w;
    float Distance = length(lightToWorld)/radius;

	float4 lightSpaceCoord = ScreenCoordToTexCoord(mul(worldPos, TESR_FlashLightViewProjTransform));
	float shadowDepth =	tex2D(TESR_ShadowSpotlightBuffer0, lightSpaceCoord.xy).r;
	float isShadow = shadowDepth + 0.05 * Distance > Distance; // BIAS to reduce shadowing artefacts

	if (Distance < 1 && lightSpaceCoord.x > 0.0 && lightSpaceCoord.x < 1.0 && lightSpaceCoord.y > 0.0 && lightSpaceCoord.y < 1.0) return float4(isShadow.rrr, 1);
	return float4(1, 1, 1, 1);
}

float4 NoShadow(VSOUT IN) : COLOR0
{
	return float4(1, 1, 1, 1);
}

// Procedural rings and mottling layered over the projected cookie, so a bare wall
// shows some structure in the beam instead of a flat disc. Fades to 1 at strength 0.
float BeamBreakup(float2 uv)
{
	if (FL_COOKIESTRENGTH <= 0.001) return 1.0;

	float2 p = uv - 0.5;
	float r = saturate(length(p) * 2.0);

	float warp = sin(uv.x * 17.0 + uv.y * 5.5) * 0.45 + sin(uv.x * -6.5 + uv.y * 19.0) * 0.35;
	float rings = sin(r * 36.0 + warp) * 0.5 + 0.5;
	float mottled = sin(uv.x * 41.0 + sin(uv.y * 13.0) * 2.2) * sin(uv.y * 29.0 + sin(uv.x * 11.0) * 1.7);
	mottled = mottled * 0.5 + 0.5;

	float edge = smoothstep(0.25, 0.95, r);
	float breakup = 1.0 + (rings - 0.5) * (0.10 + 0.14 * edge) + (mottled - 0.5) * 0.12;
	breakup -= edge * (0.04 + 0.04 * mottled);
	return lerp(1.0, saturate(breakup), saturate(FL_COOKIESTRENGTH));
}

float4 Flashlight(VSOUT IN) : COLOR0
{
	float depth = readDepth(IN.UVCoord);
    float3 eyeVector = toWorld(IN.UVCoord);
	float3 eyeDirection = normalize(eyeVector);
    float3 worldPos = TESR_CameraPosition.xyz + eyeVector * depth;
	float3 eyePos = TESR_CameraPosition.xyz;
	float fogDepth = length(eyeVector * depth); // a depth measure that's at the same distance from the camera at all angles
	float3 normal = GetWorldNormal(IN.UVCoord);

    float3 lightpos = TESR_SpotLightPosition.xyz;
    float3 lightDir = TESR_SpotLightDirection.xyz;
    float3 lightToWorld = lightpos - worldPos;
    float3 lightVector = normalize(lightToWorld);

	// Soft edges use wrap lighting instead of a clamped N.L. The normals here are
	// reconstructed from depth, which is near-random across alpha tested foliage, and a
	// clamped N.L turns that into hard speckle with a lot of exact zeros. Wrapping halves
	// the spread and removes the zeros, at the cost of a flatter looking beam.
    float ndl = dot(lightVector, normal);
    float diffuse = (FL_SOFTEDGE > 0.5) ? saturate(ndl * 0.5 + 0.5) : max(ndl, 0.0);
    float specular = pows(shades(normalize(eyeDirection + lightVector * -1), normal), 5);

	// radius based attenuation based on https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html
    float radius = TESR_SpotLightPosition.w;
    float Distance = length(lightToWorld)/radius;
	float s = saturate(Distance * Distance); 
	float atten = saturate(((1 - s) * (1 - s)) / (1 + 5.0 * s));

	float4 lightSpaceCoord = ScreenCoordToTexCoord(mul(float4(worldPos, 1), TESR_FlashLightViewProjTransform));
	float isShadow = tex2D(TESR_RenderedBuffer, IN.UVCoord);

	// Zoom the cookie about its centre so its dark edge stays on the cone cutoff as the
	// spot size changes - the two have to move together or the beam grows a hard rim.
	float2 cookieUV = (lightSpaceCoord.xy - 0.5) / max(0.05, FL_SIZE) + 0.5;
	float lightTexture = tex2D(TESR_SpotLightTexture, cookieUV).r * BeamBreakup(cookieUV);

	float sunLuma = 1 / max(0.05, luma(TESR_SunColor));
    float3 lightColor = TESR_SpotLightColor.rgb * TESR_SpotLightColor.w * sunLuma;

	float angleCosMax = cos(radians(TESR_SpotLightDirection.w * FL_SIZE));
	float angleCosMin = cos(radians(TESR_SpotLightDirection.w * FL_SIZE * 0.5));
	float cone = pow(invlerps(angleCosMax, angleCosMin, shades(lightDir, lightVector * -1)), 2.0);

	// Ramp the pool down point blank so a wall a foot away doesn't clip to white
	float nearFade = (FL_NEARFADE > 0.0) ? smoothstep(0.0, FL_NEARFADE, length(lightToWorld)) : 1.0;

    float3 light = (diffuse + specular) * lightColor * cone * atten * lightTexture * isShadow * nearFade;


	// if (lightSpaceCoord.x > 0.0 && lightSpaceCoord.x < 1.0 && lightSpaceCoord.y > 0.0 && lightSpaceCoord.y < 1.0) return float4(light.xxx, 1);
	// color = displayBuffer(color, IN.UVCoord, float2(0.7, 0.15), float2(0.2, 0.2), TESR_ShadowSpotlightBuffer0);

    // return delinearize(color);
    return float4(light, 1);
}


// simple local average without weights. Use scaleFactor to only blur a portion of the screen starting from the top left corner
float4 BoxBlurMin (VSOUT IN, uniform sampler2D buffer, uniform float scaleFactor) :COLOR0
{
	float2 io = float2(-1, 1) * 0.5;
	clip((IN.UVCoord <= scaleFactor) - 1);

	float2 maxuv = scaleFactor - 1.5 * TESR_ReciprocalResolution.xy;
	float4 color = tex2D(buffer, IN.UVCoord);
	color = min(color, tex2D(buffer, min(IN.UVCoord + io.xx * TESR_ReciprocalResolution.xy, maxuv)));
	color = min(color, tex2D(buffer, min(IN.UVCoord + io.xy * TESR_ReciprocalResolution.xy, maxuv)));
	color = min(color, tex2D(buffer, min(IN.UVCoord + io.yx * TESR_ReciprocalResolution.xy, maxuv)));
	color = min(color, tex2D(buffer, min(IN.UVCoord + io.yy * TESR_ReciprocalResolution.xy, maxuv)));

	return color;
	// return float4(color.rgb/= 5, 1);
}


// simple local average without weights. Use scaleFactor to only blur a portion of the screen starting from the top left corner
float4 BoxBlurAvg (VSOUT IN, uniform sampler2D buffer, uniform float scaleFactor) :COLOR0
{
	float2 io = float2(-1, 1) * 0.5;
	clip((IN.UVCoord <= scaleFactor) - 1);

	float2 maxuv = scaleFactor - 1.5 * TESR_ReciprocalResolution.xy;
	float4 color = tex2D(buffer, IN.UVCoord);
	color += tex2D(buffer, min(IN.UVCoord + io.xx * TESR_ReciprocalResolution.xy, maxuv));
	color += tex2D(buffer, min(IN.UVCoord + io.xy * TESR_ReciprocalResolution.xy, maxuv));
	color += tex2D(buffer, min(IN.UVCoord + io.yx * TESR_ReciprocalResolution.xy, maxuv));
	color += tex2D(buffer, min(IN.UVCoord + io.yy * TESR_ReciprocalResolution.xy, maxuv));

	// return color;
	return float4(color.rgb/= 5, 1);
}


float4 Combine (VSOUT IN) : COLOR0
{
	// With RenderPreTonemapping, which is the default, this pass draws onto the game's HDR
	// scene surface and TESR_SourceBuffer is already linear. Decoding it as sRGB and
	// re-encoding on the way out is then a second transform. It almost cancels where no
	// light is added, but it also feeds the exp() rolloff below a value divided by 12.92
	// in the dark end, so the rolloff stops rolling off and close surfaces blow out.
	// Only apply the transform when this really is the post tonemapping LDR surface.
	bool sourceIsLinear = FL_LINEARSOURCE > 0.5;
	float4 source = tex2D(TESR_SourceBuffer, IN.UVCoord);
	float4 color = sourceIsLinear ? source : linearize(source);
	float4 light = tex2D(TESR_RenderedBuffer, IN.UVCoord);

	float3 addLight = color.rgb * max(0.0, luma(exp(-color.rgb * 3.5)) * light.rgb) * FL_INTENSITY; // modulate light with base color brightness to compensate for the post process aspect

	// In-air light shaft from the FlashlightBeam effect. Gated on the same value the march
	// is gated on, so the half res buffer can never bleed in once the beam is switched off.
	if (TESR_VolumetricControl.x > 0.0) {
		// The s7 sampler is bilinear, so this upsamples the half res buffer for free
		float3 vol = tex2D(TESR_VolumetricBuffer, IN.UVCoord).rgb;

		// The shaft yields wherever this pixel's surface is already inside the beam. That
		// surface gets the lit pool, and stacking the air glow on top of it just reads as a
		// hotter cast. The test is geometric - the cone and attenuation evaluated at the
		// surface - so the pool keeps its normal brightness at any shaft strength, while
		// surfaces outside the beam still get the full shaft in front of them.
		float3 surfPos = TESR_CameraPosition.xyz + toWorld(IN.UVCoord) * readDepth(IN.UVCoord);
		float3 surfToL = TESR_SpotLightPosition.xyz - surfPos;
		float  surfDist = length(surfToL);
		float surfCone = pow(invlerps(
			cos(radians(TESR_SpotLightDirection.w * FL_SIZE)),
			cos(radians(TESR_SpotLightDirection.w * FL_SIZE * 0.5)),
			shades(TESR_SpotLightDirection.xyz, -surfToL / max(surfDist, 0.001))), 2.0);
		float sSurf = saturate(sqr(surfDist / TESR_SpotLightPosition.w));
		float surfAtten = saturate(sqr(1.0 - sSurf) / (1.0 + 5.0 * sSurf));
		vol *= 1.0 - 0.85 * saturate(surfCone * surfAtten * 4.0);

		// Soft ceiling: a ray looking straight down the beam integrates the whole lit
		// column and would otherwise fill the screen. Dim shafts pass nearly unchanged.
		vol /= 1.0 + luma(vol) * 2.5;
		addLight += vol;
	}

	// Reinhard style roll off on the added light only, so the centre of the pool stops
	// short of clipping without dimming the falloff around it
	if (FL_HOTSPOTLIMIT > 0.0) {
		float limit = 1.0 / FL_HOTSPOTLIMIT;
		addLight *= limit / max(limit, luma(addLight));
	}

	color.rgb += addLight;

    return sourceIsLinear ? color : delinearize(color);
}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 NoShadow();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Flashlight();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 BoxBlurMin(TESR_RenderedBuffer, 1.0);
	}
	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 BoxBlurAvg(TESR_RenderedBuffer, 1.0);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Combine();
	}
}


technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadows();
	}
	
	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 DepthBlur(TESR_RenderedBuffer, OffsetMaskH, 1.0, 350, TESR_SpotLightPosition.w * 2);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 DepthBlur(TESR_RenderedBuffer, OffsetMaskV, 1.0, 350, TESR_SpotLightPosition.w * 2);
	}
	
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Flashlight();
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 BoxBlurMin(TESR_RenderedBuffer, 1.0);
	}
	pass {
		VertexShader = compile vs_3_0 FrameVS();
	 	PixelShader = compile ps_3_0 BoxBlurAvg(TESR_RenderedBuffer, 1.0);
	}

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Combine();
	}

}
 