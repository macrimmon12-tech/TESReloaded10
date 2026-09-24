#include "GrassPrepass.h"
#include <d3dx9shader.h>

namespace GrassPrepass {
	// IDirect3DDevice9 vtable slots (d3d9.h declaration order): 81 DrawPrimitive, 82 DrawIndexedPrimitive.
	static constexpr UInt32 kSlotDrawPrimitive = 81;
	static constexpr UInt32 kSlotDrawIndexedPrimitive = 82;

	typedef HRESULT (STDMETHODCALLTYPE* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
	typedef HRESULT (STDMETHODCALLTYPE* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

	// The depth pass. It has to discard exactly the pixels the grass shader discards, so it repeats
	// GRASS23x000TMS.pso's alpha to the letter: same texture (s0), same interpolators with the same
	// centroid sampling, same formula, same early-out cut. The engine's alpha test state is left alone
	// and reads the same output alpha, so it applies to both draws alike. Colour writes are masked off;
	// the value never shows.
	// The cut is a branch around the discard, as in the grass shader, not clip(alpha - 1/255): fxc
	// folds that subtraction into the fade multiply as one mad on the unrounded product, which can
	// land a pixel sitting exactly on the threshold on the other side of it from the grass shader's
	// plain compare of the rounded alpha. Kept and dropped pixels would then differ between the two
	// draws. CI checks the compiled form.
	static const char* kDepthPixelShaderSource =
		"sampler2D DiffuseMap : register(s0);\n"
		"struct PS_INPUT {\n"
		"    float2 uv  : TEXCOORD0;\n"
		"    float4 sun : TEXCOORD5_centroid;\n"	// .w = distance fade
		"};\n"
		"float4 main(PS_INPUT IN) : COLOR0 {\n"
		"    float alpha = saturate(tex2D(DiffuseMap, IN.uv.xy).a * 1.75f) * IN.sun.w;\n"
		"    [branch]\n"
		"    if (alpha < 1.0f / 255.0f) clip(-1.0f);\n"
		"    return float4(0.0f, 0.0f, 0.0f, alpha);\n"
		"}\n";

	// Why a grass draw went through undoubled, each logged once.
	enum SkipReason {
		kSkip_DepthOff,			// no depth test: nothing to reject against
		kSkip_DepthWriteOff,	// depth writes already off: the prepass would write depth this draw never did
		kSkip_Blended,			// alpha blending: every layer contributes, so hiding all but the front one is wrong
		kSkip_ColorWriteOff,	// already a depth-only draw
		kSkip_DepthFunc,		// a depth test other than less / less-equal / greater / greater-equal
		kSkip_StateQuery,		// the device would not report its render state
		kSkip_PrepassFailed,	// the depth draw itself failed; drawn once, as normal
		kSkip_SecondTarget,		// a second render target bound (counted per pass, not per draw)
		kSkip_Count
	};
	static const char* kSkipNames[kSkip_Count] = {
		"depth test off", "depth writes off", "alpha blending on", "colour writes off",
		"unsupported depth test", "render state query failed", "depth draw failed",
		"second render target bound",
	};

	static IDirect3DDevice9* g_device = nullptr;
	static DrawPrimitive_t g_drawPrimitive = nullptr;
	static DrawIndexedPrimitive_t g_drawIndexedPrimitive = nullptr;
	static bool g_patched = false;
	static bool g_shaderFailed = false;
	static IDirect3DPixelShader9* g_depthPixelShader = nullptr;

	static bool g_enabled = false;
	// The grass shaders of the pass being drawn, set by OnSetShaders and cleared by the next pass
	// and at the start of every frame. Null outside a grass pass, which is what the draw hooks test first.
	static IDirect3DVertexShader9* g_grassVertexShader = nullptr;
	static IDirect3DPixelShader9* g_grassPixelShader = nullptr;

	static bool g_skipLogged[kSkip_Count] = {};
	static bool g_stateLogged = false;
	static bool g_reportPending = false;
	static UInt32 g_framePasses = 0;
	static UInt32 g_frameDoubled = 0;
	static UInt32 g_frameSkipped = 0;

	static bool CreateDepthShader(IDirect3DDevice9* apDevice) {
		if (g_depthPixelShader) return true;
		if (g_shaderFailed) return false;

		ID3DXBuffer* code = nullptr;
		ID3DXBuffer* errors = nullptr;
		HRESULT hr = D3DXCompileShader(kDepthPixelShaderSource, (UINT)strlen(kDepthPixelShaderSource), nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors, nullptr);
		if (FAILED(hr)) {
			Logger::Log("[GrassPrepass] depth shader COMPILE failed: %s", errors ? (const char*)errors->GetBufferPointer() : "no error text");
			if (errors) errors->Release();
			if (code) code->Release();
			g_shaderFailed = true;
			return false;
		}
		if (errors) errors->Release();

		hr = apDevice->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &g_depthPixelShader);
		code->Release();
		if (FAILED(hr)) {
			Logger::Log("[GrassPrepass] depth shader CREATE failed: 0x%08X", (UInt32)hr);
			g_depthPixelShader = nullptr;
			g_shaderFailed = true;
			return false;
		}
		return true;
	}

	static void LogSkip(SkipReason aeReason, DWORD aValue) {
		g_frameSkipped++;
		if (g_skipLogged[aeReason]) return;
		g_skipLogged[aeReason] = true;
		Logger::Log("[GrassPrepass] a grass draw was drawn once, not prepassed: %s (value %u). Further draws for this reason are not logged.", kSkipNames[aeReason], (UInt32)aValue);
	}

	// Only while a grass pass is current and the device still has its shaders bound. The second
	// check keeps anything else drawn before the next pass starts -- NVR's own draws through the
	// device, say -- from being mistaken for grass.
	static bool IsGrassDraw(IDirect3DDevice9* apDevice) {
		if (!g_grassPixelShader || apDevice != g_device) return false;

		IDirect3DPixelShader9* pixelShader = nullptr;
		IDirect3DVertexShader9* vertexShader = nullptr;
		apDevice->GetPixelShader(&pixelShader);
		apDevice->GetVertexShader(&vertexShader);
		if (pixelShader) pixelShader->Release();
		if (vertexShader) vertexShader->Release();
		return pixelShader == g_grassPixelShader && vertexShader == g_grassVertexShader;
	}

	// Issues aDraw twice as described in GrassPrepass.h, putting back every state it changes to the
	// value the device held, so NiDX9RenderState's cache stays in step with the device.
	template <typename Draw>
	static HRESULT DrawTwice(IDirect3DDevice9* apDevice, Draw aDraw) {
		DWORD zEnable = 0, zWrite = 0, zFunc = 0, blend = 0, colorWrite = 0, stencil = 0;
		if (FAILED(apDevice->GetRenderState(D3DRS_ZENABLE, &zEnable)) ||
			FAILED(apDevice->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite)) ||
			FAILED(apDevice->GetRenderState(D3DRS_ZFUNC, &zFunc)) ||
			FAILED(apDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) ||
			FAILED(apDevice->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite)) ||
			FAILED(apDevice->GetRenderState(D3DRS_STENCILENABLE, &stencil))) {
			LogSkip(kSkip_StateQuery, 0);
			return aDraw();
		}
		if (zEnable == D3DZB_FALSE) { LogSkip(kSkip_DepthOff, zEnable); return aDraw(); }
		if (!zWrite) { LogSkip(kSkip_DepthWriteOff, zWrite); return aDraw(); }
		if (blend) { LogSkip(kSkip_Blended, blend); return aDraw(); }
		if (!colorWrite) { LogSkip(kSkip_ColorWriteOff, colorWrite); return aDraw(); }

		// The shaded draw must pass where its depth equals what the prepass just wrote: a strict test
		// is widened to include equal. Either depth direction, so reversed depth works too.
		DWORD shadedFunc;
		switch (zFunc) {
			case D3DCMP_LESS:			shadedFunc = D3DCMP_LESSEQUAL; break;
			case D3DCMP_LESSEQUAL:		shadedFunc = D3DCMP_LESSEQUAL; break;
			case D3DCMP_GREATER:		shadedFunc = D3DCMP_GREATEREQUAL; break;
			case D3DCMP_GREATEREQUAL:	shadedFunc = D3DCMP_GREATEREQUAL; break;
			default: LogSkip(kSkip_DepthFunc, zFunc); return aDraw();
		}

		if (!g_stateLogged) {
			g_stateLogged = true;
			DWORD alphaTest = 0, alphaRef = 0, alphaFunc = 0;
			apDevice->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
			apDevice->GetRenderState(D3DRS_ALPHAREF, &alphaRef);
			apDevice->GetRenderState(D3DRS_ALPHAFUNC, &alphaFunc);
			Logger::Log("[GrassPrepass] first grass draw prepassed. Grass state: depth func %u, alpha test %u, alpha ref %u, alpha func %u, stencil %u",
				(UInt32)zFunc, (UInt32)alphaTest, (UInt32)alphaRef, (UInt32)alphaFunc, (UInt32)stencil);
		}

		// 1. Depth only. A stencil test stays on, so the depth pass is masked exactly as the shaded
		//    draw is, but every stencil operation keeps: whatever this draw writes to the stencil it
		//    writes once, in the shaded draw.
		static const D3DRENDERSTATETYPE kStencilOps[] = {
			D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS,
			D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS,
		};
		static constexpr UInt32 kStencilOpCount = sizeof(kStencilOps) / sizeof(kStencilOps[0]);
		DWORD stencilOps[kStencilOpCount] = {};
		if (stencil) {
			for (UInt32 i = 0; i < kStencilOpCount; i++) {
				apDevice->GetRenderState(kStencilOps[i], &stencilOps[i]);
				if (stencilOps[i] != D3DSTENCILOP_KEEP) apDevice->SetRenderState(kStencilOps[i], D3DSTENCILOP_KEEP);
			}
		}
		apDevice->SetPixelShader(g_depthPixelShader);
		apDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
		HRESULT hr = aDraw();
		apDevice->SetPixelShader(g_grassPixelShader);
		apDevice->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
		if (stencil) {
			for (UInt32 i = 0; i < kStencilOpCount; i++) {
				if (stencilOps[i] != D3DSTENCILOP_KEEP) apDevice->SetRenderState(kStencilOps[i], stencilOps[i]);
			}
		}
		if (FAILED(hr)) {
			LogSkip(kSkip_PrepassFailed, (DWORD)hr);
			return aDraw();
		}

		// 2. Shaded, against that depth. With depth writes off the pixel shader's discard no longer
		//    holds the depth test back until after shading, so hidden blades are rejected up front.
		apDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		if (shadedFunc != zFunc) apDevice->SetRenderState(D3DRS_ZFUNC, shadedFunc);
		hr = aDraw();
		apDevice->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
		if (shadedFunc != zFunc) apDevice->SetRenderState(D3DRS_ZFUNC, zFunc);

		g_frameDoubled++;
		return hr;
	}

	static HRESULT STDMETHODCALLTYPE HookedDrawPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE Type, UINT StartVertex, UINT PrimitiveCount) {
		if (!IsGrassDraw(This))
			return g_drawPrimitive(This, Type, StartVertex, PrimitiveCount);
		return DrawTwice(This, [&]() { return g_drawPrimitive(This, Type, StartVertex, PrimitiveCount); });
	}

	static HRESULT STDMETHODCALLTYPE HookedDrawIndexedPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount) {
		if (!IsGrassDraw(This))
			return g_drawIndexedPrimitive(This, Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
		return DrawTwice(This, [&]() { return g_drawIndexedPrimitive(This, Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount); });
	}

	// Once, ever, on the first grass pass drawn with the prepass on -- never at all while it stays
	// off. Once and not "if the slot isn't already mine", for the reason spelled out at
	// PatchMouseVTable in ImGuiManager.cpp: another mod hooking the same slots after us chains
	// under us correctly, and re-patching would capture its hook as our original and recurse.
	static bool Patch(IDirect3DDevice9* apDevice) {
		if (g_patched) return g_drawPrimitive && g_drawIndexedPrimitive;
		g_patched = true;

		void** vtable = *reinterpret_cast<void***>(apDevice);
		DWORD oldProtect;
		if (!VirtualProtect(&vtable[kSlotDrawPrimitive], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			Logger::Log("[GrassPrepass] could not unprotect the device vtable; prepass disabled");
			return false;
		}
		g_drawPrimitive = reinterpret_cast<DrawPrimitive_t>(vtable[kSlotDrawPrimitive]);
		g_drawIndexedPrimitive = reinterpret_cast<DrawIndexedPrimitive_t>(vtable[kSlotDrawIndexedPrimitive]);
		vtable[kSlotDrawPrimitive] = reinterpret_cast<void*>(HookedDrawPrimitive);
		vtable[kSlotDrawIndexedPrimitive] = reinterpret_cast<void*>(HookedDrawIndexedPrimitive);
		VirtualProtect(&vtable[kSlotDrawPrimitive], 2 * sizeof(void*), oldProtect, &oldProtect);
		g_device = apDevice;
		Logger::Log("[GrassPrepass] device draw calls hooked");
		return true;
	}

	void BeginFrame() {
		g_grassVertexShader = nullptr;
		g_grassPixelShader = nullptr;

		// One line of numbers for the first frame that drew grass after the prepass was switched on,
		// so the log shows it engaged and how many draws it doubled. Grass passes with no draws
		// counted at all would mean the grass is drawn through a call these hooks do not cover.
		if (g_reportPending && g_framePasses) {
			g_reportPending = false;
			Logger::Log("[GrassPrepass] frame report: %u grass passes, %u grass draws prepassed, %u drawn once", g_framePasses, g_frameDoubled, g_frameSkipped);
		}
		g_framePasses = 0;
		g_frameDoubled = 0;
		g_frameSkipped = 0;
	}

	void SetEnabled(bool abEnabled) {
		if (abEnabled && !g_enabled) g_reportPending = true;
		g_enabled = abEnabled;
		if (!abEnabled) {
			g_grassVertexShader = nullptr;
			g_grassPixelShader = nullptr;
		}
	}

	void OnSetShaders(NiD3DVertexShaderEx* apVertexShader, NiD3DPixelShaderEx* apPixelShader) {
		g_grassVertexShader = nullptr;
		g_grassPixelShader = nullptr;
		if (!g_enabled || !apVertexShader || !apPixelShader) return;

		// Both of the pass's shaders are grass, and both are NVR's: the depth shader repeats NVR's
		// grass pixel shader and reads NVR's grass vertex shader outputs. The pixel shader alone is
		// not enough, since it has also been seen drawing hair.
		if (!apVertexShader->Name || strncmp(apVertexShader->Name, "GRASS", 5)) return;
		if (!apPixelShader->Name || strncmp(apPixelShader->Name, "GRASS", 5)) return;
		if (!apVertexShader->ShaderHandle || (void*)apVertexShader->ShaderHandle == (void*)apVertexShader->ShaderHandleBackup) return;
		if (!apPixelShader->ShaderHandle || (void*)apPixelShader->ShaderHandle == (void*)apPixelShader->ShaderHandleBackup) return;

		IDirect3DDevice9* device = TheRenderManager->device;
		if (!device || !CreateDepthShader(device) || !Patch(device)) return;
		g_framePasses++;

		// With a second render target bound, masking colour on the first alone would let the depth
		// pass write the other. FNV draws grass to one target; if that ever changes, draw once.
		IDirect3DSurface9* secondTarget = nullptr;
		if (SUCCEEDED(device->GetRenderTarget(1, &secondTarget)) && secondTarget) {
			secondTarget->Release();
			LogSkip(kSkip_SecondTarget, 1);
			return;
		}

		g_grassVertexShader = apVertexShader->ShaderHandle;
		g_grassPixelShader = apPixelShader->ShaderHandle;
	}
}
