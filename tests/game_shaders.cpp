// Game shader A/B. Compiles game shader templates from two Shaders folders the way the game does
// (ShaderRecord::LoadShader: D3DX43 preprocess, then D3DXCompileShader with flags 0), draws old and new over varied test
// scenes into a 32-bit float target on the GPU and requires every pixel to be bit-identical, checks that the scene has
// no NaNs and that the feature under test visibly changes it (so "identical" means something), then times both at
// 2560x1440 on full-screen draws.
//  - terrain: TerrainTemplate.hlsl, TEX_COUNT 1-7, several parallax settings.
//  - objects: ObjectTemplate.hlsl's multi-light pixel shaders SLS2029-2036 with 0-6 lights in use.
// Usage: build\shader-test\game_shaders.exe <old Shaders folder> <new Shaders folder> [terrain|objects]
//        game_shaders.exe --asm-template <template file> <ps_3_0|vs_3_0> <output file> [NAME=VALUE ...]
// tools\test-game-shaders.ps1 extracts the old folder from git HEAD and runs it.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char* what) {
	if (FAILED(hr)) { char text[200]; sprintf_s(text, "%s failed: %08lx", what, hr); throw std::runtime_error(text); }
}

static unsigned seed = 12345;
static float Random() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }

typedef std::vector<std::pair<std::string, std::string>> Defines;

// Same fixed defines as ShaderRecord::LoadShader. FORWARD_SHADOWS 0 for the pixel tests (no shadow atlas here); the
// code under test does not depend on it.
static ComPtr<ID3DXBuffer> Compile(const std::string& file, const char* profile, const Defines& defines, int forwardShadows,
	unsigned* slots = nullptr, std::string* disassembly = nullptr) {
	std::vector<D3DXMACRO> macros;
	for (const auto& d : defines) macros.push_back({ d.first.c_str(), d.second.c_str() });
	macros.push_back({ "REVERSED_DEPTH", "" });
	macros.push_back({ "FORWARD_SHADOWS", forwardShadows ? "1" : "0" });
	macros.push_back({ "SKYLIGHTING_MODE", "0" });
	macros.push_back({ "SHADOW_FIXED_MODE", "2" });
	macros.push_back({ NULL, NULL });
	ComPtr<ID3DXBuffer> source, errors, code;
	HRESULT hr = D3DXPreprocessShaderFromFileA(file.c_str(), macros.data(), NULL, &source, &errors);
	if (FAILED(hr)) throw std::runtime_error("preprocess " + file + ": " + (errors ? (const char*)errors->GetBufferPointer() : ""));
	errors.Reset();
	hr = D3DXCompileShader((const char*)source->GetBufferPointer(), source->GetBufferSize(), NULL, NULL, "main", profile, 0,
		&code, &errors, NULL);
	if (FAILED(hr)) throw std::runtime_error("compile " + file + ": " + (errors ? (const char*)errors->GetBufferPointer() : ""));
	if (slots || disassembly) {
		ComPtr<ID3DXBuffer> text;
		Check(D3DXDisassembleShader((const DWORD*)code->GetBufferPointer(), FALSE, NULL, &text), "disassemble");
		if (disassembly) *disassembly = (const char*)text->GetBufferPointer();
		if (slots) {
			*slots = 0;
			const char* found = strstr((const char*)text->GetBufferPointer(), "approximately ");
			if (found) sscanf_s(found, "approximately %u", slots);
		}
	}
	return code;
}

struct Vertex { float v[11][4]; };  // POSITION0, then TEXCOORD0-9 as declared in the vertex shaders below

// Terrain: TerrainTemplate's PS_INPUT.
static const char* TerrainVertexShader = R"(
struct VIn { float4 pos : POSITION0; float4 uv : TEXCOORD0; float4 color : TEXCOORD1; float4 lpos : TEXCOORD2; float4 t : TEXCOORD3;
             float4 b : TEXCOORD4; float4 n : TEXCOORD5; float4 b0 : TEXCOORD6; float4 b1 : TEXCOORD7; float4 proj : TEXCOORD8;
             float4 view : TEXCOORD9; };
struct VOut { float4 blend_0 : COLOR0; float4 blend_1 : COLOR1; float4 sPosition : POSITION; float2 uv : TEXCOORD0;
              float3 vertex_color : TEXCOORD1; float3 lPosition : TEXCOORD2; float3 tangent : TEXCOORD3; float3 binormal : TEXCOORD4;
              float3 normal : TEXCOORD5; float4 projectionPosition : TEXCOORD6; float3 viewPosition : TEXCOORD7; };
VOut main(VIn IN) {
    VOut OUT;
    OUT.blend_0 = IN.b0; OUT.blend_1 = IN.b1; OUT.sPosition = IN.pos; OUT.uv = IN.uv.xy; OUT.vertex_color = IN.color.xyz;
    OUT.lPosition = IN.lpos.xyz; OUT.tangent = IN.t.xyz; OUT.binormal = IN.b.xyz; OUT.normal = IN.n.xyz;
    OUT.projectionPosition = IN.proj; OUT.viewPosition = IN.view.xyz;
    return OUT;
})";

// Objects: COLOR0/1 and TEXCOORD0-7 passed straight through; each variant reads the ones it declares.
static const char* ObjectVertexShader = R"(
struct VIn { float4 pos : POSITION0; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;
             float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7; float4 c0 : TEXCOORD8;
             float4 c1 : TEXCOORD9; };
struct VOut { float4 c0 : COLOR0; float4 c1 : COLOR1; float4 pos : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;
              float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6;
              float4 t7 : TEXCOORD7; };
VOut main(VIn IN) {
    VOut OUT;
    OUT.c0 = IN.c0; OUT.c1 = IN.c1; OUT.pos = IN.pos; OUT.t0 = IN.t0; OUT.t1 = IN.t1; OUT.t2 = IN.t2; OUT.t3 = IN.t3;
    OUT.t4 = IN.t4; OUT.t5 = IN.t5; OUT.t6 = IN.t6; OUT.t7 = IN.t7;
    return OUT;
})";

struct Scene { std::vector<Vertex> vertices; std::vector<unsigned short> indices; };

static const int Grid = 65;

static void GridIndices(Scene& scene) {
	for (int j = 0; j + 1 < Grid; j++)
		for (int i = 0; i + 1 < Grid; i++) {
			const unsigned short a = (unsigned short)(j * Grid + i), b = a + 1, c = a + Grid, d = c + 1;
			scene.indices.insert(scene.indices.end(), { a, b, c, b, d, c });
		}
}

static void Set(Vertex& v, int k, float x, float y, float z, float w) { v.v[k][0] = x; v.v[k][1] = y; v.v[k][2] = z; v.v[k][3] = w; }

// Terrain: ground from 150 to about 2600 units away (parallax fades out at 2048), random blend weights (some exactly 0),
// gently bent normals.
static Scene TerrainScene(int texCount) {
	Scene scene;
	const float eye[3] = { 0, -100, 150 };
	for (int j = 0; j < Grid; j++)
		for (int i = 0; i < Grid; i++) {
			const float sx = i / float(Grid - 1), sy = j / float(Grid - 1);
			Vertex v = {};
			const float wx = (sx - 0.5f) * 3000, wy = 50 + (1 - sy) * 2600, wz = Random() * 20;
			const float dx = wx - eye[0], dy = wy - eye[1], dz = wz - eye[2];
			const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
			Set(v, 0, sx * 2 - 1, 1 - sy * 2, 0.5f, 1);
			Set(v, 1, sx * 7 + Random() * 0.05f, sy * 7 + Random() * 0.05f, 0, 0);
			Set(v, 2, 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 1);
			Set(v, 3, wx, wy, wz, 1);
			Set(v, 4, 1, 0.2f * (Random() - 0.5f), 0.2f * (Random() - 0.5f), 0);
			Set(v, 5, 0.2f * (Random() - 0.5f), 1, 0.2f * (Random() - 0.5f), 0);
			Set(v, 6, 0.4f * (Random() - 0.5f), 0.4f * (Random() - 0.5f), 1, 0);
			float blends[8] = {}, total = 0;
			for (int t = 0; t < texCount; t++) { blends[t] = Random() < 0.3f ? 0.0f : Random(); total += blends[t]; }
			if (total == 0) { blends[0] = 1; total = 1; }
			for (int t = 0; t < texCount; t++) blends[t] /= total;
			Set(v, 7, blends[0], blends[1], blends[2], blends[3]);
			Set(v, 8, blends[4], blends[5], blends[6], 0);
			Set(v, 9, (sx * 2 - 1) * dist, (1 - sy * 2) * dist, dist * 0.999f, dist);
			Set(v, 10, eye[0], eye[1], eye[2], 1);
			scene.vertices.push_back(v);
		}
	GridIndices(scene);
	return scene;
}

static void RandomDirection(float out[3], float minLength, float maxLength) {
	float x = Random() - 0.5f, y = Random() - 0.5f, z = Random() * 0.8f + 0.2f;
	const float length = sqrtf(x * x + y * y + z * z), scale = (minLength + (maxLength - minLength) * Random()) / length;
	out[0] = x * scale; out[1] = y * scale; out[2] = z * scale;
}

// Objects (ObjectTemplate's LIGHTS >= 4 pixel shader): tangent-space light and view directions, object-space positions for
// the per-pixel attenuation, and the camera-relative world position in the interpolator the variant uses for it
// (TEXCOORD5 at MAX_LIGHTS 3, TEXCOORD6 at 4, the .w of TEXCOORD5-7 at 6), with the "present" sentinel 1.0.
static Scene ObjectScene(int maxLights) {
	Scene scene;
	for (int j = 0; j < Grid; j++)
		for (int i = 0; i < Grid; i++) {
			const float sx = i / float(Grid - 1), sy = j / float(Grid - 1);
			Vertex v = {};
			float dir[6][3], view[3];
			for (int k = 0; k < 6; k++) RandomDirection(dir[k], 0.7f, 1.3f);
			RandomDirection(view, 1.0f, 1.0f);
			const float world[3] = { (sx - 0.5f) * 400, 150 + 200 * sy + 20 * Random(), (0.5f - sy) * 150 + 10 * Random() };
			Set(v, 0, sx * 2 - 1, 1 - sy * 2, 0.5f, 1);
			Set(v, 1, sx * 5, sy * 5, 0, 0);                                                            // uv
			Set(v, 2, (sx - 0.5f) * 300, (sy - 0.5f) * 300, 30 * Random(), 1.0f);                       // lPosition (.w sentinel at MAX 6)
			Set(v, 3, dir[0][0], dir[0][1], dir[0][2], view[0]);                                        // lightDir, view.x
			Set(v, 4, dir[1][0], dir[1][1], dir[1][2], view[1]);                                        // light2, view.y
			Set(v, 5, dir[2][0], dir[2][1], dir[2][2], view[2]);                                        // light3, view.z
			if (maxLights == 3) {
				Set(v, 6, world[0], world[1], world[2], 1.0f);                                          // shadowWorldPos
			} else if (maxLights == 4) {
				Set(v, 6, dir[3][0], dir[3][1], dir[3][2], 0.5f);                                       // light4
				Set(v, 7, world[0], world[1], world[2], 1.0f);                                          // shadowWorldPos
			} else {
				Set(v, 6, dir[3][0], dir[3][1], dir[3][2], world[0]);                                   // light4, world.x
				Set(v, 7, dir[4][0], dir[4][1], dir[4][2], world[1]);                                   // light5, world.y
				Set(v, 8, dir[5][0], dir[5][1], dir[5][2], world[2]);                                   // light6, world.z
			}
			Set(v, 9, 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 0.6f + 0.4f * Random(), 1);    // vertex colour
			Set(v, 10, 0.5f, 0.55f, 0.6f, 0.1f + 0.3f * Random());                                     // fog colour, amount
			scene.vertices.push_back(v);
		}
	GridIndices(scene);
	return scene;
}

// 512x512 maps with a full mip chain. Colour maps: random colour, smooth heights in alpha. Normal maps: bent normals,
// random gloss in alpha.
static ComPtr<IDirect3DTexture9> MakeTexture(IDirect3DDevice9* device, int index, bool normalMap) {
	const UINT size = 512;
	ComPtr<IDirect3DTexture9> texture;
	Check(device->CreateTexture(size, size, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL), "CreateTexture");
	D3DLOCKED_RECT lock;
	Check(texture->LockRect(0, &lock, NULL, 0), "LockRect");
	const float f1 = 3.0f + index, f2 = 5.0f + 2 * index;
	for (UINT y = 0; y < size; y++) {
		DWORD* row = (DWORD*)((BYTE*)lock.pBits + y * lock.Pitch);
		for (UINT x = 0; x < size; x++) {
			const float u = x / float(size) * 6.2831853f, w = y / float(size) * 6.2831853f;
			const float h = 0.5f + 0.3f * sinf(u * f1) * cosf(w * f2) + 0.2f * (Random() - 0.5f);
			auto byte = [](float value) { return (DWORD)std::min(255.0f, std::max(0.0f, value * 255.0f + 0.5f)); };
			const float r = normalMap ? 0.5f + 0.3f * (Random() - 0.5f) : Random(), g = normalMap ? 0.5f + 0.3f * (Random() - 0.5f) : Random();
			const float b = normalMap ? 0.9f + 0.1f * Random() : Random();
			row[x] = (byte(h) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
		}
	}
	Check(texture->UnlockRect(0), "UnlockRect");
	Check(D3DXFilterTexture(texture.Get(), NULL, 0, D3DX_FILTER_BOX), "D3DXFilterTexture");
	return texture;
}

struct Constants {
	std::vector<float> c = std::vector<float>(224 * 4, 0.0f);
	void Set(int reg, float x, float y, float z, float w) { c[reg * 4] = x; c[reg * 4 + 1] = y; c[reg * 4 + 2] = z; c[reg * 4 + 3] = w; }
	void Apply(IDirect3DDevice9* device) { Check(device->SetPixelShaderConstantF(0, c.data(), 224), "SetPixelShaderConstantF"); }
};

static void CommonConstants(Constants& k) {
	for (int r = 0; r < 4; r++) {  // identity inverse projection and view (Shadow.hlsl)
		k.Set(100 + r, 0, 0, 0, 0); k.c[(100 + r) * 4 + r] = 1;
		k.Set(104 + r, 0, 0, 0, 0); k.c[(104 + r) * 4 + r] = 1;
	}
	for (int i = 0; i < 9; i++) k.Set(137 + i, 0.3f / (i + 1), 0.25f / (i + 1), 0.35f / (i + 1), 0);  // sky irradiance
}

static Constants TerrainConstants(const float parallax[4]) {
	Constants k;
	CommonConstants(k);
	k.Set(1, 0.2f, 0.2f, 0.25f, 1);                        // AmbientColor
	k.Set(3, 1.0f, 0.9f, 0.8f, 1);                         // SunColor
	k.Set(18, 0.39f, 0.52f, 0.76f, 0);                     // SunDir
	k.Set(32, 1, 0, 2, 0.5f); k.Set(33, 0, 1, 3, 0);       // LandSpec
	k.Set(34, 1, 1, 0, 1); k.Set(35, 1, 0, 1, 0);          // LandHeight: some textures without height maps
	k.Set(36, 6000, 5000, 1, 0); k.Set(37, 0.5f, 0.6f, 0.7f, 1);  // fog
	k.Set(89, 0.2f, 0.8f, 1, 1);                           // TESR_TerrainData: metal, rough, light scale, ambient scale
	k.Set(90, 1, 1, 0.7f, 2.7f);                           // TESR_TerrainExtraData: PBR on (parallax shadows only matter with PBR)
	k.Set(91, parallax[0], parallax[1], parallax[2], parallax[3]);  // TESR_TerrainParallaxData
	k.Set(92, 2048, 0.1f, 2.0f, 0);                        // TESR_TerrainParallaxExtraData: max distance, height, shadow intensity
	k.Set(135, 1.0f, 0.5f, 0, 0);                          // TESR_TerrainSkyData
	return k;
}

// lightsUsed goes where the variant reads it: EmittanceColor.a (c2) without OPT, PSLightColor[0].a (c3) with OPT.
static Constants ObjectConstants(float lightsUsed, bool opt) {
	Constants k;
	CommonConstants(k);
	unsigned saved = seed;
	seed = 777;  // the same lights for every call
	k.Set(1, 0.15f, 0.15f, 0.18f, 1);                      // AmbientColor (.a 1: no alpha test)
	k.Set(2, 0.2f, 0.1f, 0.05f, opt ? 3.0f : lightsUsed);  // EmittanceColor
	for (int i = 0; i < 10; i++) k.Set(3 + i, 0.3f + 0.7f * Random(), 0.3f + 0.7f * Random(), 0.3f + 0.7f * Random(), 0);  // PSLightColor
	if (opt) k.c[3 * 4 + 3] = lightsUsed;
	k.c[4 * 4 + 3] = 20;                                   // PSLightColor[1].w: gloss power with OPT
	for (int i = 0; i < 8; i++)                            // PSLightPosition: object space, radius
		k.Set(19 + i, (Random() - 0.5f) * 400, (Random() - 0.5f) * 400, 40 + 60 * Random(), 150 + 300 * Random());
	k.Set(27, 1, 1, 20, 0.5f);                             // Toggles: vertex colour, fog, gloss power, alpha ref
	k.Set(32, 0, 1, 1, 1);                                 // TESR_PBRData: -, roughness scale, light scale, ambient scale
	k.Set(33, 1, 1, 0.5f, 0);                              // TESR_PBRExtraData: saturation, skylight scale, directionality
	seed = saved;
	return k;
}

struct Gpu {
	ComPtr<IDirect3DDevice9> device;
	ComPtr<IDirect3DVertexShader9> terrainVS, objectVS;
	ComPtr<IDirect3DSurface9> target, readback, big;
	std::vector<ComPtr<IDirect3DTexture9>> textures;
	static const UINT Size = 1024;
};

static ComPtr<IDirect3DVertexShader9> MakeVertexShader(IDirect3DDevice9* device, const char* source) {
	ComPtr<ID3DXBuffer> code, errors;
	Check(D3DXCompileShader(source, (UINT)strlen(source), NULL, NULL, "main", "vs_3_0", 0, &code, &errors, NULL), "compile test vertex shader");
	ComPtr<IDirect3DVertexShader9> shader;
	Check(device->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &shader), "CreateVertexShader");
	return shader;
}

static void CreateGpu(Gpu& gpu) {
	HWND window = CreateWindowExA(0, "STATIC", "NVR game shader test", WS_OVERLAPPED, 0, 0, 128, 128, NULL, NULL, GetModuleHandle(NULL), NULL);
	if (!window) throw std::runtime_error("window creation failed");
	ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
	if (!d3d) throw std::runtime_error("D3D9 unavailable");
	D3DADAPTER_IDENTIFIER9 adapter = {}; Check(d3d->GetAdapterIdentifier(0, 0, &adapter), "GetAdapterIdentifier");
	printf("GPU: %s\n", adapter.Description);
	D3DDISPLAYMODE display = {}; Check(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display), "GetAdapterDisplayMode");
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
	pp.BackBufferWidth = 128; pp.BackBufferHeight = 128; pp.BackBufferFormat = display.Format;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	Check(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &gpu.device), "CreateDevice");
	IDirect3DDevice9* device = gpu.device.Get();
	gpu.terrainVS = MakeVertexShader(device, TerrainVertexShader);
	gpu.objectVS = MakeVertexShader(device, ObjectVertexShader);
	std::vector<D3DVERTEXELEMENT9> elements;
	elements.push_back({ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 });
	for (BYTE k = 0; k < 10; k++) elements.push_back({ 0, (WORD)(16 * (k + 1)), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, k });
	elements.push_back(D3DDECL_END());
	ComPtr<IDirect3DVertexDeclaration9> declaration;
	Check(device->CreateVertexDeclaration(elements.data(), &declaration), "CreateVertexDeclaration");
	Check(device->SetVertexDeclaration(declaration.Get()), "SetVertexDeclaration");
	Check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "cull");
	Check(device->SetRenderState(D3DRS_ZENABLE, FALSE), "z");
	Check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "lighting");
	for (int k = 0; k < 14; k++) {
		gpu.textures.push_back(MakeTexture(device, k % 7, k >= 7));
		device->SetSamplerState(k, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(k, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(k, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(k, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		device->SetSamplerState(k, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	}
	Check(device->CreateRenderTarget(Gpu::Size, Gpu::Size, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &gpu.target, NULL), "float target");
	Check(device->CreateOffscreenPlainSurface(Gpu::Size, Gpu::Size, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &gpu.readback, NULL), "readback");
	Check(device->CreateRenderTarget(2560, 1440, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &gpu.big, NULL), "timing target");
}

// Terrain: colour maps s0-s6, normal maps s7-s13. Objects: colour map s0, normal map s1.
static void BindTextures(Gpu& gpu, bool terrain) {
	for (int k = 0; k < 14; k++) {
		IDirect3DTexture9* texture = terrain ? gpu.textures[k].Get() : k == 0 ? gpu.textures[0].Get() : k == 1 ? gpu.textures[7].Get() : nullptr;
		Check(gpu.device->SetTexture(k, texture), "SetTexture");
	}
}

static IDirect3DPixelShader9* CreatePS(IDirect3DDevice9* device, ID3DXBuffer* code, ComPtr<IDirect3DPixelShader9>& holder) {
	Check(device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &holder), "CreatePixelShader");
	return holder.Get();
}

static void Draw(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, const Scene& scene) {
	Check(device->SetPixelShader(shader), "SetPixelShader");
	Check(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)scene.vertices.size(), (UINT)scene.indices.size() / 3,
		scene.indices.data(), D3DFMT_INDEX16, scene.vertices.data(), sizeof(Vertex)), "DrawIndexedPrimitiveUP");
}

static std::vector<float> Render(Gpu& gpu, IDirect3DPixelShader9* shader, const Scene& scene) {
	IDirect3DDevice9* device = gpu.device.Get();
	Check(device->SetRenderTarget(0, gpu.target.Get()), "SetRenderTarget");
	D3DVIEWPORT9 viewport = { 0, 0, Gpu::Size, Gpu::Size, 0, 1 };
	Check(device->SetViewport(&viewport), "SetViewport");
	Check(device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1, 0), "Clear");
	Check(device->BeginScene(), "BeginScene");
	Draw(device, shader, scene);
	Check(device->EndScene(), "EndScene");
	Check(device->GetRenderTargetData(gpu.target.Get(), gpu.readback.Get()), "GetRenderTargetData");
	D3DLOCKED_RECT lock;
	Check(gpu.readback->LockRect(&lock, NULL, D3DLOCK_READONLY), "LockRect readback");
	std::vector<float> pixels(Gpu::Size * Gpu::Size * 4);
	for (UINT y = 0; y < Gpu::Size; y++) memcpy(&pixels[y * Gpu::Size * 4], (BYTE*)lock.pBits + y * lock.Pitch, Gpu::Size * 16);
	gpu.readback->UnlockRect();
	return pixels;
}

struct Comparison { size_t different = 0, nan = 0; double worst = 0; };

static Comparison Compare(const std::vector<float>& a, const std::vector<float>& b) {
	Comparison result;
	for (size_t p = 0; p < a.size(); p += 4) {
		if (memcmp(&a[p], &b[p], 16) != 0) {
			result.different++;
			for (int k = 0; k < 4; k++) result.worst = std::max(result.worst, (double)fabsf(a[p + k] - b[p + k]));
		}
		for (int k = 0; k < 4; k++) if (a[p + k] != a[p + k] || b[p + k] != b[p + k]) { result.nan++; break; }
	}
	return result;
}

static double ChangedShare(const std::vector<float>& a, const std::vector<float>& b) {
	size_t changed = 0;
	for (size_t p = 0; p < a.size(); p += 4)
		if (fabsf(a[p] - b[p]) + fabsf(a[p + 1] - b[p + 1]) + fabsf(a[p + 2] - b[p + 2]) > 1e-3f) changed++;
	return changed / double(a.size() / 4);
}

static double TimeDraws(Gpu& gpu, IDirect3DPixelShader9* shader, const Scene& scene) {
	IDirect3DDevice9* device = gpu.device.Get();
	ComPtr<IDirect3DQuery9> start, end, frequency, disjoint;
	Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &start), "timestamp query");
	Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &end), "timestamp query");
	Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frequency), "frequency query");
	Check(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint), "disjoint query");
	Check(device->SetRenderTarget(0, gpu.big.Get()), "SetRenderTarget big");
	D3DVIEWPORT9 viewport = { 0, 0, 2560, 1440, 0, 1 };
	Check(device->SetViewport(&viewport), "SetViewport big");
	std::vector<double> samples;
	for (int run = 0; run < 25; run++) {
		Check(device->BeginScene(), "BeginScene");
		disjoint->Issue(D3DISSUE_BEGIN);
		start->Issue(D3DISSUE_END);
		for (int k = 0; k < 4; k++) Draw(device, shader, scene);
		end->Issue(D3DISSUE_END);
		disjoint->Issue(D3DISSUE_END);
		frequency->Issue(D3DISSUE_END);
		Check(device->EndScene(), "EndScene");
		UINT64 t0 = 0, t1 = 0, hz = 0; BOOL broken = TRUE;
		while (start->GetData(&t0, sizeof(t0), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (end->GetData(&t1, sizeof(t1), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (frequency->GetData(&hz, sizeof(hz), D3DGETDATA_FLUSH) == S_FALSE) {}
		while (disjoint->GetData(&broken, sizeof(broken), D3DGETDATA_FLUSH) == S_FALSE) {}
		if (run >= 5 && !broken && hz) samples.push_back((t1 - t0) * 1000.0 / hz / 4);
	}
	if (samples.empty()) return -1;
	std::sort(samples.begin(), samples.end());
	return samples[samples.size() / 2];
}

// Old and new alternated five times after a warm-up, so GPU clock changes hit both alike. Each round is already a
// median; the fastest round is the least disturbed one, so that is what is compared (with the slowest for spread).
static void TimeOldNew(Gpu& gpu, const char* label, IDirect3DPixelShader9* oldShader, IDirect3DPixelShader9* newShader, const Scene& scene) {
	TimeDraws(gpu, oldShader, scene);
	TimeDraws(gpu, newShader, scene);
	std::vector<double> oldMs, newMs;
	for (int round = 0; round < 5; round++) {
		oldMs.push_back(TimeDraws(gpu, oldShader, scene));
		newMs.push_back(TimeDraws(gpu, newShader, scene));
	}
	std::sort(oldMs.begin(), oldMs.end());
	std::sort(newMs.begin(), newMs.end());
	printf("  %-44s old %.3f ms (to %.3f), new %.3f ms (to %.3f), %+.1f%%\n", label, oldMs[0], oldMs[4],
		newMs[0], newMs[4], 100.0 * (newMs[0] - oldMs[0]) / oldMs[0]);
}

static int TestTerrain(Gpu& gpu, const std::string& oldFolder, const std::string& newFolder) {
	int failures = 0;
	const std::string oldFile = oldFolder + "\\TerrainTemplate.hlsl", newFile = newFolder + "\\TerrainTemplate.hlsl";
	auto defines = [](int texCount, const char* lights) {
		Defines d = { { "PS", "" }, { "TEX_COUNT", std::to_string(texCount) } };
		if (lights) d.push_back({ "NUM_PT_LIGHTS", lights });
		return d;
	};
	std::puts("TERRAIN instruction slots (static count; the parallax loop body counts once), with forward shadows:");
	for (int texCount = 1; texCount <= 7; texCount++) {
		unsigned oldSlots, newSlots;
		Compile(oldFile, "ps_3_0", defines(texCount, NULL), 1, &oldSlots);
		Compile(newFile, "ps_3_0", defines(texCount, NULL), 1, &newSlots);
		Compile(newFile, "ps_3_0", defines(texCount, "24"), 1);  // the point-light variants must compile too
		printf("  TEX_COUNT %d: old %u, new %u\n", texCount, oldSlots, newSlots);
	}
	struct Setting { const char* name; float parallax[4]; };
	const Setting settings[] = {
		{ "defaults (parallax, shadows, height blend, HQ)", { 1, 1, 1, 1 } },
		{ "8 steps", { 1, 1, 1, 0 } },
		{ "no height blend", { 1, 1, 0, 1 } },
		{ "no parallax shadows", { 1, 0, 1, 1 } },
		{ "parallax off", { 0, 1, 1, 1 } },
	};
	IDirect3DDevice9* device = gpu.device.Get();
	Check(device->SetVertexShader(gpu.terrainVS.Get()), "SetVertexShader");
	BindTextures(gpu, true);
	std::puts("TERRAIN pixels, 1024x1024 float target, every pixel must be bit-identical:");
	for (int texCount = 1; texCount <= 7; texCount++) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", defines(texCount, NULL), 0).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", defines(texCount, NULL), 0).Get(), newHolder);
		const Scene scene = TerrainScene(texCount);
		std::vector<float> reference[5];
		for (int s = 0; s < 5; s++) {
			TerrainConstants(settings[s].parallax).Apply(device);
			reference[s] = Render(gpu, oldShader, scene);
			const Comparison result = Compare(reference[s], Render(gpu, newShader, scene));
			printf("  TEX_COUNT %d, %-44s %s", texCount, settings[s].name, result.different ? "DIFFERENT" : "identical");
			if (result.different) printf(" (%zu pixels, largest difference %.3g)", result.different, result.worst);
			printf("\n");
			if (result.different) failures++;
			if (result.nan) { printf("FAIL: %zu NaN pixels\n", result.nan); failures++; }
		}
		const double parallax = ChangedShare(reference[0], reference[4]), shadows = ChangedShare(reference[0], reference[3]);
		printf("  TEX_COUNT %d: parallax changes %.0f%% of the pixels, parallax shadows %.0f%%\n", texCount, 100 * parallax, 100 * shadows);
		if (parallax < 0.25 || shadows < 0.1) { std::puts("FAIL: the test scene does not exercise parallax enough"); failures++; }
	}
	std::puts("TERRAIN GPU time per full-screen draw at 2560x1440 (A16B16G16R16F), game defaults:");
	TerrainConstants(settings[0].parallax).Apply(device);
	for (int texCount = 1; texCount <= 7; texCount += 2) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", defines(texCount, NULL), 1).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", defines(texCount, NULL), 1).Get(), newHolder);
		const std::string label = "TEX_COUNT " + std::to_string(texCount);
		TimeOldNew(gpu, label.c_str(), oldShader, newShader, TerrainScene(texCount));
	}
	return failures;
}

static int TestObjects(Gpu& gpu, const std::string& oldFolder, const std::string& newFolder) {
	int failures = 0;
	const std::string oldFile = oldFolder + "\\ObjectTemplate.hlsl", newFile = newFolder + "\\ObjectTemplate.hlsl";
	struct Variant { const char* name; Defines defines; int maxLights; bool opt; };
	const Variant variants[] = {  // src/effects/PBR.h; MAX_LIGHTS as in ObjectTemplate.hlsl
		{ "SLS2029 (LIGHTS 9)", { { "PS", "" }, { "LIGHTS", "9" } }, 6, false },
		{ "SLS2030 (LIGHTS 9, SI)", { { "PS", "" }, { "LIGHTS", "9" }, { "SI", "" } }, 6, false },
		{ "SLS2031 (LIGHTS 4)", { { "PS", "" }, { "LIGHTS", "4" } }, 4, false },
		{ "SLS2032 (LIGHTS 4, OPT)", { { "PS", "" }, { "LIGHTS", "4" }, { "OPT", "" } }, 4, true },
		{ "SLS2033 (LIGHTS 4, SI)", { { "PS", "" }, { "LIGHTS", "4" }, { "SI", "" } }, 4, false },
		{ "SLS2034 (LIGHTS 4, SPECULAR)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" } }, 3, false },
		{ "SLS2035 (LIGHTS 4, SPECULAR, OPT)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" }, { "OPT", "" } }, 3, true },
		{ "SLS2036 (LIGHTS 4, SPECULAR, SI)", { { "PS", "" }, { "LIGHTS", "4" }, { "SPECULAR", "" }, { "SI", "" } }, 3, false },
	};
	std::puts("OBJECTS instruction slots, with forward shadows:");
	for (const Variant& v : variants) {
		unsigned oldSlots, newSlots;
		Compile(oldFile, "ps_3_0", v.defines, 1, &oldSlots);
		Compile(newFile, "ps_3_0", v.defines, 1, &newSlots);
		printf("  %-36s old %u, new %u\n", v.name, oldSlots, newSlots);
	}
	IDirect3DDevice9* device = gpu.device.Get();
	Check(device->SetVertexShader(gpu.objectVS.Get()), "SetVertexShader");
	BindTextures(gpu, false);
	std::puts("OBJECTS pixels with 0-6 lights in use, 1024x1024 float target, every pixel must be bit-identical:");
	for (const Variant& v : variants) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", v.defines, 0).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", v.defines, 0).Get(), newHolder);
		const Scene scene = ObjectScene(v.maxLights);
		std::vector<float> fewest, most;
		std::string summary;
		for (int used = 0; used <= 6; used++) {
			ObjectConstants((float)used, v.opt).Apply(device);
			std::vector<float> a = Render(gpu, oldShader, scene);
			const Comparison result = Compare(a, Render(gpu, newShader, scene));
			summary += result.different ? " DIFFERENT" : " same";
			if (result.different) {
				printf("  %s, %d lights: %zu pixels differ, largest difference %.3g\n", v.name, used, result.different, result.worst);
				failures++;
			}
			if (result.nan) { printf("FAIL: %s, %d lights: %zu NaN pixels\n", v.name, used, result.nan); failures++; }
			if (used == 0) fewest = a;
			if (used == 6) most = a;
		}
		const double lights = ChangedShare(fewest, most);
		printf("  %-36s 0-6 lights:%s; the extra lights change %.0f%% of the pixels\n", v.name, summary.c_str(), 100 * lights);
		if (lights < 0.5) { std::puts("FAIL: the extra lights barely change the test scene"); failures++; }
	}
	std::puts("OBJECTS GPU time per full-screen draw at 2560x1440 (A16B16G16R16F), with forward shadows compiled in:");
	for (const Variant& v : variants) {
		ComPtr<IDirect3DPixelShader9> oldHolder, newHolder;
		IDirect3DPixelShader9* oldShader = CreatePS(device, Compile(oldFile, "ps_3_0", v.defines, 1).Get(), oldHolder);
		IDirect3DPixelShader9* newShader = CreatePS(device, Compile(newFile, "ps_3_0", v.defines, 1).Get(), newHolder);
		const Scene scene = ObjectScene(v.maxLights);
		for (int used : { 2, v.maxLights }) {
			ObjectConstants((float)used, v.opt).Apply(device);
			const std::string label = std::string(v.name) + ", " + std::to_string(used) + " lights";
			TimeOldNew(gpu, label.c_str(), oldShader, newShader, scene);
		}
	}
	return failures;
}

int main(int argc, char** argv) {
	try {
		if (argc >= 5 && strcmp(argv[1], "--asm-template") == 0) {
			Defines defines;
			for (int k = 5; k < argc; k++) {
				const char* eq = strchr(argv[k], '=');
				defines.push_back({ eq ? std::string(argv[k], (size_t)(eq - argv[k])) : std::string(argv[k]), eq ? std::string(eq + 1) : std::string() });
			}
			std::string text;
			Compile(argv[2], argv[3], defines, 1, nullptr, &text);
			FILE* out = nullptr;
			if (fopen_s(&out, argv[4], "w") || !out) return 1;
			fputs(text.c_str(), out);
			fclose(out);
			return 0;
		}
		if (argc != 3 && argc != 4) { std::puts("usage: game_shaders <old Shaders folder> <new Shaders folder> [terrain|objects]"); return 2; }
		const std::string which = argc == 4 ? argv[3] : "";
		Gpu gpu;
		CreateGpu(gpu);
		int failures = 0;
		if (which.empty() || which == "terrain") failures += TestTerrain(gpu, argv[1], argv[2]);
		if (which.empty() || which == "objects") failures += TestObjects(gpu, argv[1], argv[2]);
		if (failures) { printf("%d game shader check(s) FAILED\n", failures); return 1; }
		std::puts("All game shader checks passed.");
		return 0;
	}
	catch (const std::exception& e) {
		printf("ERROR: %s\n", e.what());
		return 1;
	}
}
