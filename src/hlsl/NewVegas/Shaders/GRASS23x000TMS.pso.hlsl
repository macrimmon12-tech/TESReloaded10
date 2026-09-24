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
// And, for grass textures that have one, a per-texture normal map (<name>_n.dds, NormalMaps) adds
// blade detail on top of the rounded normal; its alpha masks the sheen.
//
// Cost. Grass is heavily overdrawn, so every instruction here runs many times per screen pixel.
// Pixels the engine's alpha test would reject skip everything; the costly lighting (rounded normal,
// translucency, sheen, point lights) sits in one branch that fades out past DetailDistance; and the
// forward sun shadow can be limited to ShadowDistance.

#include "includes/Shadow.hlsl"
#include "includes/PBRScale.hlsl"

// c146/c147: past PBRScale/SkyAmbient's c134-c145. The top is SkyAmbient's other skylighting mode,
// whose TESR_SkyIrradiance[9] array runs c137-c145.
float4 TESR_GrassLighting  : register(c146); // x: translucency, y: roundness, z: root darkening, w: specular
float4 TESR_GrassLighting2 : register(c147); // x: translucency focus, y: specular glossiness, z: debug view, w: diffuse wrap
float4 TESR_GrassLighting3 : register(c148); // x: root darkening height (units), y: point light strength, z: detail distance, w: detail fade

// Point lights: NVR's nearby-light lists, filled every frame by ShaderManager::GetNearbyLights before
// the world renders. Vanilla never gives grass any point light (its passes are all BSSM_GRASS_DIRONLY),
// so these are the same lists the water shaders read. Positions are ABSOLUTE world with the radius in
// w, each list packed from index 0 with empty slots zeroed. Colour is rgb with the dimmer in w, the
// shadow-casting lights' at [0..11] and the rest at [12..23]. c149-c197, inside ps_3_0's 224.
float4 TESR_CameraPosition          : register(c149);
float4 TESR_ShadowLightPosition[12] : register(c150);
float4 TESR_LightPosition[12]       : register(c162);
float4 TESR_LightColor[24]          : register(c174);

float4 TESR_GrassLighting4 : register(c198); // x: shadow distance, y: shadow fade (units; distance 0 = no limit), z: brightness, w: ambient normal

// Per-texture normal map, bound per grass geometry by the DLL (NewVegas/Hooks/GrassNormals.cpp) when
// the grass texture has a <name>_n.dds beside it. Not TESR_ names: set directly, not through NVR's
// constant table. x: strength, 0 when this grass has no map (s10 then unbound); y: green sign.
sampler2D GrassNormalMap    : register(s10);
float4    GrassNormalParams : register(c199);

// The engine's alpha test for grass: reference 10, GREATER, as read off the device in game.
#define GRASS_ALPHA_CUTOFF (10.0f / 255.0f)

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
// Also adds the light's sheen to specularOut: the same Blinn-Phong as the sun's, off the same normal
// with the same glossiness; and its translucency to transmitOut: the same glow through the blades as
// the sun's, strongest with the light behind the blade as seen from the camera, narrowed by the same
// focus. Both unscaled here: the caller applies Specular, Translucency and the rest. V is camera to pixel.
float3 GrassPointLight(float4 light, float4 colour, float3 pixelFromCamera, float3 N, float3 V, float wrap, float gloss, float focus, inout float3 specularOut, inout float3 transmitOut) {
    float3 toLight = (light.xyz - TESR_CameraPosition.xyz) - pixelFromCamera;
    float distSq = dot(toLight, toLight);
    float att = 1.0f - saturate(distSq / max(light.w * light.w, 1.0f));
    float3 L = toLight * rsqrt(max(distSq, 1e-4f));
    float diffuse = saturate((dot(L, N) + wrap) / (1.0f + wrap));
    float3 lit = light.w > 0.0f ? colour.rgb * colour.w * att : 0.0f;

    // Normalised by hand: L - V is zero looking exactly into the light, and normalize() would NaN.
    float3 H = L - V;
    H *= rsqrt(max(dot(H, H), 1e-8f));
    specularOut += lit * pow(saturate(dot(N, H)), gloss);
    transmitOut += lit * pow(saturate(dot(V, L)), focus);

    return lit * diffuse;
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

    // Both gradient operations of this shader, first and at top level: the texture read (implicit
    // derivatives) and the face normal (ddx/ddy). Neither is legal inside the dynamic branch below.
    // The face normal is outside the forward-shadow guard because the skylight needs it whether or
    // not forward shadows are compiled in, and ForwardShadows is a live setting.
    float4 albedo = tex2D(DiffuseMap, IN.uv.xy);
    float3 shadowNormal = GetShadowGeometricNormal(IN.shadowWorldPos.xyz);
    // Screen-space derivatives for the normal map's tangent frame and its explicit-gradient read,
    // taken here for the same reason.
    float2 duvdx = ddx(IN.uv.xy);
    float2 duvdy = ddy(IN.uv.xy);
    float3 dpdx = ddx(IN.shadowWorldPos.xyz);
    float3 dpdy = ddy(IN.shadowWorldPos.xyz);
    OUT.color.a = saturate(albedo.a * 1.75f) * IN.sun.w;

    // Early out for pixels that can never show: a grass card is mostly transparent texture, and
    // overdraw multiplies whatever this shader does, so every one of those pixels used to pay for the
    // whole lighting below before the alpha test threw it away. The engine draws grass with the alpha
    // test on, reference 10, GREATER (read off the device in game), so anything under 10/255 fails it
    // however it is lit; that covers fully transparent texels and the faint edge and fade-out pixels
    // above them. Under alpha-to-coverage (TMS) such a pixel would be at most a sample or so of
    // coverage, never a visible one.
    //
    // A real branch, not just clip(): under DXVK a discard can become demote-to-helper, which keeps
    // the invocation running, so clip() alone would save nothing. [branch] keeps the compiler from
    // flattening it; the only texture reads left below are the shadow atlas's tex2Dlod, legal here.
    [branch]
    if (OUT.color.a < GRASS_ALPHA_CUTOFF) {
        clip(-1.0f);
        // Reads shadowNormal and the derivatives only to pin them above the branch. Used on one side alone, the
        // compiler sinks the derivatives into the other, and derivatives under a branch that only
        // part of a 2x2 quad takes are undefined -- strictly so under DXVK/Vulkan. The pixel is
        // discarded, so the value itself never shows. CI checks the order in the disassembly.
        OUT.color.rgb = shadowNormal + float3(duvdx + duvdy, dpdx.x + dpdy.x);
        return OUT;
    }

    // Distance falloffs, both off at 0 (what a missing setting reads as).
    //   detail      DetailDistance / DetailFade: the rounded normal, wrapped diffuse, translucency,
    //               sheen and point lights fade out with distance, and past it grass takes the vertex
    //               shader's own sun term. They are the costly part of this shader and barely show
    //               on grass a few pixels tall, which is most of the grass on screen.
    //   shadowReach ShadowDistance / ShadowFade: the forward sun shadow fades out the same way.
    float pixelDistance = length(IN.shadowWorldPos.xyz);
    float detailStart = TESR_GrassLighting3.z;
    float detail = detailStart > 0.0f ? 1.0f - saturate((pixelDistance - detailStart) / max(TESR_GrassLighting3.w, 1.0f)) : 1.0f;
    float shadowStart = TESR_GrassLighting4.x;
    float shadowReach = shadowStart > 0.0f ? 1.0f - saturate((pixelDistance - shadowStart) / max(TESR_GrassLighting4.y, 1.0f)) : 1.0f;

    float present = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f;
    float shadow = 1.0f;
#if FORWARD_SHADOWS
    // tex2Dlod inside (SampleShadowAtlas), so legal past the early-out branch above.
    [branch]
    if (present > 0.5f && shadowReach > 0.0f)
        shadow = lerp(1.0f, GetSunShadow(IN.shadowWorldPos.xyz, shadowNormal), shadowReach);
#endif

    bool grassData = abs(IN.sunColor.w - GRASS_VS_SENTINEL) < 0.001f;
    float3 sunColor = IN.sunColor.rgb;

    // Root (0) to tip (1) over the bottom RootDarkeningHeight units of the clump. Cheap, so it stays
    // at every distance.
    float tip = saturate(IN.blade.w / max(TESR_GrassLighting3.x, 1.0f));

    // What grass past the detail distance, and anything without grass data, is lit with: the vertex
    // shader's sun term and none of the extras. N is only read by DebugView 1 out there.
    float3 N = IN.blade.xyz;
    float wrapped = 0.0f;
    float3 sun = IN.sun.xyz;
    float through = 0.0f;
    float3 pointLight = 0.0f;
    float3 pointSheen = 0.0f;
    float3 pointThrough = 0.0f;
    float sheen = 0.0f;
    float sheenMask = 1.0f;
    float3 mapView = 0.15f;   // DebugView 10 where there is no normal map

    [branch]
    if (grassData && detail > 0.0f) {
        float3 L = normalize(IN.sunDir);
        float3 V = IN.shadowWorldPos.xyz / max(pixelDistance, 1e-4f);   // camera to pixel

        // Rounded normal: the variant's sun normal tipped outward by the blade's offset from the
        // clump centre. The tilt builds up over the first ~24 units out rather than jumping to full
        // at once: with a small softening the normal flipped from one side to the other within a few
        // units of the centre, and a low sun drew a hard terminator straight down the middle of
        // every clump.
        float3 offset = IN.bladeOffset.xyz;
        N = normalize(normalize(IN.blade.xyz) + roundness * offset / (length(offset) + 24.0f));

        // Normal map, if this grass texture has one. Grass vertices carry no tangents, so the frame
        // comes from the screen-space derivatives of position and UV (a cotangent frame) around the
        // card's face normal, which already faces the camera on either side of a two-sided card.
        // The map's deviation from the flat card is added on top of the rounded normal, so it adds
        // blade detail without undoing the clump shape. tex2Dgrad: an explicit-gradient read is legal
        // in this branch, where tex2D's implicit derivatives would not be.
        [branch]
        if (GrassNormalParams.x > 0.0f) {
            float3 Nf = shadowNormal;
            float3 dp2perp = cross(dpdy, Nf);
            float3 dp1perp = cross(Nf, dpdx);
            float3 T = dp2perp * duvdx.x + dp1perp * duvdy.x;
            float3 B = dp2perp * duvdx.y + dp1perp * duvdy.y;
            float invScale = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20f));

            float4 mapSample = tex2Dgrad(GrassNormalMap, IN.uv.xy, duvdx, duvdy);
            float3 tangentNormal = mapSample.xyz * 2.0f - 1.0f;
            tangentNormal.y *= GrassNormalParams.y;
            float3 mapped = normalize((T * tangentNormal.x + B * tangentNormal.y) * invScale + Nf * tangentNormal.z);

            N = normalize(N + (mapped - Nf) * GrassNormalParams.x);
            // Bethesda normal maps carry the specular mask in alpha; one without alpha reads 1.
            sheenMask = mapSample.a;
            mapView = mapped * 0.5f + 0.5f;
        }

        // Wrapped diffuse. Blades are thin and light wraps around and through them, so the side of a
        // clump facing away from the sun dims gradually instead of dropping to black the moment N.L
        // passes zero. (N.L + w) / (1 + w): w = 0 is plain Lambert, 1 lights all but the exact back.
        wrapped = saturate((dot(L, N) + wrap) / (1.0f + wrap));

        // Per-pixel sun whenever there is a per-pixel normal to light: rounded, or normal-mapped. At
        // Roundness 0 without a map keep the vertex shader's own N.L, so vanilla stays vanilla to the
        // bit (lerp of a value with itself is exact).
        float3 roundSun = (roundness > 0.0f || GrassNormalParams.x > 0.0f) ? sunColor * wrapped : IN.sun.xyz;
        sun = lerp(IN.sun.xyz, roundSun, detail);

        // Translucency: strongest looking straight toward the sun, narrowed by the focus exponent,
        // and weighted toward the tips, where blades are thinnest.
        through = pow(saturate(dot(V, L)), translucencyFocus) * translucency * (0.5f + 0.5f * tip) * present * detail;

        // Point lights (PointLights strength; 0 skips the loop). Both lists are packed from slot 0,
        // so the loop stops at the first slot where both are empty -- the usual handful of lights
        // costs a handful of iterations, not 24.
        float pointStrength = TESR_GrassLighting3.y;
        [branch]
        if (pointStrength > 0.0f) {
            [loop]
            for (int i = 0; i < 12; i++) {
                if (TESR_ShadowLightPosition[i].w <= 0.0f && TESR_LightPosition[i].w <= 0.0f) break;
                pointLight += GrassPointLight(TESR_ShadowLightPosition[i], TESR_LightColor[i], IN.shadowWorldPos.xyz, N, V, wrap, gloss, translucencyFocus, pointSheen, pointThrough);
                pointLight += GrassPointLight(TESR_LightPosition[i], TESR_LightColor[12 + i], IN.shadowWorldPos.xyz, N, V, wrap, gloss, translucencyFocus, pointSheen, pointThrough);
            }
            pointLight *= pointStrength * detail;
            // Sheen from the lights: a campfire or lamp glinting on nearby blades, as the sun does.
            pointSheen *= pointStrength * detail * specular * sheenMask;
            // Glow through the blades from the lights: a campfire behind the grass lighting it up,
            // weighted to the tips like the sun's.
            pointThrough *= pointStrength * detail * translucency * (0.5f + 0.5f * tip);
        }

        // Sheen: Blinn-Phong off the rounded normal. Normalised by hand: L - V is zero looking
        // exactly into the sun, and normalize() would NaN.
        float3 H = L - V;
        H *= rsqrt(max(dot(H, H), 1e-8f));
        sheen = pow(saturate(dot(N, H)), gloss) * specular * present * detail * sheenMask;
    }
    sun *= shadow;

    // Selects, not multiplications: without grass data sunColor is undefined, possibly NaN.
    float3 transmitted = grassData ? sunColor * shadow * through : 0.0f;

    // Root darkening: lets the roots sit in their own shade.
    float ao = grassData ? lerp(1.0f - rootDarkening, 1.0f, tip) : 1.0f;

    // Normal-based ambient (AmbientNormal): the sky light is taken from the flat card normal, blended
    // toward the lit normal -- rounded and normal-mapped -- so the side of a clump facing away from
    // the sky gets less of it too. 0 is the flat card normal alone, the look before this setting.
    // Fades with the detail lighting it borrows the normal from. Selects keep hair's undefined N out.
    float ambientBlend = saturate(TESR_GrassLighting4.w) * detail;
    float3 ambientBlended = lerp(shadowNormal, N * rsqrt(max(dot(N, N), 1e-8f)), ambientBlend);
    float ambientLength = length(ambientBlended);
    float3 ambientNormal = (grassData && ambientBlend > 0.0f && ambientLength > 1e-3f) ? ambientBlended / ambientLength : shadowNormal;

    // Same split getSunLighting/getAmbientLighting apply on the object path.
    float3 lighting = (PBRLight(sun + transmitted + pointLight + pointThrough) + PBRAmbient(IN.ambient.xyz) + SkyAmbient(ambientNormal, present)) * ao;

    // Brightness: scales the grass texture's colour, for grass that reads too bright under the extra
    // light the grass lighting adds. Grass only, not hair; 1 (or an unset 0) leaves it untouched.
    float brightness = TESR_GrassLighting4.z > 0.0f ? TESR_GrassLighting4.z : 1.0f;
    float3 litColor = lighting * albedo.rgb * (grassData ? brightness : 1.0f);

    // Sheen, in the light's colour rather than the texture's.
    litColor += grassData ? PBRLight(sunColor * shadow) * sheen + PBRLight(pointSheen) : 0.0f;

    OUT.color.rgb = lerp(litColor, IN.fog.rgb, IN.fog.w);

    // Debug views ([Shaders.Grass.Main] DebugView): one term of the lighting on its own, unfogged,
    // keeping the blade's alpha so the grass keeps its shape. All read the live settings, so a
    // slider at 0 shows as its term going flat or black.
    //   1 rounded (and normal-mapped) normals, as colour      2 sun diffuse: wrapped N.L x shadow   3 sun shadow alone
    //   4 translucency: the glow, sun and point lights   5 sheen, sun and point lights    6 root (black) to tip (white) over RootDarkeningHeight
    //   7 which grass vertex shader fed this pixel: red 000, green 001, blue 002, yellow 003
    //   8 point lights alone, in their own colour
    //   9 distance falloffs: red = detail (DetailDistance), green = forward shadow reach (ShadowDistance);
    //     yellow is full lighting, green shadows only, black neither
    //  10 normal maps: the normal-mapped normal as colour where this grass has a map, dark grey where not
    // In every view, magenta = drawn by this shader but WITHOUT grass data (not one of the four grass
    // vertex shaders -- e.g. hair), so none of the grass lighting applies to it.
    float debugView = TESR_GrassLighting2.z;
    [branch]
    if (debugView > 0.5f) {
        float3 view = N * 0.5f + 0.5f;
        view = debugView > 1.5f ? wrapped * shadow : view;
        view = debugView > 2.5f ? shadow : view;
        view = debugView > 3.5f ? saturate(through * shadow + pointThrough) : view;
        view = debugView > 4.5f ? saturate(sheen * shadow + pointSheen) : view;
        view = debugView > 5.5f ? tip : view;
        // Selects rather than an array: ps_3_0 cannot index a local array with a runtime value.
        float variant = IN.bladeOffset.w;
        float3 variantColour = variant < 0.5f ? float3(1, 0, 0) : (variant < 1.5f ? float3(0, 1, 0) : (variant < 2.5f ? float3(0, 0, 1) : float3(1, 1, 0)));
        view = debugView > 6.5f ? variantColour : view;
        view = debugView > 7.5f ? saturate(pointLight) : view;
        view = debugView > 8.5f ? float3(detail, shadowReach, 0.0f) : view;
        view = debugView > 9.5f ? mapView : view;
        OUT.color.rgb = grassData ? view : float3(1.0f, 0.0f, 1.0f);
    }

    return OUT;
};
