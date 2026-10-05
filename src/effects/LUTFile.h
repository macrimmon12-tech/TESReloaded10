#pragma once

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// Loading of colour-grading LUT files for LUTEffect. Needs only D3D9 and D3DX, so
// tests/lut_identity.cpp checks it on a NULLREF device.
//
// LUT.fx.hlsl reads an N x N x N LUT from a horizontal strip texture N*N wide and N high: texel
// (x = b*N + r, y = g) holds the output for input (r, g, b) / (N - 1).

// A 3D LUT in the Adobe/Resolve ".cube" text format: N^3 lines of "r g b", red changing fastest, then
// green, then blue (the same order as the strip above, cell by cell).
struct CubeLUT {
	unsigned N = 0;
	std::vector<float> RGB;		// 3 * N^3 values, in file order
};

// Parses a .cube file. Returns false with a reason for anything LUT.fx.hlsl cannot use as is: a 1D LUT,
// an input domain other than 0..1, a missing or wrong entry count, or an unreadable line.
inline bool ParseCubeLUT(std::istream& in, CubeLUT& cube, std::string& error)
{
	cube = CubeLUT();
	float domainMin[3] = { 0.0f, 0.0f, 0.0f }, domainMax[3] = { 1.0f, 1.0f, 1.0f };
	std::string line;
	unsigned lineNumber = 0;
	while (std::getline(in, line)) {
		lineNumber++;
		const size_t hash = line.find('#');
		if (hash != std::string::npos) line.resize(hash);
		std::istringstream fields(line);
		std::string first;
		if (!(fields >> first)) continue; // blank or comment-only line

		if (first == "TITLE") continue;
		if (first == "LUT_1D_SIZE") { error = "1D LUTs are not supported (a 3D LUT is needed)"; return false; }
		if (first == "LUT_3D_SIZE") {
			int n = 0;
			if (!(fields >> n) || n < 2 || n > 256) { error = "LUT_3D_SIZE must be 2-256"; return false; }
			cube.N = (unsigned)n;
			cube.RGB.reserve((size_t)3 * n * n * n);
			continue;
		}
		if (first == "DOMAIN_MIN" || first == "DOMAIN_MAX") {
			float* domain = first == "DOMAIN_MIN" ? domainMin : domainMax;
			if (!(fields >> domain[0] >> domain[1] >> domain[2])) { error = first + " needs three values"; return false; }
			continue;
		}
		if (first == "LUT_3D_INPUT_RANGE") { // Resolve's form of the domain
			float low = 0.0f, high = 1.0f;
			if (!(fields >> low >> high)) { error = "LUT_3D_INPUT_RANGE needs two values"; return false; }
			for (int c = 0; c < 3; c++) { domainMin[c] = low; domainMax[c] = high; }
			continue;
		}

		// Other keywords some tools write (none change how the table is read): skip them.
		if ((first[0] >= 'A' && first[0] <= 'Z') || (first[0] >= 'a' && first[0] <= 'z')) continue;

		// A data line: three numbers.
		char* end = nullptr;
		const float r = std::strtof(first.c_str(), &end);
		float g = 0.0f, b = 0.0f;
		if (end == first.c_str() || *end || !(fields >> g >> b)) {
			error = "unreadable line " + std::to_string(lineNumber) + ": " + line.substr(0, 40);
			return false;
		}
		if (!cube.N) { error = "data before LUT_3D_SIZE"; return false; }
		cube.RGB.push_back(r);
		cube.RGB.push_back(g);
		cube.RGB.push_back(b);
	}
	if (!cube.N) { error = "no LUT_3D_SIZE line"; return false; }
	for (int c = 0; c < 3; c++) {
		if (domainMin[c] != 0.0f || domainMax[c] != 1.0f) { error = "an input domain other than 0..1 is not supported"; return false; }
	}
	const size_t expected = (size_t)3 * cube.N * cube.N * cube.N;
	if (cube.RGB.size() != expected) {
		error = "expected " + std::to_string(expected / 3) + " entries for size " + std::to_string(cube.N) + ", found " + std::to_string(cube.RGB.size() / 3);
		return false;
	}
	return true;
}

// Builds the strip texture for a parsed .cube (A8R8G8B8, values clamped to 0..1 and rounded to 8 bits,
// like a PNG strip exported from it would be).
inline IDirect3DTexture9* CreateStripFromCube(IDirect3DDevice9* device, const CubeLUT& cube, std::string& error)
{
	const unsigned n = cube.N;
	IDirect3DTexture9* texture = nullptr;
	HRESULT hr = device->CreateTexture(n * n, n, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL);
	if (FAILED(hr)) { error = "could not create a " + std::to_string(n * n) + "x" + std::to_string(n) + " texture"; return nullptr; }
	D3DLOCKED_RECT rect;
	if (FAILED(texture->LockRect(0, &rect, NULL, 0))) { texture->Release(); error = "could not fill the texture"; return nullptr; }
	auto byte = [](float v) { return (BYTE)(v <= 0.0f ? 0 : v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f)); };
	for (unsigned b = 0; b < n; b++)
		for (unsigned g = 0; g < n; g++)
			for (unsigned r = 0; r < n; r++) {
				const float* v = &cube.RGB[((size_t)b * n * n + (size_t)g * n + r) * 3];
				BYTE* px = (BYTE*)rect.pBits + (size_t)g * rect.Pitch + ((size_t)b * n + r) * 4; // memory order B, G, R, A
				px[0] = byte(v[2]);
				px[1] = byte(v[1]);
				px[2] = byte(v[0]);
				px[3] = 255;
			}
	texture->UnlockRect(0);
	return texture;
}

// Loads a LUT file: a .cube file, or an image (PNG/DDS/BMP) at its EXACT size. The plain D3DX loader
// used before rounded every side up to a power of two and resampled the image (a 1089x33 strip exported
// from a 33-point LUT came out 2048x64, which the shader then reads as a different, scrambled LUT) and
// added mip levels the shader never samples.
inline IDirect3DTexture9* LoadLUTTexture(IDirect3DDevice9* device, const char* path, std::string& error)
{
	const char* dot = std::strrchr(path, '.');
	if (dot && _stricmp(dot, ".cube") == 0) {
		FILE* file = nullptr;
		if (fopen_s(&file, path, "rb") != 0 || !file) { error = "file not found"; return nullptr; }
		std::string text;
		char buffer[4096];
		size_t read;
		while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
		fclose(file);
		std::istringstream in(text);
		CubeLUT cube;
		if (!ParseCubeLUT(in, cube, error)) return nullptr;
		return CreateStripFromCube(device, cube, error);
	}

	IDirect3DTexture9* texture = nullptr;
	HRESULT hr = D3DXCreateTextureFromFileExA(device, path, D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1, 0, D3DFMT_UNKNOWN,
		D3DPOOL_MANAGED, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, NULL, NULL, &texture);
	if (FAILED(hr)) {
		// A device without non-power-of-two textures: the old loader is still better than nothing, and a
		// strip it has resampled is then rejected by LUTEffect's shape check.
		texture = nullptr;
		hr = D3DXCreateTextureFromFileA(device, path, &texture);
	}
	if (FAILED(hr) || !texture) {
		char code[32];
		sprintf_s(code, "%08lX", (unsigned long)hr);
		error = std::string("could not load the image (hr=") + code + ")";
		return nullptr;
	}
	return texture;
}
