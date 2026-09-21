// Stochastic texture tiling for terrain, after "Procedural Stochastic Textures by Tiling and
// Blending" (Deliot & Heitz, 2019) -- https://eheitzresearch.wordpress.com/722-2/
// Ported from Skyrim Community Shaders' Terrain Variation feature.
//
// The idea: instead of sampling a terrain texture once at the tiled UV, sample it twice at two
// hash-randomised offsets and blend. The offsets come from the two dominant corners of a
// triangular lattice laid over the UV, so neighbouring pixels agree and the result is continuous,
// but the texture's phase changes from cell to cell -- which is what breaks up the repeat that
// makes a large open landscape read as wallpaper.
//
// Two differences from the D3D11 source, one forced by ps_3_0 and one that ps_3_0 makes better:
//
//  1. No StochasticSampleLOD / StochasticEffectParallax. Terrain LOD and the parallax ray-march
//     stay on the vanilla path.
//
//  2. Mip selection. Hashed UV offsets break the implicit derivatives, so the original computes
//     an explicit mip: log2 of the UV derivatives plus log2 of the texture size, the latter from
//     Texture2D::GetDimensions. GetDimensions is SM4+ and has no ps_3_0 equivalent, which would
//     have meant feeding all 14 terrain texture sizes in as constants. ps_3_0 has tex2Dgrad
//     (texldd) instead: it takes the ORIGINAL derivatives directly and lets the hardware do the
//     texel-space conversion, so no texture size is needed, and it uses the real anisotropic
//     footprint rather than the source's min-derivative approximation of it. Parallax.hlsl
//     already calls tex2Dgrad on these same terrain samplers, so the instruction is known good
//     in this shader chain.
//
// Nothing here costs anything unless TERRAIN_VARIATION is 1: ComputeStochasticOffsets is only
// called under that #if, so fxc drops these functions and the c93 constant with them.

// x: lattice scale, in UV units per cell. Terrain UVs are already in texture-tile space (one
//    unit = one repeat of the texture), so 1.0 puts one lattice cell on one tile.
// y: height influence. 0 is a pure linear crossfade between the two taps; higher lets the
//    brighter/taller tap win its own area, so transitions follow relief instead of ghosting.
// z, w: unused.
float4 TESR_TerrainVariationData : register(c93);

// Maps the square UV grid onto the equilateral triangular lattice the blend is built on.
static const float2x2 TERRAIN_VARIATION_SKEW = float2x2(1.0f, 0.0f, -0.57735027f, 1.15470054f);
static const float2 TERRAIN_VARIATION_HASH = float2(1271.5151f, 3337.8237f);
static const float3 TERRAIN_VARIATION_LUMA = float3(0.2126f, 0.7152f, 0.0722f);
// Fade width for the rank swap between the second and third corners, so the corner we discard
// cannot pop in and out along the line where their weights tie.
static const float TERRAIN_VARIATION_TAP_FADE = 0.1f;
// Disables that fade near a first/second corner tie, so the primary swap stays symmetric.
static const float TERRAIN_VARIATION_DOMINANCE_GUARD = 0.02f;

struct StochasticOffsets {
	float2 offset1;
	float2 offset2;
	float  tap1Weight;  // normalised weight of tap 1, in [0.5, 1]; tap 2 takes the remainder
};

float2 TerrainVariationHash(float2 s) {
	s = frac(s * TERRAIN_VARIATION_HASH);
	s += dot(s, s.yx + 19.19f);
	return frac((s.xx + s.yy) * s.yx);
}

// The two highest-weighted corners of the triangular lattice cell containing uv, with the
// contrast exponent already applied (it is 2, so the weights are squared rather than pow()'d).
// Corners are carried as float3: xy the cell id that seeds the hash, z the barycentric weight.
StochasticOffsets ComputeStochasticOffsets(float2 uv, float scale) {
	float2 skewUV = mul(TERRAIN_VARIATION_SKEW, uv * scale);
	float2 vxID = floor(skewUV);
	float2 f = frac(skewUV);
	float bz = 1.0f - f.x - f.y;

	float3 c0, c1, c2;
	if (bz > 0.0f) {
		c0 = float3(vxID, bz);
		c1 = float3(vxID + float2(0.0f, 1.0f), f.y);
		c2 = float3(vxID + float2(1.0f, 0.0f), f.x);
	}
	else {
		c0 = float3(vxID + 1.0f, -bz);
		c1 = float3(vxID + float2(1.0f, 0.0f), 1.0f - f.y);
		c2 = float3(vxID + float2(0.0f, 1.0f), 1.0f - f.x);
	}

	// Rank the three corners by weight, descending.
	float3 t;
	if (c1.z > c0.z) { t = c0; c0 = c1; c1 = t; }
	if (c2.z > c0.z) { t = c0; c0 = c2; c2 = t; }
	if (c2.z > c1.z) { t = c1; c1 = c2; c2 = t; }

	float w1 = saturate(c0.z);
	w1 *= w1;
	float w2 = saturate(c1.z);
	w2 *= w2;
	float rankFade = smoothstep(0.0f, TERRAIN_VARIATION_TAP_FADE, c1.z - c2.z);
	float dominanceGuard = smoothstep(0.0f, TERRAIN_VARIATION_DOMINANCE_GUARD, c0.z - c1.z);
	w2 *= lerp(1.0f, rankFade, dominanceGuard);

	StochasticOffsets o;
	o.offset1 = TerrainVariationHash(c0.xy);
	o.offset2 = TerrainVariationHash(c1.xy);
	o.tap1Weight = w1 / max(w1 + w2, 1e-8f);
	return o;
}

// A neutral set, for the paths that do not run the stochastic sampler: tap 1 at zero offset with
// all the weight, so a caller that blends with it gets the vanilla sample back unchanged.
StochasticOffsets NoStochasticOffsets() {
	StochasticOffsets o;
	o.offset1 = float2(0.0f, 0.0f);
	o.offset2 = float2(0.0f, 0.0f);
	o.tap1Weight = 1.0f;
	return o;
}

// Height-aware tap weight. The source derives this per texture, from whichever map it is
// sampling; here it is derived once, from the ALBEDO, and handed to the matching normal fetch.
// Both maps must blend on the SAME weight -- otherwise a transition band takes its colour from
// one tap and its relief from the other, and lights as if the surface were not what it looks
// like. Alpha is the height channel when the texture has one, luminance otherwise.
float StochasticWeight(float4 s1, float4 s2, float tap1Weight) {
	float h1 = lerp(dot(s1.rgb, TERRAIN_VARIATION_LUMA), s1.a, step(0.001f, s1.a));
	float h2 = lerp(dot(s2.rgb, TERRAIN_VARIATION_LUMA), s2.a, step(0.001f, s2.a));
	float influence = TESR_TerrainVariationData.y;
	float w1 = tap1Weight * (1.0f + influence * h1);
	float w2 = (1.0f - tap1Weight) * (1.0f + influence * h2);
	return w1 / max(w1 + w2, 1e-8f);
}

// Albedo fetch. dx/dy are the ORIGINAL uv derivatives, which the caller must have computed at
// top level: ddx/ddy are illegal inside dynamic flow control in ps_3_0, and the offset UVs would
// give the wrong footprint anyway.
float4 StochasticSampleAlbedo(sampler2D tex, float2 uv, float2 dx, float2 dy, StochasticOffsets o, out float weight) {
	float4 s1 = tex2Dgrad(tex, uv + o.offset1, dx, dy);
	float4 s2 = tex2Dgrad(tex, uv + o.offset2, dx, dy);
	weight = StochasticWeight(s1, s2, o.tap1Weight);
	return lerp(s2, s1, weight);
}

// Any second map that has to line up with the albedo above -- the normal/gloss map. Takes the
// weight StochasticSampleAlbedo returned rather than deriving its own.
float4 StochasticSampleWith(sampler2D tex, float2 uv, float2 dx, float2 dy, StochasticOffsets o, float weight) {
	float4 s1 = tex2Dgrad(tex, uv + o.offset1, dx, dy);
	float4 s2 = tex2Dgrad(tex, uv + o.offset2, dx, dy);
	return lerp(s2, s1, weight);
}
