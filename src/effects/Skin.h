#pragma once

class SkinShaders : public ShaderCollection
{
public:
	SkinShaders() : ShaderCollection("Skin") {};

	// Set from NewVegasReloaded/Main.cpp when VanillaPlusSkin.dll is detected, same soft-
	// detection pattern as AmbientOcclusion's bNVAOLoaded. VPS ships its own skin lighting
	// model (a subsurface-scattering LUT) through its own NVSE plugin, using the same vanilla
	// shader names ("SKIN2000.vso" etc.) that this collection's own hand-authored/decompiled
	// replacements are keyed on. Since the two can't blend at the file level, Templates()
	// below routes every SKIN name to SkinVPSTemplate.hlsl instead when this is set, which
	// reproduces VPS's own lighting model and adds NVR's forward sun-shadow atlas -- the one
	// thing VPS's own shader has no access to.
	bool bVPSLoaded = false;

	struct SkinStruct {
		D3DXVECTOR4		SkinData;
		D3DXVECTOR4		SkinColor;
	};
	SkinStruct Constants;

	std::map<std::string_view, ShaderTemplate> Templates() {
		if (!bVPSLoaded) return std::map<std::string_view, ShaderTemplate>();

		return std::map<std::string_view, ShaderTemplate>{
			// Vertex shaders: ADTS Base/SKIN family, plus AD's LIGHTS=3/SKIN (ONLY_LIGHT).
			{ "SKIN2000.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}} } },
			{ "SKIN2001.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"SKIN", ""}} } },
			{ "SKIN2005.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"LIGHTS", "2"}, {"SKIN", ""}} } },
			{ "SKIN2013.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}, {"SKIN", ""}} } },
			// Vertex shaders: ADTS10 many-lights family, pairs with the SKIN2010-2012 PS below.
			// NVR never shipped these -- without a matching VS, those PS names' shadowWorldPos
			// interpolator holds whatever the underlying vanilla/VPS VS left there, and
			// SHADOW_VS_PRESENT correctly declines to shadow off that garbage. That's the
			// "otherwise looks right, just never shadowed" report for busy multi-light interior
			// scenes (e.g. Raider camps with several torches/lanterns in view at once). The VS
			// body doesn't actually vary by LIGHTS -- these compile identically to 2000/2001
			// above -- but the engine looks shaders up by exact name, so each still needs its
			// own entry.
			{ "SKIN2020.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"LIGHTS", "9"}} } },
			{ "SKIN2021.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"LIGHTS", "9"}, {"SKIN", ""}} } },
			{ "SKIN2022.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"LIGHTS", "4"}} } },
			{ "SKIN2023.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"LIGHTS", "4"}, {"SKIN", ""}} } },
			{ "SKIN2024.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"LIGHTS", "4"}, {"SPECULAR", ""}} } },
			{ "SKIN2025.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"LIGHTS", "4"}, {"SPECULAR", ""}, {"SKIN", ""}} } },

			// Pixel shaders: ADTS Base/PROJ_SHADOW/LIGHTS=2 family.
			{ "SKIN2000.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}} } },
			{ "SKIN2001.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2002.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"LIGHTS", "2"}} } },
			{ "SKIN2003.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			// Pixel shaders: AD's ONLY_LIGHT LIGHTS=2/3 family.
			{ "SKIN2004.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "2"}} } },
			{ "SKIN2005.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2006.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}} } },
			{ "SKIN2007.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}, {"PROJ_SHADOW", ""}} } },
			// Pixel shaders: DiffusePt point-light-only family.
			{ "SKIN2008.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "2"}} } },
			{ "SKIN2009.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "3"}} } },
			// Pixel shaders: ADTS10 many-lights family. SKIN2012 (SPECULAR -- the grime/sheen
			// term) was disabled upstream for a compile bug in the old decompiled file; our
			// template doesn't share that bug, so it's safe to route here too.
			{ "SKIN2010.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"LIGHTS", "9"}} } },
			{ "SKIN2011.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"LIGHTS", "4"}} } },
			{ "SKIN2012.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"LIGHTS", "4"}, {"SPECULAR", ""}} } },
		};
	};

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};