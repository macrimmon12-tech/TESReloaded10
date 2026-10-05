#include "CombineDepth.h"


void CombineDepthEffect::UpdateConstants() {
	NiCamera* Camera = WorldSceneGraph->camera;

	if (!johnnyguitar || !JG_GetClipDist) {
		Constants.viewNearZ = Camera->Frustum.Near;
	}
	else {
		float nearZ = JG_GetClipDist();

		Constants.viewNearZ = nearZ ? max(nearZ, 0.3)  : Camera->Frustum.Near;
	}
}

void CombineDepthEffect::RegisterTextures() {
	// Effects recompute the post-projection depth from the linear channel (Depth.hlsl
	// projectedDepthFromLinear), so a single 32-bit channel carries everything they read.
	const D3DFORMAT format = TheSettingManager->SettingsMain.Main.SlimDepthBuffer ? D3DFMT_R32F : D3DFMT_G32R32F;
	TheTextureManager->InitTexture("TESR_DepthBuffer", &Textures.CombinedDepthTexture, &Textures.CombinedDepthSurface, TheRenderManager->width, TheRenderManager->height, format);
	Logger::Log("Depth buffer: %s.", format == D3DFMT_R32F ? "R32F (single channel)" : "G32R32F");
}

// ---- One-off check of the merged draw ----
// Every call in the merged path returns success, yet on a GTX 1070 (native D3D9) its targets stayed stale:
// the retail runtime returns S_OK even for a draw the driver rejects. So the first time the merged path
// runs in a session, the normals target is filled with a marker colour, and its centre pixel is read back
// before and after the draw (a one-off stall of a few milliseconds). Marker still there afterwards = the
// draw never ran; the merged path then switches itself off (separate passes until restart). The render
// states the draw ran with are logged too, to show which D3D9 rule it may have broken.
static const char* SurfaceFormatName(D3DFORMAT format) {
	switch (format) {
	case D3DFMT_R32F: return "R32F";
	case D3DFMT_G32R32F: return "G32R32F";
	case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
	case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
	case D3DFMT_A8R8G8B8: return "A8R8G8B8";
	case D3DFMT_X8R8G8B8: return "X8R8G8B8";
	case D3DFMT_D24S8: return "D24S8";
	case D3DFMT_D24X8: return "D24X8";
	case D3DFMT_D16: return "D16";
	case (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'): return "INTZ";
	default: return "other";
	}
}

// Centre pixel of an A16B16G16R16F render target, through a 1x1 copy (the GPU finishes the frame so far).
static bool ReadCentreTexel(IDirect3DDevice9* Device, IDirect3DSurface9* surface, const D3DSURFACE_DESC& desc, float texel[4]) {
	if (desc.Format != D3DFMT_A16B16G16R16F) return false;
	IDirect3DSurface9* probe = nullptr;
	IDirect3DSurface9* readable = nullptr;
	bool read = false;
	if (SUCCEEDED(Device->CreateRenderTarget(1, 1, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &probe, NULL)) &&
		SUCCEEDED(Device->CreateOffscreenPlainSurface(1, 1, desc.Format, D3DPOOL_SYSTEMMEM, &readable, NULL))) {
		const RECT centre = { (LONG)desc.Width / 2, (LONG)desc.Height / 2, (LONG)desc.Width / 2 + 1, (LONG)desc.Height / 2 + 1 };
		D3DLOCKED_RECT locked = {};
		if (SUCCEEDED(Device->StretchRect(surface, &centre, probe, NULL, D3DTEXF_NONE)) &&
			SUCCEEDED(Device->GetRenderTargetData(probe, readable)) &&
			SUCCEEDED(readable->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
			D3DXFloat16To32Array(texel, (const D3DXFLOAT16*)locked.pBits, 4);
			readable->UnlockRect();
			read = true;
		}
	}
	if (probe) probe->Release();
	if (readable) readable->Release();
	return read;
}

static bool IsMarker(const float texel[4]) {
	// D3DCOLOR_ARGB(0x40, 0x20, 0x10, 0x08) converted to floats; the half-float rounding is far below 0.002.
	const float marker[4] = { 0x20 / 255.0f, 0x10 / 255.0f, 0x08 / 255.0f, 0x40 / 255.0f };
	for (int i = 0; i < 4; ++i)
		if (fabsf(texel[i] - marker[i]) > 0.002f) return false;
	return true;
}

static void LogMergedState(IDirect3DDevice9* Device, const D3DCAPS9& caps, const D3DSURFACE_DESC& depthDesc, const D3DSURFACE_DESC& normalsDesc, IDirect3DSurface9* depthStencil) {
	const D3DRENDERSTATETYPE states[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
		D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1 };
	DWORD value[10] = {};
	for (int i = 0; i < 10; ++i)
		if (FAILED(Device->GetRenderState(states[i], &value[i]))) value[i] = 0xFFFFFFFF;
	D3DVIEWPORT9 viewport = {};
	Device->GetViewport(&viewport);
	D3DSURFACE_DESC ds = {};
	if (depthStencil) depthStencil->GetDesc(&ds);
	Logger::Log("Merged depth/normals state: RT0 %ux%u %s, RT1 %ux%u %s, game depth-stencil (unbound for the draw) %ux%u %s multisample %u | "
		"caps: %lu targets, independent bit depths %d, post-shader blending %d, independent write masks %d",
		depthDesc.Width, depthDesc.Height, SurfaceFormatName(depthDesc.Format), normalsDesc.Width, normalsDesc.Height, SurfaceFormatName(normalsDesc.Format),
		ds.Width, ds.Height, depthStencil ? SurfaceFormatName(ds.Format) : "none", (unsigned)ds.MultiSampleType, caps.NumSimultaneousRTs,
		(caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) ? 1 : 0, (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING) ? 1 : 0,
		(caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS) ? 1 : 0);
	Logger::Log("Merged depth/normals state: z %lu, z write %lu, stencil %lu, alpha test %lu, blend %lu, fog %lu, sRGB write %lu, scissor %lu, "
		"write mask RT0 %lx RT1 %lx, viewport %lu,%lu %lux%lu",
		value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7], value[8], value[9],
		viewport.X, viewport.Y, viewport.Width, viewport.Height);
}

bool CombineDepthEffect::RenderWithNormals(IDirect3DDevice9* Device, IDirect3DSurface9* NormalsSurface) {
	if (mergedNormalsFailed || TheSettingManager->SettingsMain.Main.DisableMergedNormals ||
		!Enabled || !Effect || !ShouldRender() || !NormalsSurface || !Textures.CombinedDepthSurface)
		return false;
	D3DXHANDLE technique = Effect->GetTechniqueByName("DepthNormals");
	D3DSURFACE_DESC depthDesc = {}, normalsDesc = {};
	D3DCAPS9 caps = {};
	if (!technique || FAILED(Device->GetDeviceCaps(&caps)) || caps.NumSimultaneousRTs < 2 ||
		FAILED(Textures.CombinedDepthSurface->GetDesc(&depthDesc)) || FAILED(NormalsSurface->GetDesc(&normalsDesc)) ||
		depthDesc.Width != normalsDesc.Width || depthDesc.Height != normalsDesc.Height) {
		mergedNormalsFailed = true;
		return false;
	}
	// R32F depth is 32 bits per pixel, the normals are 64: mixing sizes needs this cap.
	const bool sameBitDepth = (depthDesc.Format == D3DFMT_G32R32F) == (normalsDesc.Format == D3DFMT_A16B16G16R16F);
	if (!sameBitDepth && !(caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS)) {
		mergedNormalsFailed = true;
		Logger::Log("Merged depth/normals unavailable: MRT with different bit depths not supported.");
		return false;
	}

	auto timer = TimeLogger();
	static bool checked = false;
	const bool check = !checked;
	checked = true;
	float before[4] = {}, after[4] = {};
	bool haveBefore = false;
	if (check && SUCCEEDED(Device->ColorFill(NormalsSurface, NULL, D3DCOLOR_ARGB(0x40, 0x20, 0x10, 0x08))))
		haveBefore = ReadCentreTexel(Device, NormalsSurface, normalsDesc, before);

	DWORD oldWriteMask1 = 0xF;
	Device->GetRenderState(D3DRS_COLORWRITEENABLE1, &oldWriteMask1);
	// With the game's depth-stencil still bound, the two-target draw was silently dropped on a
	// GTX 1070 (native D3D9). The fog MRT pass, which works, unbinds it first; do the same.
	IDirect3DSurface9* depthStencil = nullptr;
	Device->GetDepthStencilSurface(&depthStencil);
	Device->SetDepthStencilSurface(nullptr);
	HRESULT result = Device->SetRenderTarget(0, Textures.CombinedDepthSurface);
	if (SUCCEEDED(result)) result = Device->SetRenderTarget(1, NormalsSurface);
	if (SUCCEEDED(result)) result = Device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
	if (SUCCEEDED(result)) result = Effect->SetTechnique(technique);
	if (SUCCEEDED(result)) {
		SetCT();
		UINT passes = 0;
		result = Effect->Begin(&passes, 0);
		if (SUCCEEDED(result)) {
			result = Effect->BeginPass(0);
			if (SUCCEEDED(result)) {
				if (check) LogMergedState(Device, caps, depthDesc, normalsDesc, depthStencil);
				result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
				Effect->EndPass();
			}
			Effect->End();
		}
	}
	Device->SetRenderTarget(1, nullptr);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE1, oldWriteMask1);
	Device->SetDepthStencilSurface(depthStencil);
	if (depthStencil) depthStencil->Release();
	if (FAILED(result)) {
		mergedNormalsFailed = true;
		Logger::Log("Merged depth/normals failed (%08lx); using separate passes until restart.", result);
		return false;
	}
	if (check) {
		const bool haveAfter = haveBefore && ReadCentreTexel(Device, NormalsSurface, normalsDesc, after);
		if (!haveAfter || !IsMarker(before)) {
			Logger::Log("Merged depth/normals check: inconclusive (marker %s, read back %s).",
				haveBefore && IsMarker(before) ? "written" : "not written", haveAfter ? "ok" : "failed");
		}
		else {
			const bool ran = !IsMarker(after);
			Logger::Log("Merged depth/normals check: normals centre pixel %.4f %.4f %.4f %.4f after the draw (marker %.4f %.4f %.4f %.4f): %s",
				after[0], after[1], after[2], after[3], before[0], before[1], before[2], before[3],
				ran ? "the two-target draw RAN." : "the two-target draw was DROPPED by the driver; using separate passes until restart.");
			if (!ran) {
				mergedNormalsFailed = true;
				return false; // the caller renders depth and normals the usual way this frame
			}
		}
	}
	static bool reported = false;
	if (!reported) {
		Logger::Log("Merged depth/normals active: one pass, two render targets.");
		reported = true;
	}
	renderTime = timer.LogTime("CombineDepth::RenderWithNormals");
	return true;
}