// Depth-based blending of objects into terrain, for GAME shaders (objects).
//
// Ported in principle, not in implementation, from Community Shaders' Terrain Blending. That
// feature is D3D11 on a DEFERRED renderer: it writes the blend weight into G-buffer alpha
// (Lighting.hlsl psout.Diffuse.w) and resolves it in DeferredCompositeCS. FNV is forward and
// has no G-buffer, so there is nowhere to defer the weight to and it has to act at draw time.
//
// It needs none of their hook machinery, though. They render terrain into a second depth target
// and merge with a compute shader because Skyrim's prepass gives them no other way to know where
// terrain is. Here ShaderRecord::SetCT resolves the live depth buffer via RESZ for any shader
// that declares TESR_DepthBuffer, so an object simply reads what has already been drawn this
// frame -- which, by the time statics render, includes the terrain behind them.
//
// THIS IS A PROBE. It computes the blend factor and can display it; it does not yet apply it.
// Two things have to hold before applying is worth building, and only a running game can say:
//   1. the depth read lands after terrain and before this object, so the factor tracks ground
//   2. the per-shader-bind RESZ resolve is affordable at this call frequency
// Set TERRAIN_BLEND_DEBUG to see the factor directly: white where an object meets terrain,
// black away from it. Wrong ordering shows as a factor that ignores the ground entirely.

#ifndef TERRAIN_BLEND_DEBUG
    #define TERRAIN_BLEND_DEBUG 1
#endif

// Distance in world units over which an object fades into the terrain behind it. Community
// Shaders uses 10 linear depth units; FNV's unit scale differs, so this is a starting guess.
#ifndef TERRAIN_BLEND_RANGE
    #define TERRAIN_BLEND_RANGE 12.0f
#endif

// Pinned past Shadow.hlsl's block, which documents c100-c133 as clear and ends at c133.
float4 TESR_ReciprocalResolution : register(c134);
float4 TESR_CameraData           : register(c135); // x: nearZ, y: farZ

// Objects top out at s7 and Shadow.hlsl takes s9, so s10 is free here. MUST stay on ONE line,
// closing brace included: ShaderTextureValue::GetSamplerStateString finds "register ( sN )" and
// reads only to the end of that line, and silently falls back to POINT/WRAP defaults if split.
#ifndef TERRAIN_BLEND_DEPTH_SAMPLER_REG
    #define TERRAIN_BLEND_DEPTH_SAMPLER_REG s10
#endif
sampler2D TESR_DepthBuffer : register(TERRAIN_BLEND_DEPTH_SAMPLER_REG) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// 0 where the object is far in front of whatever is behind it, rising to 1 as it approaches.
//
// cameraRelativeWorldPos is Shadow.hlsl's shadowWorldPos.xyz, already carried per-fragment.
// TESR_InvViewTransform's third row is the camera forward in world space, so projecting onto it
// gives view depth -- the same quantity TESR_DepthBuffer stores once scaled by farZ -- without
// needing a second interpolator or any new vertex shader work.
float GetTerrainBlendFactor(float3 cameraRelativeWorldPos, float2 vpos) {
    float2 screenUV = (vpos + 0.5f) * TESR_ReciprocalResolution.xy;

    float sceneViewZ = tex2D(TESR_DepthBuffer, screenUV).x * TESR_CameraData.y;
    float objectViewZ = dot(cameraRelativeWorldPos, TESR_InvViewTransform[2].xyz);

    return saturate((sceneViewZ - objectViewZ) / TERRAIN_BLEND_RANGE);
}
