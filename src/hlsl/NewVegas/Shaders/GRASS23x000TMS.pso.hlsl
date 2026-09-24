// Grass PS -- vanilla GRASS23x000TMS.pso plus forward sun shadow and grass lighting.
// Shared by GRASS23x002.vso (LFS) and GRASS23x003.vso (LVS).
//
// The VS hands the lighting over already split, so only the sun is shadowed:
//   TEXCOORD4.xyz ambient    TEXCOORD5.xyz sun    TEXCOORD5.w fade    COLOR0 fog (.w amount)
// and, for the grass lighting below, the raw pieces to redo the sun per pixel:
//   TEXCOORD2 sun normal + height above the base    TEXCOORD3 offset from the clump centre
//   TEXCOORD6 sun colour before N.L       TEXCOORD7 sun direction
//
// Grass lighting. Vanilla lights a whole clump as one flat card facing its up direction, so a field
// is one tone wherever the sun is. Four additions, each off at 0 (and all at 0 is vanilla exactly):
//   - Roundness bends each blade's normal outward from the clump's centre, so the side of a clump
//     facing away from the sun falls into shade and fields gain depth.
//   - Root darkening shades blades toward the ground over the bottom RootDarkeningHeight units,
//     measured from the geometry: each vertex's height above the clump's base along its up axis.
//     (Not the vertex alpha the wind weights its sway by: on FNV's grass meshes that runs sideways
//     across the blades, not up them.)
//   - Translucency lets sunlight through the blades when the sun is behind them: the glow of a
//     backlit field. Coloured by the grass texture, like light through a leaf, and shadowed.
//   - Specular adds a soft sheen off the rounded normals toward the sun.

#include "includes/Shadow.hlsl"
#include "includes/PBRScale.hlsl"

// c146/c147: past PBRScale/SkyAmbient's c134-c145. The top is SkyAmbient's other skylighting mode,
// whose TESR_SkyIrradiance[9] array runs c137-c145.
float4 TESR_GrassLighting  : register(c146); // x: translucency, y: roundness, z: root darkening, w: specular
float4 TESR_GrassLighting2 : register(c147); // x: translucency focus, y: specular glossiness, z: debug view, w: diffuse wrap
float4 TESR_GrassLighting3 : register(c148); // x: root darkening height (units), y: point light strength

// Point lights: NVR's nearby-light lists, filled every frame by ShaderManager::GetNearbyLights before
// the world renders. Vanilla never gives grass any point light (its passes are all BSSM_GRASS_DIRONLY),
// so these are the same lists the water shaders read. Positions are ABSOLUTE world with the radius in
// w, each list packed from index 0 with empty slots zeroed. Colour is rgb with the dimmer in w, the
// shadow-casting lights' at [0..11] and the rest at [12..23]. c149-c197, inside ps_3_0's 224.
float4 TESR_CameraPosition          : register(c149);
float4 TESR_ShadowLightPosition[12] : register(c150);
float4 TESR_LightPosition[12]       : register(c162);
float4 TESR_LightColor[24]          : register(c174);

sampler2D DiffuseMap : register(s0);

// Grass-data sentinel. This pixel shader is not only paired with the four grass vertex shaders: it
// has also been found drawing hair, through a vertex shader that writes none of the grass data below,
// so those interpolators hold undefined values there. The grass vertex shaders stamp 2.0 into
// sunColor.w; without it every grass-lighting term is skipped and the pixel is lit exactly as before
// the grass lighting existed. Selects, not multiplications, so undefined inputs (possibly NaN) never
// reach the result. Same idea as SHADOW_VS_SENTINEL, a different value so the two cannot be confused.
#define GRASS_VS_SENTINEL 2.0f

// One point light on a grass pixel: the vanilla attenuation NVR's object shaders use,
// 1 - saturate(d^2 / r^2), times the same wrapped diffuse as the sun. toLight is built as
// (light - camera) - pixel with the pixel camera-relative, so the large world coordinates cancel
// before any per-pixel maths. No cube shadows: grass does not sample the point-shadow maps.
float3 GrassPointLight(float4 light, float4 colour, float3 pixelFromCamera, float3 N, float wrap) {
    float3 toLight = (light.xyz - TESR_CameraPosition.xyz) - pixelFromCamera;
    float distSq = dot(toLight, toLight);
    float att = 1.0f - saturate(distSq / max(light.w * light.w, 1.0f));
    float3 L = toLight * rsqrt(max(distSq, 1e-4f));
    float diffuse = saturate((dot(L, N) + wrap) / (1.0f + wrap));
    return light.w > 0.0f ? colour.rgb * colour.w * att * diffuse : 0.0f;
}

struct PS_INPUT {
    float2 uv             : TEXCOORD0;
    float4 shadowWorldPos : TEXCOORD1;
    float4 blade          : TEXCOORD2_centroid;   // xyz: sun normal, w: height above the clump's base (units)
    float4 bladeOffset    : TEXCOORD3_centroid;   // xyz: offset from the clump centre, w: VS variant 0-3
    float3 ambient        : TEXCOORD4_centroid;
    float4 sun            : TEXCOORD5_centroid;   // .w = distance fade
    float4 sunColor       : TEXCOORD6_centroid;   // xyz: sun before N.L, w: GRASS_VS_SENTINEL
    float3 sunDir         : TEXCOORD7_centroid;
    float4 fog            : COLOR0;               // .w = fog amount
};

struct PS_OUTPUT {
    float4 color : COLOR0;
};

PS_OUTPUT main(PS_INPUT IN) {
    PS_OUTPUT OUT;

    float translucency = TESR_GrassLighting.x;
    float roundness = TESR_GrassLighting.y;
    float rootDarkening = TESR_GrassLighting.z;
    float specular = TESR_GrassLighting.w;
    float translucencyFocus = max(TESR_GrassLighting2.x, 1.0f);
    float gloss = max(TESR_GrassLighting2.y, 1.0f);
    float wrap = saturate(TESR_GrassLighting2.w);

    // Outside the guard: the skylight needs this normal whether or not forward shadows
    // are compiled in, and ForwardShadows is a live setting that can switch them off.
    float3 shadowNormal = GetShadowGeometricNormal(IN.shadowWorldPos.xyz);
    float present = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f;
    float shadow = 1.0f;
#if FORWARD_SHADOWS
    // ddx/ddy must stay at top level, outside dynamic flow control.
    shadow = present > 0.5f
         ? GetSunShadow(IN.shadowWorldPos.xyz, shadowNormal)
         : 1.0f;
#endif

    bool grassData = abs(IN.sunColor.w - GRASS_VS_SENTINEL) < 0.001f;
    float3 sunColor = IN.sunColor.rgb;

    float3 L = normalize(IN.sunDir);
    float3 V = normalize(IN.shadowWorldPos.xyz);   // camera-relative world position: camera to pixel

    // Rounded normal: the variant's sun normal tipped outward by the blade's offset from the clump
    // centre. The tilt builds up over the first ~24 units out rather than jumping to full at once:
    // with a small softening the normal flipped from one side to the other within a few units of
    // the centre, and a low sun drew a hard terminator straight down the middle of every clump.
    float3 offset = IN.bladeOffset.xyz;
    float3 N = normalize(normalize(IN.blade.xyz) + roundness * offset / (length(offset) + 24.0f));

    // Wrapped diffuse. Blades are thin and light wraps around and through them, so the side of a
    // clump facing away from the sun dims gradually instead of dropping to black the moment N.L
    // passes zero. (N.L + w) / (1 + w): w = 0 is plain Lambert, 1 lights all but the exact back.
    float wrapped = saturate((dot(L, N) + wrap) / (1.0f + wrap));

    // At Roundness 0 keep the vertex shader's own N.L, so vanilla stays vanilla to the bit.
    float3 sun = (grassData && roundness > 0.0f) ? sunColor * wrapped : IN.sun.xyz;
    sun *= shadow;

    // Translucency: strongest looking straight toward the sun, narrowed by the focus exponent, and
    // weighted toward the tips, where blades are thinnest.
    // Root (0) to tip (1) over the bottom RootDarkeningHeight units of the clump.
    float tip = saturate(IN.blade.w / max(TESR_GrassLighting3.x, 1.0f));
    float through = pow(saturate(dot(V, L)), translucencyFocus) * translucency * (0.5f + 0.5f * tip) * present;
    through = grassData ? through : 0.0f;
    float3 transmitted = grassData ? sunColor * shadow * through : 0.0f;

    // Point lights (PointLights strength; 0 skips the loop). Both lists are packed from slot 0, so
    // the loop stops at the first slot where both are empty -- the usual handful of lights costs a
    // handful of iterations, not 24.
    float3 pointLight = 0.0f;
    float pointStrength = TESR_GrassLighting3.y;
    if (grassData && pointStrength > 0.0f) {
        [loop]
        for (int i = 0; i < 12; i++) {
            if (TESR_ShadowLightPosition[i].w <= 0.0f && TESR_LightPosition[i].w <= 0.0f) break;
            pointLight += GrassPointLight(TESR_ShadowLightPosition[i], TESR_LightColor[i], IN.shadowWorldPos.xyz, N, wrap);
            pointLight += GrassPointLight(TESR_LightPosition[i], TESR_LightColor[12 + i], IN.shadowWorldPos.xyz, N, wrap);
        }
        pointLight *= pointStrength;
    }

    // Root darkening: lets the roots sit in their own shade.
    float ao = grassData ? lerp(1.0f - rootDarkening, 1.0f, tip) : 1.0f;

    // Same split getSunLighting/getAmbientLighting apply on the object path.
    float3 lighting = (PBRLight(sun + transmitted + pointLight) + PBRAmbient(IN.ambient.xyz) + SkyAmbient(shadowNormal, present)) * ao;

    float4 albedo = tex2D(DiffuseMap, IN.uv.xy);
    float3 litColor = lighting * albedo.rgb;

    // Sheen: Blinn-Phong off the rounded normal, in the light's colour rather than the texture's.
    // Normalised by hand: L - V is zero looking exactly into the sun, and normalize() would NaN.
    float3 H = L - V;
    H *= rsqrt(max(dot(H, H), 1e-8f));
    float sheen = grassData ? pow(saturate(dot(N, H)), gloss) * specular * present : 0.0f;
    litColor += grassData ? PBRLight(sunColor * shadow) * sheen : 0.0f;

    OUT.color.rgb = lerp(litColor, IN.fog.rgb, IN.fog.w);
    OUT.color.a = saturate(albedo.a * 1.75f) * IN.sun.w;

    // Debug views ([Shaders.Grass.Main] DebugView): one term of the lighting on its own, unfogged,
    // keeping the blade's alpha so the grass keeps its shape. All read the live settings, so a
    // slider at 0 shows as its term going flat or black.
    //   1 rounded normals, as colour      2 sun diffuse: wrapped N.L x shadow   3 sun shadow alone
    //   4 translucency: the glow factor   5 sheen    6 root (black) to tip (white) over RootDarkeningHeight
    //   7 which grass vertex shader fed this pixel: red 000, green 001, blue 002, yellow 003
    //   8 point lights alone, in their own colour
    // In every view, magenta = drawn by this shader but WITHOUT grass data (not one of the four grass
    // vertex shaders -- e.g. hair), so none of the grass lighting applies to it.
    float debugView = TESR_GrassLighting2.z;
    if (debugView > 0.5f) {
        float3 view = N * 0.5f + 0.5f;
        view = debugView > 1.5f ? wrapped * shadow : view;
        view = debugView > 2.5f ? shadow : view;
        view = debugView > 3.5f ? saturate(through * shadow) : view;
        view = debugView > 4.5f ? saturate(sheen * shadow) : view;
        view = debugView > 5.5f ? tip : view;
        // Selects rather than an array: ps_3_0 cannot index a local array with a runtime value.
        float variant = IN.bladeOffset.w;
        float3 variantColour = variant < 0.5f ? float3(1, 0, 0) : (variant < 1.5f ? float3(0, 1, 0) : (variant < 2.5f ? float3(0, 0, 1) : float3(1, 1, 0)));
        view = debugView > 6.5f ? variantColour : view;
        view = debugView > 7.5f ? saturate(pointLight) : view;
        OUT.color.rgb = grassData ? view : float3(1.0f, 0.0f, 1.0f);
    }

    return OUT;
};
