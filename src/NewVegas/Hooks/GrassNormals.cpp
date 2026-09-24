#include "GrassNormals.h"
#include <unordered_map>
#include <string>
#include <algorithm>

namespace GrassNormals {
	// NVR's sky hook (Hooks.cpp) replaces SkyShader::UpdateConstants at this vtable entry, and it works
	// in game, so it pins UpdateConstants to slot 31 of the BSShader vtable layout TallGrassShader shares.
	static constexpr UInt32 kSkyUpdateConstantsEntry = 0x10AFE94;
	static constexpr UInt32 kUpdateConstantsSlot = 31;
	static constexpr UInt32 kGetRTTISlot = 2;
	static constexpr UInt32 kVtblTallGrassShader = 0x10B8980;	// class_vtbls.h
	static constexpr UInt32 kVtblNiSourceTexture = 0x109B9EC;	// class_vtbls.h

	// Must match GRASS23x000TMS.pso.hlsl: GrassNormalMap : register(s10), GrassNormalParams : register(c199).
	// s10 is free in the grass shader (s0 diffuse, s9 shadow atlas); c199 sits past its TESR_ constants.
	static constexpr UInt32 kSampler = 10;
	static constexpr UInt32 kParamsRegister = 199;

	static VirtFuncDetour s_detour;
	static bool s_installed = false;

	struct TextureEntry {
		const char* path;				// the NiSourceTexture's filename pointer when this was built
		IDirect3DTexture9* normal;		// null: no normal map for this texture
	};
	struct PropertyEntry {
		const void* texture;			// null: none found, and not looked for again
		bool matched;					// found as the texture bound for the draw, so revalidated against it
	};
	static std::unordered_map<UInt64, PropertyEntry> s_propertyTextures;	// (texturing, shade, geometry) -> diffuse
	static std::unordered_map<const void*, TextureEntry> s_textures;
	static std::unordered_map<std::string, IDirect3DTexture9*> s_normalsByPath;	// loaded once, kept for the session
	static bool s_loggedNoTexture = false;
	static bool s_loggedFirst = false;

	// ---- Memory reads that may be wrong, and so are guarded -------------------------------------
	// Nothing here trusts a guessed structure layout past what NVR's headers already assert: the grass
	// texture is found by walking the geometry's properties for objects that are provably textures, and
	// the one bound for the draw is preferred. Every read is guarded.

	static bool ReadVtableRTTIName(UInt32 aVtable, char* apName, UInt32 aSize) {
		// GetRTTI is read, never called: MSVC compiles each class's to `mov eax, imm32 / ret`.
		__try {
			const UInt8* code = *(const UInt8**)(aVtable + kGetRTTISlot * 4);
			if (code[0] != 0xB8 || code[5] != 0xC3) return false;
			const NiRTTI* rtti = *(const NiRTTI**)(code + 1);
			strncpy_s(apName, aSize, rtti->name, _TRUNCATE);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// The game executable's own image: code and vtables live there, never a texture, so the search
	// does not wander into it.
	static UInt32 s_imageStart = 0;
	static UInt32 s_imageEnd = 0;

	static bool LooksLikePointer(UInt32 aValue) {
		// No upper bound below 4 GB: FNV is commonly large-address-aware, with heap above 0x80000000.
		if (aValue < 0x10000 || aValue >= 0xFFFF0000 || (aValue & 3)) return false;
		return !(aValue >= s_imageStart && aValue < s_imageEnd);
	}

	static bool ReadWord(const void* apAddress, UInt32& arValue) {
		__try {
			arValue = *(const UInt32*)apAddress;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Any Gamebryo texture, whatever its class: its renderer data (NiTexture +0x24) points back at it
	// (NiDX9TextureData +0x08). A two-way link no unrelated pair of words forms by chance.
	static bool IsTexture(const void* apObject) {
		UInt32 vtable = 0;
		if (!ReadWord(apObject, vtable)) return false;
		if (vtable == kVtblNiSourceTexture) return true;
		UInt32 rendererData = 0;
		UInt32 parent = 0;
		if (!ReadWord((const UInt8*)apObject + 0x24, rendererData) || !LooksLikePointer(rendererData)) return false;
		return ReadWord((const UInt8*)rendererData + 0x08, parent) && parent == (UInt32)apObject;
	}

	static IDirect3DBaseTexture9* D3DTextureOf(const void* apTexture) {
		UInt32 rendererData = 0;
		UInt32 d3dTexture = 0;
		if (!ReadWord((const UInt8*)apTexture + 0x24, rendererData) || !rendererData) return nullptr;
		if (!ReadWord((const UInt8*)rendererData + 0x64, d3dTexture)) return nullptr;
		return (IDirect3DBaseTexture9*)d3dTexture;
	}

	// NiSourceTexture's filename (+0x30), accepted only if it reads as a .dds path.
	static const char* FilenameOf(const void* apTexture, char* apBuffer, UInt32 aSize) {
		__try {
			const char* name = *(const char* const*)((const UInt8*)apTexture + 0x30);
			if (!name) return nullptr;
			UInt32 length = 0;
			for (; length < aSize - 1 && name[length]; length++) {
				char c = name[length];
				if (c < 0x20 || c > 0x7E) return nullptr;
				apBuffer[length] = c;
			}
			apBuffer[length] = 0;
			if (length < 5 || _stricmp(apBuffer + length - 4, ".dds")) return nullptr;
			return name;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	// Class name of a Gamebryo object, for the diagnostic dump: GetRTTI's code read, never called.
	static bool ObjectClassName(const void* apObject, char* apName, UInt32 aSize) {
		UInt32 vtable = 0;
		if (!ReadWord(apObject, vtable) || !(vtable >= s_imageStart && vtable < s_imageEnd)) return false;
		return ReadVtableRTTIName(vtable, apName, aSize);
	}

	struct SearchState {
		IDirect3DBaseTexture9* bound;
		const void* match;			// the texture bound for the draw
		const void* firstNamed;		// else the first texture with a .dds filename
		const void* visited[256];
		UInt32 visitedCount;
	};

	// Depth-limited walk of an object's first words for textures: a property or geometry holds its
	// texture directly or through arrays and map objects.
	static void Scan(const void* apObject, UInt32 aDepth, SearchState& arState) {
		for (UInt32 v = 0; v < arState.visitedCount; v++)
			if (arState.visited[v] == apObject) return;
		if (arState.visitedCount < 256) arState.visited[arState.visitedCount++] = apObject;

		const UInt32 width = aDepth >= 3 ? 32 : 16;
		for (UInt32 i = 0; i < width && !arState.match; i++) {
			UInt32 value = 0;
			if (!ReadWord((const UInt32*)apObject + i, value)) return;
			if (!LooksLikePointer(value)) continue;

			const void* candidate = (const void*)value;
			if (IsTexture(candidate)) {
				char name[MAX_PATH];
				if (arState.bound && D3DTextureOf(candidate) == arState.bound) arState.match = candidate;
				else if (!arState.firstNamed && FilenameOf(candidate, name, sizeof(name))) arState.firstNamed = candidate;
				continue;
			}
			if (aDepth > 1) Scan(candidate, aDepth - 1, arState);
		}
	}

	// Once: the Gamebryo objects within two steps of each root, by offset and class name, and every
	// texture met on the way. What the log needs to show where grass keeps its texture if the search
	// above ever comes up empty.
	static void DumpObject(const char* apLabel, const void* apObject, UInt32 aDepth, UInt32 aIndent) {
		char name[128] = "?";
		ObjectClassName(apObject, name, sizeof(name));
		Logger::Log("[GrassNormals] %*s%s %08X %s", aIndent * 2, "", apLabel, (UInt32)apObject, name);
		if (!aDepth) return;
		for (UInt32 i = 0; i < 32; i++) {
			UInt32 value = 0;
			if (!ReadWord((const UInt32*)apObject + i, value)) return;
			if (!LooksLikePointer(value)) continue;
			const void* child = (const void*)value;
			char label[32];
			sprintf_s(label, "+%02X", i * 4);
			if (IsTexture(child)) {
				char filename[MAX_PATH] = "(no filename)";
				FilenameOf(child, filename, sizeof(filename));
				Logger::Log("[GrassNormals] %*s%s texture %08X d3d %08X %s", (aIndent + 1) * 2, "", label, value, (UInt32)D3DTextureOf(child), filename);
				continue;
			}
			char childName[128];
			if (ObjectClassName(child, childName, sizeof(childName)) || aDepth > 1)
				DumpObject(label, child, aDepth - 1, aIndent + 1);
		}
	}

	static const void* CurrentGeometry() {
		UInt32 holder = 0;
		UInt32 geometry = 0;
		// The holder may be a static inside the executable, so only non-null is required of it.
		if (!ReadWord((const void*)0x011F91E0, holder) || holder < 0x10000) return nullptr;
		if (!ReadWord((const void*)holder, geometry) || !LooksLikePointer(geometry)) return nullptr;
		return (const void*)geometry;
	}

	static const void* FindTexture(const NiPropertyState* apProperties, IDirect3DBaseTexture9* apBound) {
		const void* texturing = apProperties->m_spTextureProperty;
		const void* shade = apProperties->m_spShadeProperty;
		const void* geometry = CurrentGeometry();
		UInt64 key = (((UInt64)(UInt32)texturing << 32) | (UInt32)shade) ^ ((UInt64)(UInt32)geometry * 0x9E3779B97F4A7C15ull);

		auto cached = s_propertyTextures.find(key);
		if (cached != s_propertyTextures.end()) {
			const PropertyEntry& entry = cached->second;
			if (!entry.texture) return nullptr;
			// Revalidated every time, since objects freed with their cell can be reallocated at the
			// same address with another texture: still a texture, and still the bound one if it was.
			if (IsTexture(entry.texture) && (!entry.matched || D3DTextureOf(entry.texture) == apBound))
				return entry.texture;
		}

		SearchState state = {};
		state.bound = apBound;
		if (texturing) Scan(texturing, 4, state);
		if (!state.match && shade) Scan(shade, 4, state);
		if (!state.match && geometry) Scan(geometry, 3, state);
		const void* result = state.match ? state.match : state.firstNamed;

		if (s_propertyTextures.size() > 8192) s_propertyTextures.clear();
		s_propertyTextures[key] = { result, state.match != nullptr };

		if (result && !s_loggedFirst) {
			s_loggedFirst = true;
			char filename[MAX_PATH] = "(no filename)";
			FilenameOf(result, filename, sizeof(filename));
			Logger::Log("[GrassNormals] first grass texture found: %s (%s)", filename,
				state.match ? "the texture bound for the draw" : "not the one bound yet; taken from the geometry");
		}
		if (!result && !s_loggedNoTexture) {
			s_loggedNoTexture = true;
			Logger::Log("[GrassNormals] could not find the texture of a grass geometry; its grass draws without a normal map. Dump follows (once):");
			Logger::Log("[GrassNormals] texture bound on stage 0: %08X", (UInt32)apBound);
			if (texturing) DumpObject("texturing property", texturing, 2, 0);
			if (shade) DumpObject("shade property", shade, 2, 0);
			if (geometry) DumpObject("geometry", geometry, 1, 0);
		}
		return result;
	}

	// ---- Normal map files ------------------------------------------------------------------------

	// textures\landscape\grass\foo.dds -> Data\Textures\landscape\grass\foo_n.dds
	static bool NormalMapPath(const char* apDiffuse, std::string& arPath) {
		std::string path = apDiffuse;
		std::transform(path.begin(), path.end(), path.begin(), [](char c) { return c == '/' ? '\\' : (char)tolower((unsigned char)c); });
		if (path.size() < 5 || path.compare(path.size() - 4, 4, ".dds")) return false;
		if (path.size() >= 6 && !path.compare(path.size() - 6, 6, "_n.dds")) return false;
		path.erase(path.size() - 4);
		path += "_n.dds";

		if (!path.compare(0, 5, "data\\")) arPath = path;
		else if (!path.compare(0, 9, "textures\\")) arPath = "Data\\" + path;
		else arPath = "Data\\Textures\\" + path;
		return true;
	}

	static IDirect3DTexture9* LoadNormalMap(const char* apDiffuse) {
		std::string path;
		if (!NormalMapPath(apDiffuse, path)) return nullptr;

		auto loaded = s_normalsByPath.find(path);
		if (loaded != s_normalsByPath.end()) return loaded->second;

		IDirect3DTexture9* texture = nullptr;
		if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
			Logger::Log("[GrassNormals] no normal map for %s (looked for %s)", apDiffuse, path.c_str());
		}
		else if (FAILED(D3DXCreateTextureFromFileA(TheRenderManager->device, path.c_str(), &texture))) {
			Logger::Log("[GrassNormals] could not load %s", path.c_str());
			texture = nullptr;
		}
		else {
			Logger::Log("[GrassNormals] loaded %s", path.c_str());
		}
		s_normalsByPath[path] = texture;
		return texture;
	}

	static IDirect3DTexture9* NormalMapFor(const void* apTexture) {
		char filename[MAX_PATH];
		const char* namePointer = FilenameOf(apTexture, filename, sizeof(filename));
		if (!namePointer) return nullptr;

		auto entry = s_textures.find(apTexture);
		if (entry != s_textures.end() && entry->second.path == namePointer)
			return entry->second.normal;

		IDirect3DTexture9* normal = LoadNormalMap(filename);
		if (s_textures.size() > 8192) s_textures.clear();
		s_textures[apTexture] = { namePointer, normal };
		return normal;
	}

	// ---- The hook ------------------------------------------------------------------------------------

	static void Bind(const NiPropertyState* apProperties) {
		GrassShaders* grass = TheShaderManager->Shaders.Grass;
		if (!grass || !grass->Enabled || !apProperties) return;

		NiDX9RenderState* renderState = TheRenderManager->renderState;
		IDirect3DTexture9* normal = nullptr;
		if (grass->NormalMapStrength > 0.0f) {
			const void* texture = FindTexture(apProperties, renderState->GetTexture(0));
			if (texture) normal = NormalMapFor(texture);
		}

		// x: strength, 0 when this grass has no map; y: green channel sign.
		float params[4] = { normal ? grass->NormalMapStrength : 0.0f, grass->NormalMapFlipGreen ? -1.0f : 1.0f, 0.0f, 0.0f };
		// Through the render state, not the device, so NiDX9RenderState's texture cache stays in step.
		// Unbound when this grass has no map, so nothing from another grass type is left on s10.
		renderState->SetTexture(kSampler, normal);
		if (normal) {
			renderState->SetSamplerState(kSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
			renderState->SetSamplerState(kSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
			renderState->SetSamplerState(kSampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR, false);
			renderState->SetSamplerState(kSampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR, false);
			renderState->SetSamplerState(kSampler, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR, false);
			renderState->SetSamplerState(kSampler, D3DSAMP_SRGBTEXTURE, FALSE, false);
		}
		TheRenderManager->device->SetPixelShaderConstantF(kParamsRegister, params, 1);
	}

	static void __fastcall TallGrassShader__UpdateConstants(TallGrassShader* apThis, void*, const NiPropertyState* apProperties) {
		ThisCall(s_detour.GetOverwrittenAddr(), apThis, apProperties);
		Bind(apProperties);
	}

	void Install() {
		if (s_installed) return;

		// Both vtables must be what they are taken for before an entry is replaced: SkyShader's at
		// the base the sky hook's slot implies, TallGrassShader's at class_vtbls.h's address.
		const UInt32 skyVtable = kSkyUpdateConstantsEntry - kUpdateConstantsSlot * 4;
		char skyName[64] = {};
		char grassName[64] = {};
		bool skyOk = ReadVtableRTTIName(skyVtable, skyName, sizeof(skyName)) && !strcmp(skyName, "SkyShader");
		bool grassOk = ReadVtableRTTIName(kVtblTallGrassShader, grassName, sizeof(grassName)) && !strcmp(grassName, "TallGrassShader");
		if (!skyOk || !grassOk) {
			Logger::Log("[GrassNormals] vtable check failed (sky '%s', grass '%s'); grass normal maps disabled", skyName, grassName);
			return;
		}

		HMODULE image = GetModuleHandleA(nullptr);
		IMAGE_NT_HEADERS* headers = (IMAGE_NT_HEADERS*)((UInt8*)image + ((IMAGE_DOS_HEADER*)image)->e_lfanew);
		s_imageStart = (UInt32)image;
		s_imageEnd = s_imageStart + headers->OptionalHeader.SizeOfImage;

		s_detour.ReplaceVirtualFunc(kVtblTallGrassShader + kUpdateConstantsSlot * 4, TallGrassShader__UpdateConstants);
		s_installed = true;
		Logger::Log("[GrassNormals] installed");
	}
}
