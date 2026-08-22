/**
 * Copyright (C) 2011 Jorge Jimenez (jorge@iryoku.com)
 * Copyright (C) 2011 Jose I. Echevarria (joseignacioechevarria@gmail.com) 
 * Copyright (C) 2011 Belen Masia (bmasia@unizar.es) 
 * Copyright (C) 2011 Fernando Navarro (fernandn@microsoft.com) 
 * Copyright (C) 2011 Diego Gutierrez (diegog@unizar.es)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the following disclaimer
 *       in the documentation and/or other materials provided with the 
 *       distribution:
 *
 *      "Uses SMAA. Copyright (C) 2011 by Jorge Jimenez, Jose I. Echevarria,
 *       Tiago Sousa, Belen Masia, Fernando Navarro and Diego Gutierrez."
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS 
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR 
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDERS OR CONTRIBUTORS 
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are 
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the copyright holders.
 */

float4 TESR_SMAAResolution;
float4 TESR_SMAAData; // x edge threshold, y relative depth threshold, z max search steps, w subpixel shift

#ifndef SMAA_RT_METRICS
#define SMAA_RT_METRICS TESR_SMAAResolution
#endif

#define SMAA_HLSL_3

// In place of SMAA_PRESET_ULTRA, which hardcodes these four. The threshold and the search
// step count are used as plain values at their sites - a step() in the edge detection and a
// mad() in the blending weight vertex shader - so they can come from a constant and be tuned
// without a rebuild. The diagonal count indexes a for loop and has to stay compile time.
// Values match ULTRA, so the defaults are unchanged.
#define SMAA_THRESHOLD             (TESR_SMAAData.x)
#define SMAA_MAX_SEARCH_STEPS      (TESR_SMAAData.z)
#define SMAA_MAX_SEARCH_STEPS_DIAG 16
#define SMAA_CORNER_ROUNDING       25

#include "includes/SMAA.hlsl"

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; SRGBTEXTURE = FALSE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; SRGBTEXTURE = FALSE; };
sampler2D TESR_SMAA_Edges : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; SRGBTEXTURE = FALSE; };
sampler2D TESR_SMAA_Blend : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; SRGBTEXTURE = FALSE; };
sampler2D TESR_SMAA_AreaTex : register(s4) < string ResourceName = "Effects\SMAA_AreaTex.dds"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; SRGBTEXTURE = FALSE; };
sampler2D TESR_SMAA_SearchTex : register(s5) < string ResourceName = "Effects\SMAA_SearchTex.dds"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = POINT; SRGBTEXTURE = FALSE; };

/**
 * Function wrappers
 */
// Shifts the texture coordinate relative to the pixel being written, in texels. The quad these
// passes draw comes from the game and no NVR effect adjusts for the D3D9 half texel rule, so
// whether texel centres line up with pixel centres depends on the quad and, under DXVK, on the
// translation layer as well. SMAA is far more sensitive to that than a blur is: half a texel of
// slip means the edge found at one pixel is blended at its neighbour, which weakens the result
// and skews it in one direction. 0 leaves sampling exactly as it was.
#define SMAA_SUBPIXEL_SHIFT (TESR_SMAAData.w * TESR_SMAAResolution.xy)

void DX9_SMAAEdgeDetectionVS(inout float4 position : POSITION,
                             inout float2 texcoord : TEXCOORD0,
                             out float4 offset[3] : TEXCOORD1) {
    texcoord += SMAA_SUBPIXEL_SHIFT;
    SMAAEdgeDetectionVS(texcoord, offset);
}

void DX9_SMAABlendingWeightCalculationVS(inout float4 position : POSITION,
                                         inout float2 texcoord : TEXCOORD0,
                                         out float2 pixcoord : TEXCOORD1,
                                         out float4 offset[3] : TEXCOORD2) {
    texcoord += SMAA_SUBPIXEL_SHIFT;
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
}

void DX9_SMAANeighborhoodBlendingVS(inout float4 position : POSITION,
                                    inout float2 texcoord : TEXCOORD0,
                                    out float4 offset : TEXCOORD1) {
    texcoord += SMAA_SUBPIXEL_SHIFT;
    SMAANeighborhoodBlendingVS(texcoord, offset);
}


float4 DX9_SMAALumaEdgeDetectionPS(float4 position : SV_POSITION,
                                   float2 texcoord : TEXCOORD0,
                                   float4 offset[3] : TEXCOORD1) : COLOR {
    return float4(SMAALumaEdgeDetectionPS(texcoord, offset, TESR_RenderedBuffer), 0.0, 0.0);
}

float4 DX9_SMAAColorEdgeDetectionPS(float4 position : SV_POSITION,
                                    float2 texcoord : TEXCOORD0,
                                    float4 offset[3] : TEXCOORD1) : COLOR {
    return float4(SMAAColorEdgeDetectionPS(texcoord, offset, TESR_RenderedBuffer), 0.0, 0.0);
}

float4 DX9_SMAADepthEdgeDetectionPS(float4 position : SV_POSITION,
                                    float2 texcoord : TEXCOORD0,
                                    float4 offset[3] : TEXCOORD1) : COLOR {
    // Not SMAADepthEdgeDetectionPS, because its threshold is absolute and this buffer is not.
    // CombineDepth stores viewZ/farZ, so the stock 0.1*SMAA_THRESHOLD works out as that fraction
    // of the FAR PLANE - hundreds of units at New Vegas draw distances - and almost nothing ever
    // registered as an edge. Comparing the difference against the centre depth instead is scale
    // invariant, so one setting behaves the same on a nearby crate and a distant ridge.
    float3 n = SMAAGatherNeighbours(texcoord, offset, TESR_DepthBuffer);
    float2 delta = abs(n.xx - float2(n.y, n.z));
    float2 edges = step(TESR_SMAAData.y * max(n.x, 0.000001), delta);

    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        discard;

    return float4(edges, 0.0, 0.0);
}

float4 DX9_SMAABlendingWeightCalculationPS(float4 position : SV_POSITION,
                                           float2 texcoord : TEXCOORD0,
                                           float2 pixcoord : TEXCOORD1,
                                           float4 offset[3] : TEXCOORD2) : COLOR {
    return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, TESR_SMAA_Edges, TESR_SMAA_AreaTex, TESR_SMAA_SearchTex, 0.0);
}

float4 DX9_SMAANeighborhoodBlendingPS(float4 position : SV_POSITION,
                                      float2 texcoord : TEXCOORD0,
                                      float4 offset : TEXCOORD1) : COLOR {
    return SMAANeighborhoodBlendingPS(texcoord, offset, TESR_RenderedBuffer, TESR_SMAA_Blend);
}

/**
 * Techniques.
 */
technique LumaEdgeDetection {
    pass LumaEdgeDetection {
        VertexShader = compile vs_3_0 DX9_SMAAEdgeDetectionVS();
        PixelShader = compile ps_3_0 DX9_SMAALumaEdgeDetectionPS();
        ZEnable = false;
        SRGBWriteEnable = false;
        AlphaBlendEnable = false;
        AlphaTestEnable = false;

        // We will be creating the stencil buffer for later usage.
        StencilEnable = true;
        StencilPass = REPLACE;
        StencilRef = 1;
    }
}

technique ColorEdgeDetection {
    pass ColorEdgeDetection {
        VertexShader = compile vs_3_0 DX9_SMAAEdgeDetectionVS();
        PixelShader = compile ps_3_0 DX9_SMAAColorEdgeDetectionPS();
        ZEnable = false;
        SRGBWriteEnable = false;
        AlphaBlendEnable = false;
        AlphaTestEnable = false;

        // We will be creating the stencil buffer for later usage.
        StencilEnable = true;
        StencilPass = REPLACE;
        StencilRef = 1;
    }
}

technique DepthEdgeDetection {
    pass DepthEdgeDetection {
        VertexShader = compile vs_3_0 DX9_SMAAEdgeDetectionVS();
        PixelShader = compile ps_3_0 DX9_SMAADepthEdgeDetectionPS();
        ZEnable = false;
        SRGBWriteEnable = false;
        AlphaBlendEnable = false;
        AlphaTestEnable = false;

        // We will be creating the stencil buffer for later usage.
        StencilEnable = true;
        StencilPass = REPLACE;
        StencilRef = 1;
    }
}

technique BlendWeightCalculation {
    pass BlendWeightCalculation {
        VertexShader = compile vs_3_0 DX9_SMAABlendingWeightCalculationVS();
        PixelShader = compile ps_3_0 DX9_SMAABlendingWeightCalculationPS();
        ZEnable = false;
        SRGBWriteEnable = false;
        AlphaBlendEnable = false;
        AlphaTestEnable = false;

        // Here we want to process only marked pixels.
        StencilEnable = true;
        StencilPass = KEEP;
        StencilFunc = EQUAL;
        StencilRef = 1;
    }
}

technique NeighborhoodBlending {
    pass NeighborhoodBlending {
        VertexShader = compile vs_3_0 DX9_SMAANeighborhoodBlendingVS();
        PixelShader = compile ps_3_0 DX9_SMAANeighborhoodBlendingPS();
        ZEnable = false;
        SRGBWriteEnable = false;
        AlphaBlendEnable = false;
        AlphaTestEnable = false;

        // Here we want to process all the pixels.
        StencilEnable = false;
    }
}
