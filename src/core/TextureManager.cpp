#include <algorithm>


void TextureManager::Initialize() {

	Logger::Log("Starting the textures manager...");
	auto timer = TimeLogger();
	
	TheTextureManager = new TextureManager();

	IDirect3DDevice9* Device = TheRenderManager->device;
	UInt32 Width = TheRenderManager->width;
	UInt32 Height = TheRenderManager->height;
	
	// create textures used by NVR and bind them to surfaces
	TheTextureManager->InitTexture("TESR_SourceBuffer", &TheTextureManager->SourceTexture, &TheTextureManager->SourceSurface, Width, Height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_RenderedBuffer", &TheTextureManager->RenderedTexture, &TheTextureManager->RenderedSurface, Width, Height, D3DFMT_A16B16G16R16F);

	Device->CreateTexture(Width, Height, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &TheTextureManager->DepthTexture, NULL);
	TheTextureManager->RegisterTexture("TESR_DepthBufferWorld",(IDirect3DBaseTexture9**)&TheTextureManager->DepthTexture);
	Device->CreateTexture(Width, Height, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &TheTextureManager->DepthTextureViewModel, NULL);
	TheTextureManager->RegisterTexture("TESR_DepthBufferViewModel",(IDirect3DBaseTexture9**)&TheTextureManager->DepthTextureViewModel);

	NvAPI_D3D9_RegisterResource(TheTextureManager->DepthTexture);
	NvAPI_D3D9_RegisterResource(TheTextureManager->DepthTextureViewModel);

	TheTextureManager->RegisterTexture(WordWaterHeightMapBuffer, &TheTextureManager->WaterHeightMapB);
	TheTextureManager->RegisterTexture(WordWaterReflectionMapBuffer, &TheTextureManager->WaterReflectionMapB);

	timer.LogTime("TextureManager::Initialize");
}

/*
* Creates a texture of the given size and format and binds a surface to it, so it can be used as render target.
*/
void TextureManager::InitTexture(const char* Name, IDirect3DTexture9** Texture, IDirect3DSurface9** Surface, int Width, int Height, D3DFORMAT Format, bool mipmaps) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	// create a texture to receive the surface contents
	HRESULT create;
	if (!mipmaps)
		create = Device->CreateTexture(Width, Height, 1, D3DUSAGE_RENDERTARGET, Format, D3DPOOL_DEFAULT, Texture, NULL);
	else
		create = Device->CreateTexture(Width, Height, 0, D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP, Format, D3DPOOL_DEFAULT, Texture, NULL);

	if (FAILED(create)) {
		Logger::Log("[ERROR] : Failed to init texture %s", Name);
		return;
	}

	// set the surface level to the texture.
	(*Texture)->GetSurfaceLevel(0, Surface);
	RegisterTexture(Name, (IDirect3DBaseTexture9**)Texture);
}


/*
* Adds a texture to the list of sampler names recognized in shaders
*/
void TextureManager::RegisterTexture(const char* Name, IDirect3DBaseTexture9** Texture) {
	Logger::Log("Registering Texture %s", Name);
	TextureNames[Name] = Texture;
}

/*
* Gets a texture from the cache based on texture path
*/
IDirect3DBaseTexture9* TextureManager::GetCachedTexture(std::string& pathS) {
	TextureList::iterator t = TextureCache.find(pathS);
	if (t == TextureCache.end()) return nullptr;
	return t->second;
}


/*
* Gets a game dynamic texture by the sampler name
*/
IDirect3DBaseTexture9* TextureManager::GetTextureByName(std::string& Name) {
	TexturePointersList::iterator t = TextureNames.find(Name);
	if (t == TextureNames.end()) {
		Logger::Log("[ERROR] Texture %s not found.", Name.c_str());
		return nullptr;
	}
	return *(t->second);
}


static bool IsBlockCompressed(D3DFORMAT Format) {
	return Format == D3DFMT_DXT1 || Format == D3DFMT_DXT2 || Format == D3DFMT_DXT3 || Format == D3DFMT_DXT4 || Format == D3DFMT_DXT5;
}

/*
* True when every byte of a stored mip level is zero.
*/
static bool IsLevelEmpty(IDirect3DTexture9* Texture, UINT Level) {
	D3DSURFACE_DESC Desc;
	D3DLOCKED_RECT Locked;
	if (FAILED(Texture->GetLevelDesc(Level, &Desc)) || FAILED(Texture->LockRect(Level, &Locked, NULL, D3DLOCK_READONLY))) return false;
	const UINT Rows = IsBlockCompressed(Desc.Format) ? (Desc.Height + 3) / 4 : Desc.Height;
	bool Empty = true;
	for (UINT y = 0; y < Rows && Empty; y++) {
		const BYTE* Row = (const BYTE*)Locked.pBits + (size_t)y * Locked.Pitch;
		for (INT x = 0; x < Locked.Pitch && Empty; x++) Empty = !Row[x];
	}
	Texture->UnlockRect(Level);
	return Empty;
}

/*
* D3DX builds the smaller mip levels of a file that has none of its own (a PNG, for example) from the full-size level. Seen
* under DXVK: every smaller level comes back empty while the full-size level is intact, on a texture that changes from launch
* to launch, and D3DXFilterTexture on the loaded texture leaves them empty too. Sampling then blends in black as the mip level
* rises. D3DX does build a level correctly from a copy of the level above held in NVR's memory, so rebuild them that way.
*/
static void RepairMipLevels(IDirect3DTexture9* Texture, const std::string& TexturePath) {
	const DWORD Levels = Texture->GetLevelCount();
	if (Levels < 2 || IsLevelEmpty(Texture, 0) || !IsLevelEmpty(Texture, 1)) return;

	HRESULT Result = D3D_OK;
	for (DWORD Level = 1; Level < Levels && SUCCEEDED(Result); Level++) {
		D3DSURFACE_DESC Desc;
		D3DLOCKED_RECT Locked;
		Texture->GetLevelDesc(Level - 1, &Desc);
		Result = Texture->LockRect(Level - 1, &Locked, NULL, D3DLOCK_READONLY);
		if (FAILED(Result)) break;
		const UINT Rows = IsBlockCompressed(Desc.Format) ? (Desc.Height + 3) / 4 : Desc.Height;
		const std::vector<BYTE> Above((const BYTE*)Locked.pBits, (const BYTE*)Locked.pBits + (size_t)Locked.Pitch * Rows);
		const UINT Pitch = Locked.Pitch;
		Texture->UnlockRect(Level - 1);

		IDirect3DSurface9* Surface = nullptr;
		Result = Texture->GetSurfaceLevel(Level, &Surface);
		if (FAILED(Result)) break;
		const RECT Source = { 0, 0, (LONG)Desc.Width, (LONG)Desc.Height };
		Result = D3DXLoadSurfaceFromMemory(Surface, NULL, NULL, Above.data(), Desc.Format, Pitch, NULL, &Source, D3DX_FILTER_BOX, 0);
		Surface->Release();
	}

	if (FAILED(Result) || IsLevelEmpty(Texture, 1))
		Logger::Log("[ERROR] : Mip levels of %s came back empty and could not be rebuilt (%08X)", TexturePath.c_str(), (unsigned)Result);
	else
		Logger::Log("Mip levels of %s came back empty, rebuilt them", TexturePath.c_str());
}

/*
* Loads the actual texture file or get it from cache based on type/Name
*/
IDirect3DBaseTexture9* TextureManager::GetFileTexture(std::string TexturePath, TextureRecord::TextureRecordType Type) {

	IDirect3DBaseTexture9* Texture = GetCachedTexture(TexturePath);
	if (Texture) return Texture;

	switch (Type) {
	case TextureRecord::TextureRecordType::PlanarBuffer:
		D3DXCreateTextureFromFileA(TheRenderManager->device, TexturePath.data(), (IDirect3DTexture9**)&Texture);
		if (Texture) RepairMipLevels((IDirect3DTexture9*)Texture, TexturePath);
		break;
	case TextureRecord::TextureRecordType::VolumeBuffer:
		D3DXCreateVolumeTextureFromFileA(TheRenderManager->device, TexturePath.data(), (IDirect3DVolumeTexture9**)&Texture);
		break;
	case TextureRecord::TextureRecordType::CubeBuffer:
		D3DXCreateCubeTextureFromFileA(TheRenderManager->device, TexturePath.data(), (IDirect3DCubeTexture9**)&Texture);
		break;
	default:
		Logger::Log("[ERROR] : Invalid texture type %i for %s", Type, TexturePath);
	}

	if (!Texture) Logger::Log("[ERROR] : Couldn't load texture file %s", TexturePath);
	else Logger::Log("Loaded texture file %s", TexturePath);

	// add texture to cache
	TextureCache[TexturePath] = Texture;
	return Texture;
}


void TextureManager::SetWaterHeightMap(IDirect3DBaseTexture9* WaterHeightMap) {
    if (WaterHeightMapB == WaterHeightMap) return;
    WaterHeightMapB = WaterHeightMap;  //This may cause crashes on certain conditions
//    Logger::Log("Binding %0X", WaterHeightMap);
	for (WaterMapList::iterator it = TheTextureManager->WaterHeightMapTextures.begin(); it != TheTextureManager->WaterHeightMapTextures.end(); it++){
		 (*it)->Texture = WaterHeightMap;
	}
}


void TextureManager::SetWaterReflectionMap(IDirect3DBaseTexture9* WaterReflectionMap) {
    if (WaterReflectionMapB == WaterReflectionMap) return;
    WaterReflectionMapB = WaterReflectionMap;
//    Logger::Log("Binding %0X", WaterReflectionMap);
	for (WaterMapList::iterator it = TheTextureManager->WaterReflectionMapTextures.begin(); it != TheTextureManager->WaterReflectionMapTextures.end(); it++){
		 (*it)->Texture = WaterReflectionMap;
	}
}



/*
* Debug function to save the texture from the sampler into a "Test" folder
*/
void TextureManager::DumpToFile (IDirect3DTexture9* Texture, const char*  Name) {
	IDirect3DSurface9* Surface = nullptr;
	Texture->GetSurfaceLevel(0, &Surface);
	char path[32] = ".\\Test\\";
	strcat(path, Name);
	strcat(path, ".jpg");
	if (Surface) {
		D3DXSaveSurfaceToFileA(path, D3DXIFF_JPG, Surface, NULL, NULL);
		Surface->Release();
		Surface = nullptr;
	}
	else {
		Logger::Log("Surface is null for %s", Name);
	}
}