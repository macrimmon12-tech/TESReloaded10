// Regression test for LUTEffect's identity-LUT skip (src/effects/LUTIdentity.h).
//
//  1. The shipped neutral_lut.png, loaded by the real D3DX loader exactly as
//     TextureManager::GetFileTexture does, is recognised as an identity.
//  2. Synthetic identity strips (N = 16/32/64) are recognised; a texel off by more than one 8-bit
//     step, a different size, or an unsupported format is not.
//  3. Numerically: sampling an identity strip the way LUT.fx.hlsl does (two bilinear reads blended
//     on blue, HDR-compat scaling) returns the input, so skipping the pass changes nothing.
//
// Uses a NULLREF device (no GPU needed); falls back to REF/HAL if the runtime lacks it.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>

#include "../src/effects/LUTIdentity.h"
#include "../src/effects/LUTFile.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static IDirect3DDevice9* CreateTestDevice(IDirect3D9* d3d)
{
	HWND window = GetDesktopWindow();
	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.hDeviceWindow = window;
	const D3DDEVTYPE types[] = { D3DDEVTYPE_NULLREF, D3DDEVTYPE_REF, D3DDEVTYPE_HAL };
	for (D3DDEVTYPE type : types) {
		IDirect3DDevice9* device = nullptr;
		if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, type, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device)))
			return device;
	}
	return nullptr;
}

// Identity strip: texel (x = b*N + r, y = g) holds (r, g, b) / (N - 1). Memory order B, G, R, A.
static IDirect3DTexture9* MakeStrip(IDirect3DDevice9* device, UINT n, D3DFORMAT format = D3DFMT_A8R8G8B8, UINT width = 0)
{
	IDirect3DTexture9* texture = nullptr;
	if (FAILED(device->CreateTexture(width ? width : n * n, n, 1, 0, format, D3DPOOL_MANAGED, &texture, NULL))) return nullptr;
	D3DLOCKED_RECT rect;
	if (FAILED(texture->LockRect(0, &rect, NULL, 0))) { texture->Release(); return nullptr; }
	if (format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8) {
		const UINT texWidth = width ? width : n * n;
		for (UINT g = 0; g < n; g++) {
			BYTE* row = (BYTE*)rect.pBits + (size_t)g * rect.Pitch;
			for (UINT b = 0; b < n; b++)
				for (UINT r = 0; r < n; r++) {
					if (b * n + r >= texWidth) continue; // a deliberately narrow texture
					BYTE* px = row + (size_t)(b * n + r) * 4;
					px[0] = (BYTE)(b * 255.0f / (n - 1) + 0.5f);
					px[1] = (BYTE)(g * 255.0f / (n - 1) + 0.5f);
					px[2] = (BYTE)(r * 255.0f / (n - 1) + 0.5f);
					px[3] = 255;
				}
		}
	}
	texture->UnlockRect(0);
	return texture;
}

static void SetChannel(IDirect3DTexture9* texture, UINT x, UINT y, int channel, int delta)
{
	D3DLOCKED_RECT rect;
	texture->LockRect(0, &rect, NULL, 0);
	BYTE* px = (BYTE*)rect.pBits + (size_t)y * rect.Pitch + (size_t)x * 4 + channel;
	*px = (BYTE)std::max(0, std::min(255, (int)*px + delta));
	texture->UnlockRect(0);
}

// CPU model of SampleLUT() in LUT.fx.hlsl on a texture whose texels come from `texel(x, y)` (0..1).
struct Rgb { float r, g, b; };
static Rgb Bilinear(const std::vector<Rgb>& tex, UINT width, UINT height, float u, float v)
{
	const float x = u * width - 0.5f, y = v * height - 0.5f;
	const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
	const float fx = x - x0, fy = y - y0;
	auto at = [&](int xx, int yy) { xx = std::max(0, std::min((int)width - 1, xx)); yy = std::max(0, std::min((int)height - 1, yy)); return tex[(size_t)yy * width + xx]; };
	Rgb a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
	auto mix = [&](float p, float q, float t) { return p + (q - p) * t; };
	return { mix(mix(a.r, b.r, fx), mix(c.r, d.r, fx), fy), mix(mix(a.g, b.g, fx), mix(c.g, d.g, fx), fy), mix(mix(a.b, b.b, fx), mix(c.b, d.b, fx), fy) };
}

static Rgb SampleLUT(const std::vector<Rgb>& tex, float n, Rgb color)
{
	const float b = color.b * (n - 1.0f);
	const float bCell = std::floor(b), bFrac = b - bCell;
	const float invW = 1.0f / (n * n), invH = 1.0f / n;
	const float u1 = (bCell * n + color.r * (n - 1.0f) + 0.5f) * invW;
	const float u2 = ((bCell + 1.0f) * n + color.r * (n - 1.0f) + 0.5f) * invW;
	const float v = (color.g * (n - 1.0f) + 0.5f) * invH;
	Rgb p = Bilinear(tex, (UINT)(n * n), (UINT)n, u1, v), q = Bilinear(tex, (UINT)(n * n), (UINT)n, u2, v);
	return { p.r + (q.r - p.r) * bFrac, p.g + (q.g - p.g) * bFrac, p.b + (q.b - p.b) * bFrac };
}

int main()
{
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	IDirect3DDevice9* device = d3d ? CreateTestDevice(d3d) : nullptr;
	if (!device) { std::printf("SKIP: no D3D9 device (NULLREF/REF/HAL) could be created here.\n"); return 0; }

	// 1. The shipped file through the real loader.
	IDirect3DTexture9* shipped = nullptr;
	HRESULT hr = D3DXCreateTextureFromFileA(device, "resource/Textures/NewVegasReloaded/LUTs/neutral_lut.png", &shipped);
	CHECK(SUCCEEDED(hr) && shipped, "D3DX loads neutral_lut.png (hr=%08lx)", (unsigned long)hr);
	if (shipped) {
		D3DSURFACE_DESC desc = {};
		shipped->GetLevelDesc(0, &desc);
		std::printf("      loaded as %ux%u format %u, %u levels\n", desc.Width, desc.Height, (unsigned)desc.Format, (unsigned)shipped->GetLevelCount());
		CHECK(IsIdentityLUT(shipped), "shipped neutral_lut.png is recognised as an identity LUT");
		shipped->Release();
	}

	// 2. Synthetic strips and rejections.
	for (UINT n : { 16u, 32u, 64u }) {
		IDirect3DTexture9* strip = MakeStrip(device, n);
		CHECK(strip && IsIdentityLUT(strip), "synthetic %ux%ux%u identity strip is recognised", n, n, n);
		if (strip) strip->Release();
	}
	{
		IDirect3DTexture9* strip = MakeStrip(device, 16);
		SetChannel(strip, 100, 7, 2, 1);
		CHECK(IsIdentityLUT(strip), "a texel one 8-bit step off is still an identity (rounding)");
		SetChannel(strip, 100, 7, 2, 2); // now 3 steps off
		CHECK(!IsIdentityLUT(strip), "a texel three steps off is not an identity");
		strip->Release();
	}
	{
		IDirect3DTexture9* strip = MakeStrip(device, 16);
		SetChannel(strip, 255, 15, 0, -40); // last texel, blue channel
		CHECK(!IsIdentityLUT(strip), "a change in the last texel is found");
		strip->Release();
	}
	{
		IDirect3DTexture9* strip = MakeStrip(device, 16, D3DFMT_A8R8G8B8, 250);
		CHECK(strip && !IsIdentityLUT(strip), "a strip whose width is not N*N is not an identity");
		if (strip) strip->Release();
	}
	{
		IDirect3DTexture9* strip = MakeStrip(device, 16, D3DFMT_A16B16G16R16F);
		CHECK(!strip || !IsIdentityLUT(strip), "an unsupported format is not treated as an identity");
		if (strip) strip->Release();
	}
	CHECK(!IsIdentityLUT(nullptr), "a null texture is not an identity");

	// 3. The shader's own maths on an identity strip returns its input (worst case over many colours).
	for (UINT n : { 16u, 32u, 64u }) {
		std::vector<Rgb> tex((size_t)n * n * n);
		for (UINT g = 0; g < n; g++)
			for (UINT b = 0; b < n; b++)
				for (UINT r = 0; r < n; r++) {
					auto q = [&](UINT v) { return (float)(int)(v * 255.0f / (n - 1) + 0.5f) / 255.0f; }; // 8-bit texels
					tex[(size_t)g * (n * n) + b * n + r] = { q(r), q(g), q(b) };
				}
		std::mt19937 rng(1234 + n);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f), hdr(0.0f, 8.0f);
		float worstSdr = 0.0f, worstHdr = 0.0f;
		for (int i = 0; i < 200000; i++) {
			Rgb c = { unit(rng), unit(rng), unit(rng) };
			Rgb o = SampleLUT(tex, (float)n, c);
			worstSdr = std::max(worstSdr, std::max(std::fabs(o.r - c.r), std::max(std::fabs(o.g - c.g), std::fabs(o.b - c.b))));
			// HDR compat: normalise by the brightest channel (>= 1), sample, scale back.
			Rgb h = { hdr(rng), hdr(rng), hdr(rng) };
			const float scale = std::max(std::max(h.r, h.g), std::max(h.b, 1.0f));
			Rgb oh = SampleLUT(tex, (float)n, { h.r / scale, h.g / scale, h.b / scale });
			oh = { oh.r * scale, oh.g * scale, oh.b * scale };
			worstHdr = std::max(worstHdr, std::max(std::fabs(oh.r - h.r), std::max(std::fabs(oh.g - h.g), std::fabs(oh.b - h.b))) / scale);
		}
		std::printf("      N=%u: worst |out - in| SDR %.5f (1/255 = %.5f), HDR relative to scale %.5f\n", n, worstSdr, 1.0f / 255.0f, worstHdr);
		CHECK(worstSdr <= 1.0f / 255.0f && worstHdr <= 1.0f / 255.0f, "N=%u identity strip through the LUT maths stays within one 8-bit step", n);
	}

	// 4. .cube files (src/effects/LUTFile.h): parsed into the strip layout LUT.fx.hlsl reads.
	auto cubeText = [](unsigned n, auto output, const char* header) {
		std::string text = std::string("TITLE \"test\"\n# comment\n") + header + "LUT_3D_SIZE " + std::to_string(n) + "\n";
		char line[96];
		for (unsigned b = 0; b < n; b++)
			for (unsigned g = 0; g < n; g++)
				for (unsigned r = 0; r < n; r++) { // red fastest, as the format defines
					Rgb o = output(r / (float)(n - 1), g / (float)(n - 1), b / (float)(n - 1));
					sprintf_s(line, "%.6f %.6f %.6f\n", o.r, o.g, o.b);
					text += line;
				}
		return text;
	};
	auto identity = [](float r, float g, float b) { return Rgb{ r, g, b }; };
	for (unsigned n : { 2u, 17u, 33u }) {
		std::istringstream in(cubeText(n, identity, ""));
		CubeLUT cube;
		std::string error;
		const bool parsed = ParseCubeLUT(in, cube, error);
		IDirect3DTexture9* strip = parsed ? CreateStripFromCube(device, cube, error) : nullptr;
		D3DSURFACE_DESC desc = {};
		if (strip) strip->GetLevelDesc(0, &desc);
		CHECK(strip && desc.Width == n * n && desc.Height == n && IsIdentityLUT(strip),
			"an identity .cube of size %u becomes a %ux%u identity strip (%s)", n, n * n, n, error.c_str());
		if (strip) strip->Release();
	}
	{
		// A non-trivial LUT: the strip sampled the way LUT.fx.hlsl samples it must reproduce the LUT's own
		// trilinear interpolation (this is what pins down the axis order: red across a cell, green down,
		// blue from cell to cell).
		const unsigned n = 17;
		auto grade = [](float r, float g, float b) { return Rgb{ 0.1f + 0.8f * g * g, std::sqrt(b) * 0.9f, 1.0f - r * 0.7f }; };
		std::istringstream in(cubeText(n, grade, "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n"));
		CubeLUT cube;
		std::string error;
		CHECK(ParseCubeLUT(in, cube, error), "a graded 17-point .cube with DOMAIN lines parses (%s)", error.c_str());
		IDirect3DTexture9* strip = CreateStripFromCube(device, cube, error);
		std::vector<Rgb> tex((size_t)n * n * n);
		if (strip) {
			D3DLOCKED_RECT rect;
			strip->LockRect(0, &rect, NULL, D3DLOCK_READONLY);
			for (unsigned y = 0; y < n; y++)
				for (unsigned x = 0; x < n * n; x++) {
					const BYTE* px = (const BYTE*)rect.pBits + (size_t)y * rect.Pitch + (size_t)x * 4;
					tex[(size_t)y * n * n + x] = { px[2] / 255.0f, px[1] / 255.0f, px[0] / 255.0f };
				}
			strip->UnlockRect(0);
			strip->Release();
		}
		auto entry = [&](unsigned r, unsigned g, unsigned b) {
			const float* v = &cube.RGB[((size_t)b * n * n + (size_t)g * n + r) * 3];
			return Rgb{ v[0], v[1], v[2] };
		};
		auto trilinear = [&](Rgb c) {
			const float fr = c.r * (n - 1), fg = c.g * (n - 1), fb = c.b * (n - 1);
			const unsigned r0 = std::min((unsigned)fr, n - 2), g0 = std::min((unsigned)fg, n - 2), b0 = std::min((unsigned)fb, n - 2);
			const float dr = fr - r0, dg = fg - g0, db = fb - b0;
			Rgb sum = { 0, 0, 0 };
			for (int i = 0; i < 8; i++) {
				const float w = ((i & 1) ? dr : 1 - dr) * ((i & 2) ? dg : 1 - dg) * ((i & 4) ? db : 1 - db);
				const Rgb v = entry(r0 + (i & 1), g0 + ((i >> 1) & 1), b0 + ((i >> 2) & 1));
				sum = { sum.r + w * v.r, sum.g + w * v.g, sum.b + w * v.b };
			}
			return sum;
		};
		std::mt19937 rng(77);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		float worst = 0.0f;
		for (int i = 0; i < 100000 && strip; i++) {
			const Rgb c = { unit(rng), unit(rng), unit(rng) };
			const Rgb shader = SampleLUT(tex, (float)n, c), reference = trilinear(c);
			worst = std::max(worst, std::max(std::fabs(shader.r - reference.r), std::max(std::fabs(shader.g - reference.g), std::fabs(shader.b - reference.b))));
		}
		std::printf("      graded .cube, N=%u: worst |shader model - trilinear .cube| %.5f\n", n, worst);
		CHECK(strip && worst <= 1.0f / 255.0f, "a graded .cube read through the shader's sampling matches its own trilinear lookup within one 8-bit step");
	}
	{
		auto rejects = [&](const std::string& text, const char* what) {
			std::istringstream in(text);
			CubeLUT cube;
			std::string error;
			const bool parsed = ParseCubeLUT(in, cube, error);
			CHECK(!parsed && !error.empty(), "rejected: %s (%s)", what, error.c_str());
		};
		std::string complete = cubeText(2, identity, "");
		rejects("0 0 0\n", "no LUT_3D_SIZE");
		rejects(complete.substr(0, complete.size() - 10), "an entry missing");
		rejects("LUT_1D_SIZE 16\n", "a 1D LUT");
		rejects(cubeText(2, identity, "DOMAIN_MAX 2 2 2\n"), "a domain other than 0..1");
		rejects(cubeText(2, identity, "LUT_3D_INPUT_RANGE -0.5 1.5\n"), "a Resolve input range other than 0..1");
		rejects("LUT_3D_SIZE 2\n0 0 zero\n", "an unreadable data line");
		rejects("LUT_3D_SIZE 1\n", "a size below 2");
	}

	// 5. Images keep their exact size. A 33-point strip is 1089x33; the old loader turned it into 2048x64.
	{
		const unsigned n = 33;
		std::istringstream in(cubeText(n, identity, ""));
		CubeLUT cube;
		std::string error;
		ParseCubeLUT(in, cube, error);
		IDirect3DTexture9* strip = CreateStripFromCube(device, cube, error);
		char path[MAX_PATH], dir[MAX_PATH];
		GetTempPathA(MAX_PATH, dir);
		sprintf_s(path, "%snvr_lut_test_%lu.png", dir, GetCurrentProcessId());
		const bool saved = strip && SUCCEEDED(D3DXSaveTextureToFileA(path, D3DXIFF_PNG, strip, NULL));
		if (strip) strip->Release();
		IDirect3DTexture9* old = nullptr;
		D3DSURFACE_DESC oldDesc = {}, newDesc = {};
		if (saved && SUCCEEDED(D3DXCreateTextureFromFileA(device, path, &old))) { old->GetLevelDesc(0, &oldDesc); old->Release(); }
		IDirect3DTexture9* exact = saved ? LoadLUTTexture(device, path, error) : nullptr;
		if (exact) exact->GetLevelDesc(0, &newDesc);
		std::printf("      1089x33 PNG strip: old loader %ux%u, new loader %ux%u, %u level(s)\n", oldDesc.Width, oldDesc.Height,
			newDesc.Width, newDesc.Height, exact ? (unsigned)exact->GetLevelCount() : 0u);
		CHECK(exact && newDesc.Width == n * n && newDesc.Height == n && exact->GetLevelCount() == 1 && IsIdentityLUT(exact),
			"a non-power-of-two PNG strip loads at its exact size, one level, texels intact");
		if (exact) exact->Release();
		DeleteFileA(path);

		// The same table as a .cube file on disk, through the same entry point LUTEffect uses.
		sprintf_s(path, "%snvr_lut_test_%lu.cube", dir, GetCurrentProcessId());
		FILE* file = nullptr;
		if (fopen_s(&file, path, "wb") == 0 && file) { const std::string text = cubeText(n, identity, ""); fwrite(text.data(), 1, text.size(), file); fclose(file); }
		IDirect3DTexture9* fromCube = LoadLUTTexture(device, path, error);
		D3DSURFACE_DESC cubeDesc = {};
		if (fromCube) fromCube->GetLevelDesc(0, &cubeDesc);
		CHECK(fromCube && cubeDesc.Width == n * n && cubeDesc.Height == n && IsIdentityLUT(fromCube), "a .cube file on disk loads as a strip (%s)", error.c_str());
		if (fromCube) fromCube->Release();
		DeleteFileA(path);
		std::string missing;
		CHECK(!LoadLUTTexture(device, "does-not-exist.cube", missing) && !missing.empty(), "a missing .cube reports an error instead of crashing");
	}

	device->Release();
	d3d->Release();
	std::printf(failures ? "\n%d check(s) FAILED\n" : "\nAll LUT identity checks passed\n", failures);
	return failures ? 1 : 0;
}
