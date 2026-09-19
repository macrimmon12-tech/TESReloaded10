
float4 TESR_ShadowData : register(c0);
float4 TESR_ShadowFormatData : register(c1);

sampler2D DiffuseMap : register(s0) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = POINT; };

static const float FormatBits = TESR_ShadowFormatData.y;

float2 GetEVSMExponents(in float positiveExponent, in float negativeExponent) {
    const float maxExponent = FormatBits == 0.0 ? 5.54f : 42.0f;

    float2 lightSpaceExponents = float2(positiveExponent, negativeExponent);

    // Clamp to maximum range of fp32/fp16 to prevent overflow/underflow
    return min(lightSpaceExponents, maxExponent);
}

// Applies exponential warp to shadow map depth, input depth should be in [0, 1]
float2 WarpDepth(float depth, float2 exponents) {
    // Rescale depth into [-1, 1]
    depth = 2.0f * depth - 1.0f;
    float pos = exp(exponents.x * depth);
    float neg = -exp(-exponents.y * depth);
    return float2(pos, neg);
}

struct VS_OUTPUT {
    float4 texcoord_0 : TEXCOORD0;
	float4 texcoord_1 : TEXCOORD1;
};

struct PS_OUTPUT {
    float4 color_0 : COLOR0;
};

PS_OUTPUT main(VS_OUTPUT IN) {
    PS_OUTPUT OUT;

	if (TESR_ShadowData.y == 1.0f) { // Leaves (Speedtrees) or alpha is required
		float4 diffuse = tex2D(DiffuseMap, IN.texcoord_1.xy);
        if (diffuse.a < 0.5f)
            discard;
    }
	
	float depth = IN.texcoord_0.z / IN.texcoord_0.w;

	// TESR_ShadowFormatData.x : shadow mode
	// 0: VSM
	// 1: EVSM2
	// 2: EVSM4
    if (TESR_ShadowFormatData.x == 0.0f && !TESR_ShadowData.z) {
		// VSM
		// Cheat to reduce shadow acne in variance maps.
        float dx = ddx(depth);
        float dy = ddy(depth);
        float moment2 = depth * depth + 0.25 * (dx * dx + dy * dy);
        OUT.color_0 = float4(depth, moment2, 0.0f, 1.0f);
    }
    else if (TESR_ShadowFormatData.x == 1.0f && !TESR_ShadowData.z) {
		// EVSM2: the positively warped depth and its square, the same (pos, pos^2) pair EVSM4
		// stores in .xz. GetLightAmountValueEVSM2 reads the second channel as the second moment of
		// the first, so storing the negative warp there made the variance neg - pos*pos: negative for
		// every texel, clamped to the floor every time, and no variance term at all.
		//
		// The fp16 exponent clamp is the tell that this was always the intended layout. It stops at
		// 5.54, and exp(5.54)^2 is about 64900 against a maximum of 65504 - a bound that only matters
		// if the square is what gets stored.
		//
		// Dropping the negative warp is what EVSM2 is: half the channels of EVSM4, so light bleeds
		// through thin geometry more readily, for an atlas of four bytes per texel rather than eight.
        float2 exponents = GetEVSMExponents(40.0f, 5.0f);
        float2 evsm2 = WarpDepth(depth, exponents);
        OUT.color_0 = float4(evsm2.x, evsm2.x * evsm2.x, 0.0f, 1.0f);
    }
    else if (TESR_ShadowFormatData.x == 2.0f && !TESR_ShadowData.z) {
		// EVSM4
		// Cheat to reduce shadow acne in variance maps.
        float2 exponents = GetEVSMExponents(40.0f, 5.0f);
        float2 evsm2 = WarpDepth(depth, exponents);
        OUT.color_0 = float4(evsm2, evsm2 * evsm2);
    }
    else {
		// Only depth.
		OUT.color_0 = float4(depth, 0.0f, 0.0f, 1.0f);
	}

	return OUT;	
};