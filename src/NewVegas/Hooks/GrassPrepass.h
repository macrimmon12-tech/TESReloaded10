#pragma once

// Depth prepass for grass: every grass draw is issued twice.
//
// Grass is a pile of alpha-tested cards. A pixel that can be discarded only writes depth once its
// shader has run, so blades behind other blades are not rejected before shading, and dense grass
// lights the same screen pixel once for every card layered over it. Here the first draw writes
// depth alone, through a pixel shader that does nothing but the grass shader's own alpha cut. The
// second draws the real grass shader with depth writes off and the depth test left as it was (a
// strict test widened to include equal), so only the front-most blade at each pixel passes the
// test, and it is rejected before the lighting runs: each pixel is lit about once.
//
// The cost is on the CPU and in vertex work: grass draw calls and grass vertices double. Worth it
// where grass fills the screen; [_Shaders.Grass.Main] DepthPrepass turns it off.
//
// Hooked at the device (IDirect3DDevice9 DrawPrimitive / DrawIndexedPrimitive), gated by
// OnSetShaders: only a pass whose vertex and pixel shaders are both NVR's replaced grass
// shaders is doubled, and only while those are still what the device has bound.
namespace GrassPrepass {
	void BeginFrame();															// once per frame, before the scene renders
	void SetEnabled(bool abEnabled);											// every frame, from GrassShaders::DepthPrepass
	void OnSetShaders(NiD3DVertexShaderEx* apVertexShader, NiD3DPixelShaderEx* apPixelShader);	// after BSShader::SetShaders
}
