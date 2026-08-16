#include "MaterialPass.h"
#include <d3dx9shader.h>

namespace MaterialPass {
	// Everything this pass is configured by lives on the Flashlight effect, because it is
	// the same light: the cone, the near fade and the hotspot limit have to match the
	// screen space pass exactly or the two disagree along the edge of the beam.
	static FlashlightEffect::FlashlightSettingsStruct& FL() {
		return TheShaderManager->Effects.Flashlight->Settings;
	}

	static constexpr UInt32 kRTTI_NiNode = 0x11F4428;
	static constexpr UInt32 kRTTI_NiGeometry = 0x11F4ACC;
	static constexpr UInt32 kRTTI_NiSwitchNode = 0x11F5EB4;
	static constexpr UInt32 kRenderTriShapeAlt = 0xE745A0;
	static constexpr UInt32 kRenderTriStripsAlt = 0xE74840;
	static constexpr UInt32 kSavedStreamCount = 8;
	static constexpr UInt32 kSavedTextureCount = 2;
	static constexpr UInt32 kSavedSamplerStateCount = 5;

	static const char* kVertexShaderSource =
		"row_major float4x4 gWorldViewProj : register(c0);\n"
		"row_major float4x4 gWorld : register(c4);\n"
		"struct VS_IN { float4 pos : POSITION0; float3 normal : NORMAL0; float4 uv : TEXCOORD0; };\n"
		"struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 worldRel : TEXCOORD1; float3 normalRel : TEXCOORD2; };\n"
		"VS_OUT main(VS_IN IN) {\n"
		"    VS_OUT OUT;\n"
		"    float4 localPos = float4(IN.pos.xyz, 1.0);\n"
		"    float4 worldPos = mul(localPos, gWorld);\n"
		"    OUT.pos = mul(localPos, gWorldViewProj);\n"
		"    OUT.uv = IN.uv.xy;\n"
		"    OUT.worldRel = worldPos.xyz;\n"
		"    OUT.normalRel = normalize(mul(float4(IN.normal.xyz, 0.0), gWorld).xyz);\n"
		"    return OUT;\n"
		"}\n";

	static const char* kPixelShaderSource =
		"float4 gLightColorIntensity : register(c0);\n"
		"float4 gLightDirAngle : register(c1);\n"
		"float4 gTuning : register(c2);\n"
		"float4 gLightPosRadius : register(c3);\n"
		"float4 gSpecular : register(c4);\n"
		"float4 gSpecularTuning : register(c5);\n"
		"float4 gHotspot : register(c6);\n"
		"sampler2D DiffuseMap : register(s0);\n"
		"struct PS_IN { float2 uv : TEXCOORD0; float3 worldRel : TEXCOORD1; float3 normalRel : TEXCOORD2; };\n"
		"float invlerp(float a, float b, float v) { return saturate((v - a) / max(0.0001, b - a)); }\n"
		"float luma(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }\n"
		"float4 main(PS_IN IN) : COLOR0 {\n"
		"    float debugStrength = 4.0 * max(gLightColorIntensity.w, 0.25);\n"
		"    if (gTuning.y > 0.5 && gTuning.y < 1.5) return float4(debugStrength, 0.0, 0.0, 1.0);\n"
		"    float4 base = tex2D(DiffuseMap, IN.uv);\n"
		"    float3 toLight = gLightPosRadius.xyz - IN.worldRel;\n"
		"    float distToLight = length(toLight);\n"
		"    float3 lightVector = toLight / max(distToLight, 0.001);\n"
		"    float radius = max(gLightPosRadius.w, 1.0);\n"
		"    float s = saturate((distToLight / radius) * (distToLight / radius));\n"
		"    float atten = saturate(((1.0 - s) * (1.0 - s)) / (1.0 + 5.0 * s));\n"
		"    float angle = max(1.0, gLightDirAngle.w);\n"
		"    float angleCosMax = cos(radians(angle));\n"
		"    float angleCosMin = cos(radians(angle * 0.5));\n"
		"    float cone = pow(invlerp(angleCosMax, angleCosMin, dot(normalize(gLightDirAngle.xyz), -lightVector)), 2.0);\n"
		"    float nearFade = (gTuning.x > 0.0) ? smoothstep(0.0, gTuning.x, distToLight) : 1.0;\n"
		"    float contribution = atten * cone * nearFade;\n"
		"    float3 normal = normalize(IN.normalRel);\n"
		"    float diffuse = saturate(dot(normal, lightVector));\n"
		"    float3 viewVector = normalize(-IN.worldRel);\n"
		"    float3 halfVector = normalize(lightVector + viewVector);\n"
		"    float nDotH = saturate(dot(normal, halfVector));\n"
		"    float gloss = smoothstep(0.45, 0.95, saturate(gSpecular.z));\n"
		"    float specPower = max(gSpecularTuning.x, gSpecular.y * 2.0);\n"
		"    float specExponent = max(specPower / 32.0, 1.0);\n"
		"    float specLobe = pow(saturate((nDotH - 0.72) / 0.28), specExponent);\n"
		"    float specular = gSpecular.x * gloss * specLobe;\n"
		"    specular *= (diffuse <= 0.2) ? saturate(diffuse + 0.5) : 1.0;\n"
		"    float3 diffuseLight = base.rgb * diffuse * gLightColorIntensity.rgb * gLightColorIntensity.w * contribution;\n"
		"    float specularMask = saturate(specular);\n"
		"    float3 specularLight = saturate(specularMask * contribution * gLightColorIntensity.rgb);\n"
		"    if (gTuning.y > 6.5) return float4(saturate(specLobe * 8.0), gloss, saturate(specLobe * gloss * 16.0), 1.0);\n"
		"    if (gTuning.y > 5.5) return float4(0.0, 0.0, 0.0, 1.0);\n"
		"    if (gTuning.y > 4.5) return float4(pow(nDotH, 16.0) * debugStrength * contribution, pow(nDotH, 16.0) * debugStrength * contribution, pow(nDotH, 16.0) * debugStrength * contribution, 1.0);\n"
		"    if (gTuning.y > 3.5) return float4(0.0, 0.0, 0.0, 1.0);\n"
		"    if (gTuning.y > 2.5) return float4(specularLight * 4.0, 1.0);\n"
		"    if (gTuning.y > 1.5) return float4(0.0, 0.0, debugStrength * contribution, 1.0);\n"
		"    float3 outLight = diffuseLight + specularLight;\n"
		"    if (gHotspot.x > 0.0) {\n"
		"        float limit = 1.0 / gHotspot.x;\n"
		"        float outLum = luma(outLight);\n"
		"        outLight *= limit / max(limit, outLum);\n"
		"    }\n"
		"    return float4(outLight, 1.0);\n"
		"}\n";

	static const char* kNormalMapVertexShaderSource =
		"row_major float4x4 gWorldViewProj : register(c0);\n"
		"row_major float4x4 gWorld : register(c4);\n"
		"struct VS_IN { float4 pos : POSITION0; float3 tangent : TANGENT0; float3 binormal : BINORMAL0; float3 normal : NORMAL0; float4 uv : TEXCOORD0; };\n"
		"struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 worldRel : TEXCOORD1; float3 tangentRel : TEXCOORD2; float3 binormalRel : TEXCOORD3; float3 normalRel : TEXCOORD4; };\n"
		"VS_OUT main(VS_IN IN) {\n"
		"    VS_OUT OUT;\n"
		"    float4 localPos = float4(IN.pos.xyz, 1.0);\n"
		"    float4 worldPos = mul(localPos, gWorld);\n"
		"    OUT.pos = mul(localPos, gWorldViewProj);\n"
		"    OUT.uv = IN.uv.xy;\n"
		"    OUT.worldRel = worldPos.xyz;\n"
		"    OUT.tangentRel = normalize(mul(float4(IN.tangent.xyz, 0.0), gWorld).xyz);\n"
		"    OUT.binormalRel = normalize(mul(float4(IN.binormal.xyz, 0.0), gWorld).xyz);\n"
		"    OUT.normalRel = normalize(mul(float4(IN.normal.xyz, 0.0), gWorld).xyz);\n"
		"    return OUT;\n"
		"}\n";

	static const char* kNormalMapPixelShaderSource =
		"float4 gLightColorIntensity : register(c0);\n"
		"float4 gLightDirAngle : register(c1);\n"
		"float4 gTuning : register(c2);\n"
		"float4 gLightPosRadius : register(c3);\n"
		"float4 gSpecular : register(c4);\n"
		"float4 gSpecularTuning : register(c5);\n"
		"float4 gHotspot : register(c6);\n"
		"sampler2D DiffuseMap : register(s0);\n"
		"sampler2D NormalMap : register(s1);\n"
		"struct PS_IN { float2 uv : TEXCOORD0; float3 worldRel : TEXCOORD1; float3 tangentRel : TEXCOORD2; float3 binormalRel : TEXCOORD3; float3 normalRel : TEXCOORD4; };\n"
		"float invlerp(float a, float b, float v) { return saturate((v - a) / max(0.0001, b - a)); }\n"
		"float luma(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }\n"
		"float4 main(PS_IN IN) : COLOR0 {\n"
		"    float debugStrength = 4.0 * max(gLightColorIntensity.w, 0.25);\n"
		"    if (gTuning.y > 0.5 && gTuning.y < 1.5) return float4(debugStrength, 0.0, 0.0, 1.0);\n"
		"    float4 base = tex2D(DiffuseMap, IN.uv);\n"
		"    float3 toLight = gLightPosRadius.xyz - IN.worldRel;\n"
		"    float distToLight = length(toLight);\n"
		"    float3 lightVector = toLight / max(distToLight, 0.001);\n"
		"    float radius = max(gLightPosRadius.w, 1.0);\n"
		"    float s = saturate((distToLight / radius) * (distToLight / radius));\n"
		"    float atten = saturate(((1.0 - s) * (1.0 - s)) / (1.0 + 5.0 * s));\n"
		"    float angle = max(1.0, gLightDirAngle.w);\n"
		"    float angleCosMax = cos(radians(angle));\n"
		"    float angleCosMin = cos(radians(angle * 0.5));\n"
		"    float cone = pow(invlerp(angleCosMax, angleCosMin, dot(normalize(gLightDirAngle.xyz), -lightVector)), 2.0);\n"
		"    float nearFade = (gTuning.x > 0.0) ? smoothstep(0.0, gTuning.x, distToLight) : 1.0;\n"
		"    float contribution = atten * cone * nearFade;\n"
		"    float3 tangent = normalize(IN.tangentRel);\n"
		"    float3 binormal = normalize(IN.binormalRel);\n"
		"    float3 normal = normalize(IN.normalRel);\n"
		"    float4 normalSample = tex2D(NormalMap, IN.uv);\n"
		"    float3 mapNormal = normalSample.xyz * 2.0 - 1.0;\n"
		"    mapNormal.xy *= max(gSpecular.w, 0.0);\n"
		"    mapNormal = normalize(mapNormal);\n"
		"    float3 worldNormal = normalize(mapNormal.x * tangent + mapNormal.y * binormal + mapNormal.z * normal);\n"
		"    float vertexDiffuse = saturate(dot(normal, lightVector));\n"
		"    float diffuse = saturate(dot(worldNormal, lightVector));\n"
		"    float3 viewVector = normalize(-IN.worldRel);\n"
		"    float3 halfVector = normalize(lightVector + viewVector);\n"
		"    float glossInput = saturate(normalSample.a);\n"
		"    float gloss = smoothstep(0.45, 0.95, glossInput);\n"
		"    float nDotH = saturate(dot(worldNormal, halfVector));\n"
		"    float specPower = max(gSpecularTuning.x, gSpecular.y * 2.0);\n"
		"    float specExponent = max(specPower / 32.0, 1.0);\n"
		"    float specLobe = pow(saturate((nDotH - 0.72) / 0.28), specExponent);\n"
		"    float specular = gSpecular.x * gloss * specLobe;\n"
		"    specular *= (diffuse <= 0.2) ? saturate(diffuse + 0.5) : 1.0;\n"
		"    float3 diffuseLight = base.rgb * diffuse * gLightColorIntensity.rgb * gLightColorIntensity.w * contribution;\n"
		"    float specularMask = saturate(specular);\n"
		"    float3 specularLight = saturate(specularMask * contribution * gLightColorIntensity.rgb);\n"
		"    if (gTuning.y > 6.5) return float4(saturate(specLobe * 8.0), gloss, saturate(specLobe * gloss * 16.0), 1.0);\n"
		"    if (gTuning.y > 5.5) {\n"
		"        float normalDelta = diffuse - vertexDiffuse;\n"
		"        float posDelta = saturate(normalDelta * 8.0) * debugStrength * contribution;\n"
		"        float negDelta = saturate(-normalDelta * 8.0) * debugStrength * contribution;\n"
		"        return float4(posDelta, negDelta, negDelta, 1.0);\n"
		"    }\n"
		"    if (gTuning.y > 4.5) return float4(pow(nDotH, 16.0) * debugStrength * contribution, pow(nDotH, 16.0) * debugStrength * contribution, pow(nDotH, 16.0) * debugStrength * contribution, 1.0);\n"
		"    if (gTuning.y > 3.5) return float4(glossInput * debugStrength * contribution, glossInput * debugStrength * contribution, glossInput * debugStrength * contribution, 1.0);\n"
		"    if (gTuning.y > 2.5) return float4(specularLight * 4.0, 1.0);\n"
		"    if (gTuning.y > 1.5) return float4(0.0, debugStrength * contribution, 0.0, 1.0);\n"
		"    float3 outLight = diffuseLight + specularLight;\n"
		"    if (gHotspot.x > 0.0) {\n"
		"        float limit = 1.0 / gHotspot.x;\n"
		"        float outLum = luma(outLight);\n"
		"        outLight *= limit / max(limit, outLum);\n"
		"    }\n"
		"    return float4(outLight, 1.0);\n"
		"}\n";

	struct RenderItem {
		NiGeometry* geometry;
		IDirect3DBaseTexture9* diffuse;
		IDirect3DBaseTexture9* normalMap;
		float specularStrength;
		float specularPower;
		float fallbackGloss;
		bool useNormalMap;
	};

	struct StreamState {
		IDirect3DVertexBuffer9* buffer;
		UINT offset;
		UINT stride;
	};

	struct SavedDeviceState {
		IDirect3DVertexShader9* vertexShader;
		IDirect3DPixelShader9* pixelShader;
		IDirect3DVertexDeclaration9* vertexDeclaration;
		IDirect3DBaseTexture9* textures[kSavedTextureCount];
		IDirect3DIndexBuffer9* indexBuffer;
		StreamState streams[kSavedStreamCount];
		DWORD fvf;
		DWORD renderStates[10];
		DWORD samplerStates[kSavedTextureCount][kSavedSamplerStateCount];

		void Save(IDirect3DDevice9* apDevice) {
			vertexShader = nullptr;
			pixelShader = nullptr;
			vertexDeclaration = nullptr;
			indexBuffer = nullptr;
			fvf = 0;
			ZeroMemory(textures, sizeof(textures));
			ZeroMemory(streams, sizeof(streams));
			ZeroMemory(renderStates, sizeof(renderStates));
			ZeroMemory(samplerStates, sizeof(samplerStates));

			apDevice->GetVertexShader(&vertexShader);
			apDevice->GetPixelShader(&pixelShader);
			apDevice->GetVertexDeclaration(&vertexDeclaration);
			for (UInt32 i = 0; i < kSavedTextureCount; ++i)
				apDevice->GetTexture(i, &textures[i]);
			apDevice->GetIndices(&indexBuffer);
			apDevice->GetFVF(&fvf);
			for (UInt32 i = 0; i < kSavedStreamCount; ++i)
				apDevice->GetStreamSource(i, &streams[i].buffer, &streams[i].offset, &streams[i].stride);

			apDevice->GetRenderState(D3DRS_ZENABLE, &renderStates[0]);
			apDevice->GetRenderState(D3DRS_ZWRITEENABLE, &renderStates[1]);
			apDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, &renderStates[2]);
			apDevice->GetRenderState(D3DRS_SRCBLEND, &renderStates[3]);
			apDevice->GetRenderState(D3DRS_DESTBLEND, &renderStates[4]);
			apDevice->GetRenderState(D3DRS_ALPHATESTENABLE, &renderStates[5]);
			apDevice->GetRenderState(D3DRS_STENCILENABLE, &renderStates[6]);
			apDevice->GetRenderState(D3DRS_CULLMODE, &renderStates[7]);
			apDevice->GetRenderState(D3DRS_COLORWRITEENABLE, &renderStates[8]);
			apDevice->GetRenderState(D3DRS_FOGENABLE, &renderStates[9]);

			for (UInt32 i = 0; i < kSavedTextureCount; ++i) {
				apDevice->GetSamplerState(i, D3DSAMP_ADDRESSU, &samplerStates[i][0]);
				apDevice->GetSamplerState(i, D3DSAMP_ADDRESSV, &samplerStates[i][1]);
				apDevice->GetSamplerState(i, D3DSAMP_MAGFILTER, &samplerStates[i][2]);
				apDevice->GetSamplerState(i, D3DSAMP_MINFILTER, &samplerStates[i][3]);
				apDevice->GetSamplerState(i, D3DSAMP_MIPFILTER, &samplerStates[i][4]);
			}
		}

		void Restore(IDirect3DDevice9* apDevice) {
			NiDX9RenderState* renderState = TheRenderManager ? TheRenderManager->renderState : nullptr;
			if (renderState) {
				renderState->SetVertexShader(vertexShader, false);
				renderState->SetPixelShader(pixelShader, false);
				if (vertexDeclaration)
					renderState->SetVertexDeclaration(vertexDeclaration, false);
				else
					renderState->SetFVF(fvf, false);
				for (UInt32 i = 0; i < kSavedTextureCount; ++i)
					renderState->SetTexture(i, textures[i]);
				renderState->SetRenderState(D3DRS_ZENABLE, renderStates[0], RenderStateArgs);
				renderState->SetRenderState(D3DRS_ZWRITEENABLE, renderStates[1], RenderStateArgs);
				renderState->SetRenderState(D3DRS_ALPHABLENDENABLE, renderStates[2], RenderStateArgs);
				renderState->SetRenderState(D3DRS_SRCBLEND, renderStates[3], RenderStateArgs);
				renderState->SetRenderState(D3DRS_DESTBLEND, renderStates[4], RenderStateArgs);
				renderState->SetRenderState(D3DRS_ALPHATESTENABLE, renderStates[5], RenderStateArgs);
				renderState->SetRenderState(D3DRS_STENCILENABLE, renderStates[6], RenderStateArgs);
				renderState->SetRenderState(D3DRS_CULLMODE, renderStates[7], RenderStateArgs);
				renderState->SetRenderState(D3DRS_COLORWRITEENABLE, renderStates[8], RenderStateArgs);
				renderState->SetRenderState(D3DRS_FOGENABLE, renderStates[9], RenderStateArgs);
				for (UInt32 i = 0; i < kSavedTextureCount; ++i) {
					renderState->SetSamplerState(i, D3DSAMP_ADDRESSU, samplerStates[i][0], false);
					renderState->SetSamplerState(i, D3DSAMP_ADDRESSV, samplerStates[i][1], false);
					renderState->SetSamplerState(i, D3DSAMP_MAGFILTER, samplerStates[i][2], false);
					renderState->SetSamplerState(i, D3DSAMP_MINFILTER, samplerStates[i][3], false);
					renderState->SetSamplerState(i, D3DSAMP_MIPFILTER, samplerStates[i][4], false);
				}
			} else {
				apDevice->SetVertexShader(vertexShader);
				apDevice->SetPixelShader(pixelShader);
				if (vertexDeclaration)
					apDevice->SetVertexDeclaration(vertexDeclaration);
				else
					apDevice->SetFVF(fvf);
				for (UInt32 i = 0; i < kSavedTextureCount; ++i) {
					apDevice->SetTexture(i, textures[i]);
					apDevice->SetSamplerState(i, D3DSAMP_ADDRESSU, samplerStates[i][0]);
					apDevice->SetSamplerState(i, D3DSAMP_ADDRESSV, samplerStates[i][1]);
					apDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, samplerStates[i][2]);
					apDevice->SetSamplerState(i, D3DSAMP_MINFILTER, samplerStates[i][3]);
					apDevice->SetSamplerState(i, D3DSAMP_MIPFILTER, samplerStates[i][4]);
				}
			}

			apDevice->SetIndices(indexBuffer);
			for (UInt32 i = 0; i < kSavedStreamCount; ++i)
				apDevice->SetStreamSource(i, streams[i].buffer, streams[i].offset, streams[i].stride);

			if (vertexShader) vertexShader->Release();
			if (pixelShader) pixelShader->Release();
			if (vertexDeclaration) vertexDeclaration->Release();
			for (UInt32 i = 0; i < kSavedTextureCount; ++i) {
				if (textures[i])
					textures[i]->Release();
			}
			if (indexBuffer) indexBuffer->Release();
			for (UInt32 i = 0; i < kSavedStreamCount; ++i) {
				if (streams[i].buffer)
					streams[i].buffer->Release();
			}
		}
	};

	static bool g_renderActive = false;
	static std::vector<RenderItem> g_renderQueue;
	static IDirect3DVertexShader9* g_vertexShader = nullptr;
	static IDirect3DPixelShader9* g_pixelShader = nullptr;
	static IDirect3DVertexShader9* g_normalMapVertexShader = nullptr;
	static IDirect3DPixelShader9* g_normalMapPixelShader = nullptr;
	static bool g_shaderFailed = false;

	static bool IsNiNode(NiAVObject* apObject) {
		return apObject && CdeclCall<bool>(0x43B300, kRTTI_NiNode, apObject);
	}

	static bool IsGeometry(NiAVObject* apObject) {
		return apObject && CdeclCall<bool>(0x43B300, kRTTI_NiGeometry, apObject);
	}

	static bool IsSwitchNode(NiAVObject* apObject) {
		return apObject && CdeclCall<bool>(0x43B300, kRTTI_NiSwitchNode, apObject);
	}

	static bool IsLightingShadeType(NiShadeProperty::ShaderPropType aeType) {
		return aeType == NiShadeProperty::kProp_Lighting ||
			aeType == NiShadeProperty::kProp_PPLighting ||
			aeType == NiShadeProperty::kProp_Lighting30 ||
			aeType == NiShadeProperty::kProp_Hair ||
			aeType == NiShadeProperty::kProp_SpeedTreeBranch ||
			aeType == NiShadeProperty::kProp_SpeedTreeLeaf;
	}

	static bool HasBSShaderFlags(NiShadeProperty::ShaderPropType aeType) {
		return aeType == NiShadeProperty::kProp_Lighting ||
			aeType == NiShadeProperty::kProp_PPLighting ||
			aeType == NiShadeProperty::kProp_Lighting30 ||
			aeType == NiShadeProperty::kProp_Hair ||
			aeType == NiShadeProperty::kProp_NoLighting ||
			aeType == NiShadeProperty::kProp_DistantLOD ||
			aeType == NiShadeProperty::kProp_TallGrass ||
			aeType == NiShadeProperty::kProp_SpeedTreeBranch ||
			aeType == NiShadeProperty::kProp_SpeedTreeLeaf;
	}

	static NiShadeProperty* GetShadeProperty(NiGeometry* apGeometry) {
		return apGeometry ? apGeometry->propertyState.m_spShadeProperty : nullptr;
	}

	static IDirect3DBaseTexture9* GetTexture(BSShaderPPLightingProperty* apProperty, UInt32 auiIndex) {
		if (!apProperty || auiIndex >= 6 || !apProperty->ppTextures[auiIndex])
			return nullptr;
		NiSourceTexture* texture = *apProperty->ppTextures[auiIndex];
		if (!texture || !texture->rendererData)
			return nullptr;
		return texture->rendererData->dTexture;
	}

	static bool DeclarationHasUsage(IDirect3DVertexDeclaration9* apDeclaration, BYTE aucUsage) {
		if (!apDeclaration)
			return false;

		D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH];
		UINT count = MAXD3DDECLLENGTH;
		if (FAILED(apDeclaration->GetDeclaration(elements, &count)))
			return false;

		for (UINT i = 0; i < count; ++i) {
			if (elements[i].Stream == 0xFF)
				break;
			if (elements[i].Usage == aucUsage && elements[i].UsageIndex == 0)
				return true;
		}
		return false;
	}

	static bool HasTangentBasis(NiGeometry* apGeometry) {
		NiGeometryData* modelData = apGeometry ? apGeometry->geomData : nullptr;
		NiGeometryBufferData* bufferData = modelData ? modelData->m_pkBuffData : nullptr;
		if (!bufferData || !bufferData->VertexDeclaration)
			return false;
		return DeclarationHasUsage(bufferData->VertexDeclaration, D3DDECLUSAGE_TANGENT) &&
			DeclarationHasUsage(bufferData->VertexDeclaration, D3DDECLUSAGE_BINORMAL) &&
			DeclarationHasUsage(bufferData->VertexDeclaration, D3DDECLUSAGE_NORMAL);
	}

	static float ClampFloat(float afValue, float afMin, float afMax) {
		if (afValue < afMin)
			return afMin;
		if (afValue > afMax)
			return afMax;
		return afValue;
	}

	static float GetSpecularScale(NiGeometry* apGeometry) {
		NiMaterialProperty* material = apGeometry ? apGeometry->propertyState.m_spMaterialProperty : nullptr;
		if (!material)
			return 1.0f;

		float scale = material->spec.r;
		if (material->spec.g > scale)
			scale = material->spec.g;
		if (material->spec.b > scale)
			scale = material->spec.b;
		if (scale <= 0.001f)
			return 1.0f;
		return ClampFloat(scale, 0.0f, 4.0f);
	}

	static float GetSpecularPower(NiGeometry* apGeometry) {
		NiMaterialProperty* material = apGeometry ? apGeometry->propertyState.m_spMaterialProperty : nullptr;
		if (!material || material->fShine <= 0.0f)
			return 32.0f;
		return ClampFloat(material->fShine, 4.0f, 128.0f);
	}

	static bool IsWithinLight(NiAVObject* apObject) {
		if (!TheShaderManager || !apObject)
			return false;

		NiBound* bound = apObject->m_kWorldBound;
		const NiPoint3 center = bound ? bound->Center : apObject->m_worldTransform.pos;
		const float boundRadius = bound ? bound->Radius : 0.0f;
		const D3DXVECTOR4& light = TheShaderManager->SpotLightPosition[0];
		const float radius = light.w + boundRadius;
		const float dx = center.x - light.x;
		const float dy = center.y - light.y;
		const float dz = center.z - light.z;
		return (dx * dx + dy * dy + dz * dz) <= radius * radius;
	}

	static bool UsesAlphaBlendOrTest(NiGeometry* apGeometry) {
		NiAlphaProperty* alpha = apGeometry ? apGeometry->propertyState.m_spAlphaProperty : nullptr;
		if (!alpha)
			return false;
		return (alpha->flags & (NiAlphaProperty::ALPHA_BLEND_MASK | NiAlphaProperty::TEST_ENABLE_MASK)) != 0;
	}

	static bool ShouldQueueGeometry(
		NiGeometry* apGeometry,
		BSShaderPPLightingProperty* apProperty,
		IDirect3DBaseTexture9** apDiffuse) {
		if (!g_renderActive || !apGeometry || !apProperty || !apDiffuse)
			return false;
		if ((int)g_renderQueue.size() >= FL().MaterialLight.MaxGeometry)
			return false;
		if (apGeometry->skinInstance || UsesAlphaBlendOrTest(apGeometry))
			return false;
		if (apGeometry->m_flags & NiAVObject::APP_CULLED)
			return false;

		const char* name = apGeometry->m_pcName ? apGeometry->m_pcName : "";
		if (strstr(name, "Terrain LOD") == name)
			return false;

		NiGeometryData* modelData = apGeometry->geomData;
		if (!modelData || !modelData->m_pkBuffData || !modelData->m_pkBuffData->VertCount)
			return false;
		if (!IsWithinLight(apGeometry))
			return false;

		*apDiffuse = GetTexture(apProperty, 0);
		return *apDiffuse != nullptr;
	}

	static void QueueGeometry(
		NiGeometry* apGeometry,
		BSShaderPPLightingProperty* apProperty,
		IDirect3DBaseTexture9* apNormalMap,
		bool abSpecular,
		float afSpecularStrength,
		float afSpecularPower,
		bool abUseNormalMap) {
		IDirect3DBaseTexture9* diffuse = nullptr;
		if (!ShouldQueueGeometry(apGeometry, apProperty, &diffuse))
			return;
		const float specularStrength = abSpecular ? afSpecularStrength : 0.0f;
		g_renderQueue.push_back({ apGeometry, diffuse, abUseNormalMap ? apNormalMap : nullptr, specularStrength, afSpecularPower, 0.5f, abUseNormalMap && apNormalMap });
	}

	void CaptureGeometry(NiGeometry* apGeometry) {
		if (!g_renderActive || !apGeometry)
			return;
		if (!*(void**)apGeometry || !IsGeometry(static_cast<NiAVObject*>(apGeometry)))
			return;

		NiShadeProperty* shade = GetShadeProperty(apGeometry);
		if (!shade)
			return;

		const auto shadeType = shade->m_eShaderType;
		const bool skinned = apGeometry->skinInstance != nullptr;

		if (!HasBSShaderFlags(shadeType))
			return;

		auto* property = static_cast<BSShaderProperty*>(shade);
		const UInt32 flags0 = property->ulFlags[0];
		const UInt32 flags1 = property->ulFlags[1];
		const bool specular = (flags0 & BSShaderProperty::Specular) != 0;
		const bool parallax = (flags0 & (BSShaderProperty::Parallax_Shader | BSShaderProperty::Parallax_Occlusion)) != 0;
		const bool normalEligible = (flags1 & BSShaderProperty::Skip_Normal_Maps) == 0;
		const bool modelSpaceNormals = (flags0 & BSShaderProperty::Model_Space_Normals) != 0;
		const float specularStrength = specular ? GetSpecularScale(apGeometry) : 0.0f;
		const float specularPower = GetSpecularPower(apGeometry);
		bool tangentData = false;
		bool tangentBasis = false;
		bool normalTexture = false;
		bool useNormalMap = false;
		IDirect3DBaseTexture9* normalMap = nullptr;
		BSShaderPPLightingProperty* ppLighting = nullptr;
		UInt32 lightCount = 0;


		if (shadeType == NiShadeProperty::kProp_PPLighting) {
			ppLighting = static_cast<BSShaderPPLightingProperty*>(property);
			tangentData = ppLighting->spTangentSpaceData != nullptr;
			tangentBasis = HasTangentBasis(apGeometry);
			normalMap = GetTexture(ppLighting, 1);
			normalTexture = normalMap != nullptr;
			useNormalMap = normalEligible && !modelSpaceNormals && tangentBasis && normalTexture;
		}

		QueueGeometry(apGeometry, ppLighting, normalMap, specular, specularStrength, specularPower, useNormalMap);
	}

	static void CaptureObject(NiAVObject* apObject) {
		if (!apObject || !*(void**)apObject)
			return;
		if (apObject->m_flags & NiAVObject::APP_CULLED)
			return;
		//world bounds enclose children, so this prunes whole subtrees outside the
		//beam before any RTTI or property work
		if (!IsWithinLight(apObject))
			return;

		if (IsGeometry(apObject)) {
			CaptureGeometry(static_cast<NiGeometry*>(apObject));
			return;
		}

		if (!IsNiNode(apObject))
			return;

		auto* node = static_cast<NiNode*>(apObject);
		if (!node->m_children.data || node->m_children.numObjs == 0)
			return;

		if (IsSwitchNode(apObject)) {
			auto* switchNode = static_cast<NiSwitchNode*>(apObject);
			if (switchNode->m_iIndex < 0)
				return;
			UInt32 index = (UInt32)switchNode->m_iIndex;
			if (index < node->m_children.numObjs)
				CaptureObject(node->m_children.data[index]);
			return;
		}

		for (UInt32 i = 0; i < node->m_children.numObjs; ++i)
			CaptureObject(node->m_children.data[i]);
	}

	void CaptureScene(SceneGraph* apSceneGraph) {
		if (!g_renderActive || !apSceneGraph)
			return;

		CaptureObject(apSceneGraph);
	}

	void BeginFrame(bool abActive) {
		g_renderQueue.clear();
		g_renderActive = abActive && FL().MaterialLight.Enabled &&
			FL().MaterialLight.Intensity > 0.0f &&
			FL().MaterialLight.MaxGeometry > 0;
	}

	static bool CompileShader(const char* apSource, const char* apEntry, const char* apProfile, ID3DXBuffer** apCode) {
		ID3DXBuffer* errors = nullptr;
		HRESULT hr = D3DXCompileShader(apSource, (UINT)strlen(apSource), nullptr, nullptr, apEntry, apProfile, 0, apCode, &errors, nullptr);
		if (errors)
			errors->Release();
		return SUCCEEDED(hr);
	}

	static bool EnsureShaders(IDirect3DDevice9* apDevice) {
		if (g_vertexShader && g_pixelShader && g_normalMapVertexShader && g_normalMapPixelShader)
			return true;
		if (g_shaderFailed || !apDevice)
			return false;

		ID3DXBuffer* vsCode = nullptr;
		ID3DXBuffer* psCode = nullptr;
		ID3DXBuffer* normalVsCode = nullptr;
		ID3DXBuffer* normalPsCode = nullptr;
		if (!CompileShader(kVertexShaderSource, "main", "vs_3_0", &vsCode) ||
			!CompileShader(kPixelShaderSource, "main", "ps_3_0", &psCode) ||
			!CompileShader(kNormalMapVertexShaderSource, "main", "vs_3_0", &normalVsCode) ||
			!CompileShader(kNormalMapPixelShaderSource, "main", "ps_3_0", &normalPsCode)) {
			g_shaderFailed = true;
			if (vsCode) vsCode->Release();
			if (psCode) psCode->Release();
			if (normalVsCode) normalVsCode->Release();
			if (normalPsCode) normalPsCode->Release();
			return false;
		}

		HRESULT vs = apDevice->CreateVertexShader((const DWORD*)vsCode->GetBufferPointer(), &g_vertexShader);
		HRESULT ps = apDevice->CreatePixelShader((const DWORD*)psCode->GetBufferPointer(), &g_pixelShader);
		HRESULT normalVs = apDevice->CreateVertexShader((const DWORD*)normalVsCode->GetBufferPointer(), &g_normalMapVertexShader);
		HRESULT normalPs = apDevice->CreatePixelShader((const DWORD*)normalPsCode->GetBufferPointer(), &g_normalMapPixelShader);
		vsCode->Release();
		psCode->Release();
		normalVsCode->Release();
		normalPsCode->Release();

		if (FAILED(vs) || FAILED(ps) || FAILED(normalVs) || FAILED(normalPs)) {
			if (g_vertexShader) {
				g_vertexShader->Release();
				g_vertexShader = nullptr;
			}
			if (g_pixelShader) {
				g_pixelShader->Release();
				g_pixelShader = nullptr;
			}
			if (g_normalMapVertexShader) {
				g_normalMapVertexShader->Release();
				g_normalMapVertexShader = nullptr;
			}
			if (g_normalMapPixelShader) {
				g_normalMapPixelShader->Release();
				g_normalMapPixelShader = nullptr;
			}
			g_shaderFailed = true;
			return false;
		}

		return true;
	}

	static void ApplyRenderState() {
		NiDX9RenderState* renderState = TheRenderManager->renderState;
		renderState->SetVertexShader(g_vertexShader, false);
		renderState->SetPixelShader(g_pixelShader, false);
		renderState->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE, RenderStateArgs);
		renderState->SetRenderState(D3DRS_ZWRITEENABLE, D3DZB_FALSE, RenderStateArgs);
		renderState->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE, RenderStateArgs);
		renderState->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE, RenderStateArgs);
		renderState->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE, RenderStateArgs);
		renderState->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE, RenderStateArgs);
		renderState->SetRenderState(D3DRS_STENCILENABLE, FALSE, RenderStateArgs);
		renderState->SetRenderState(D3DRS_COLORWRITEENABLE, 15, RenderStateArgs);
		renderState->SetRenderState(D3DRS_FOGENABLE, FALSE, RenderStateArgs);
		renderState->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
		renderState->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
		renderState->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR, false);
		renderState->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR, false);
		renderState->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR, false);
		renderState->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
		renderState->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
		renderState->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR, false);
		renderState->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR, false);
		renderState->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR, false);
	}

	static void SetCullMode(NiGeometry* apGeometry) {
		NiStencilProperty* stencil = apGeometry ? apGeometry->propertyState.m_spStencilProperty : nullptr;
		if (stencil)
			TheRenderManager->renderState->SetCullMode(stencil->GetDrawMode());
		else
			TheRenderManager->renderState->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW, RenderStateArgs);
	}

	static bool DrawGeometryBuffer(NiGeometry* apGeometry) {
		if (!apGeometry || !apGeometry->geomData)
			return false;
		NiGeometryBufferData* data = apGeometry->geomData->m_pkBuffData;
		if (!data || !data->VertCount)
			return false;
		if (data->StreamCount > kSavedStreamCount || !data->VertexStride)
			return false;

		IDirect3DDevice9* device = TheRenderManager->device;
		for (UInt32 i = 0; i < data->StreamCount; ++i) {
			if (!data->VBChip || !data->VBChip[i] || !data->VBChip[i]->VB)
				return false;
			device->SetStreamSource(i, data->VBChip[i]->VB, 0, data->VertexStride[i]);
		}

		if (data->IB)
			device->SetIndices(data->IB);
		if (data->FVF)
			TheRenderManager->renderState->SetFVF(data->FVF, false);
		else if (data->VertexDeclaration)
			TheRenderManager->renderState->SetVertexDeclaration(data->VertexDeclaration, false);
		else
			return false;

		const UInt16 dirtyFlags = apGeometry->geomData->m_usDirtyFlags;
		if (apGeometry->geomData->IsStripsData())
			ThisCall(kRenderTriStripsAlt, NiDX9Renderer::GetSingleton(), apGeometry);
		else
			ThisCall(kRenderTriShapeAlt, NiDX9Renderer::GetSingleton(), apGeometry);
		apGeometry->geomData->m_usDirtyFlags = dirtyFlags;
		return true;
	}

	void RenderWorld() {
		if (!g_renderActive || g_renderQueue.empty() || !TheRenderManager || !TheShaderManager)
			return;

		IDirect3DDevice9* device = TheRenderManager->device;
		if (!EnsureShaders(device))
			return;

		SavedDeviceState saved{};
		saved.Save(device);
		ApplyRenderState();

		D3DXVECTOR4 lightPos = TheShaderManager->SpotLightPosition[0];
		lightPos.x -= TheRenderManager->CameraPosition.x;
		lightPos.y -= TheRenderManager->CameraPosition.y;
		lightPos.z -= TheRenderManager->CameraPosition.z;

		D3DXVECTOR4 lightDir = TheShaderManager->SpotLightDirection[0];
		D3DXVec3Normalize((D3DXVECTOR3*)&lightDir, (D3DXVECTOR3*)&lightDir);
		lightDir.w = TheShaderManager->SpotLightDirection[0].w;

		// Fold the light's own dimmer and the night boost into the colour, exactly as the
		// screen space pass does, so this pass tracks the flashlight instead of having to be
		// re-balanced against it every time the dimmer moves or the sun goes down. The
		// dimmer belongs in rgb rather than w because w only reaches the diffuse term -
		// the specular term reads rgb alone. MaterialLight Intensity stays in w, where it
		// is the diffuse strength of this pass alone, matching Specular for the highlight.
		const D3DXVECTOR4& sun = TheShaderManager->ShaderConst.sunColor;
		float sunLuma = 1.0f / max(0.05f, sun.x * 0.2126f + sun.y * 0.7152f + sun.z * 0.0722f);
		float scale = TheShaderManager->SpotLightColor[0].w * sunLuma;

		D3DXVECTOR4 lightColor = TheShaderManager->SpotLightColor[0];
		lightColor.x *= scale;
		lightColor.y *= scale;
		lightColor.z *= scale;
		lightColor.w = FL().MaterialLight.Intensity;
		D3DXVECTOR4 tuning(FL().NearFade, (float)FL().MaterialLight.DebugMode, 0.0f, 0.0f);

		device->SetPixelShaderConstantF(0, (float*)&lightColor, 1);
		device->SetPixelShaderConstantF(1, (float*)&lightDir, 1);
		device->SetPixelShaderConstantF(2, (float*)&tuning, 1);
		device->SetPixelShaderConstantF(3, (float*)&lightPos, 1);
		D3DXVECTOR4 specularTuning(FL().MaterialLight.SpecularPower, 0.0f, 0.0f, 0.0f);
		device->SetPixelShaderConstantF(5, (float*)&specularTuning, 1);
		D3DXVECTOR4 hotspot(FL().HotspotLimit, 0.0f, 0.0f, 0.0f);
		device->SetPixelShaderConstantF(6, (float*)&hotspot, 1);

		for (const RenderItem& item : g_renderQueue) {
			if (!item.geometry || !item.diffuse)
				continue;
			const bool useNormalMap = item.useNormalMap && item.normalMap;
			const float specularStrength = item.specularStrength * FL().MaterialLight.Specular;

			D3DXMATRIX world;
			TheRenderManager->CreateD3DMatrix(&world, &item.geometry->m_worldTransform);
			D3DXMATRIX worldViewProj = world * TheRenderManager->ViewProjMatrix;
			device->SetVertexShaderConstantF(0, (float*)&worldViewProj, 4);
			device->SetVertexShaderConstantF(4, (float*)&world, 4);

			D3DXVECTOR4 specular(specularStrength, item.specularPower, item.fallbackGloss, FL().MaterialLight.NormalStrength);
			device->SetPixelShaderConstantF(4, (float*)&specular, 1);

			TheRenderManager->renderState->SetVertexShader(useNormalMap ? g_normalMapVertexShader : g_vertexShader, false);
			TheRenderManager->renderState->SetPixelShader(useNormalMap ? g_normalMapPixelShader : g_pixelShader, false);
			TheRenderManager->renderState->SetTexture(0, item.diffuse);
			TheRenderManager->renderState->SetTexture(1, useNormalMap ? item.normalMap : nullptr);
			SetCullMode(item.geometry);
			DrawGeometryBuffer(item.geometry);
		}

		saved.Restore(device);
	}

}
