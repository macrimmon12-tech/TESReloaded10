#pragma once

class TextureRecord {
public:
	TextureRecord();

	enum TextureRecordType {
		None,
		PlanarBuffer,
		VolumeBuffer,
		CubeBuffer
	};
	static TextureRecord*		GetTextureRecord(const char* Name, std::string TexturePath);
	static TextureRecordType	GetTextureType(UINT Type);

	void						GetSamplerStates(std::string samplerStateSubstring);
	bool						BindTexture(const char* Name);
	bool						LoadTexture(TextureRecordType Type, const char* TexturePath);

	IDirect3DBaseTexture9*		Texture;
	// For named render textures: the TextureManager slot the texture came from. Effects re-read
	// it on every bind, so a slot whose texture is swapped (the frame chain's rendered buffer)
	// is followed instead of the pointer cached at first bind.
	IDirect3DBaseTexture9**		TextureRef = nullptr;
	DWORD						SamplerStates[SamplerStatesMax];
};
