#pragma once

class ReShadeIntegration {
public:
	static void Initialize();
	static void Shutdown();
	static void RenderEffects(IDirect3DSurface9* renderTarget);
};
