#pragma once

class FlashlightEffect : public EffectRecord
{
public:
	FlashlightEffect() : EffectRecord("Flashlight") {
		spotLightActive = false;
		SpotLight = nullptr;
	};

	struct FlashlightSettingsStruct {
		NiColor		Color;
		float		Dimmer;
		float		ConeAngle;
		float		Distance;
		bool		renderShadows;
		NiPoint3	Offset;
		bool		attachToWeapon;
		float		SpotSize;
		float		Intensity;
		float		NearFade;
		float		HotspotLimit;
		float		CookieStrength;
		bool		softEdges;

		// Forward re-light of nearby static geometry, drawn by MaterialPass
		struct MaterialLightStruct {
			bool	Enabled;
			float	Intensity;
			float	Specular;
			float	SpecularPower;
			float	SpecularBoost;
			float	NormalStrength;
			int		MaxGeometry;
			int		DebugMode;
		};
		MaterialLightStruct	MaterialLight;
	};
	FlashlightSettingsStruct	Settings;

	struct FlashlightStruct {
		D3DXMATRIX	FlashlightViewProj;
		D3DXVECTOR4	Position;
		D3DXVECTOR4	Direction;
		D3DXVECTOR4	Color;
		D3DXVECTOR4	Tuning;		// x spot size, y intensity, z near fade, w soft edges
		D3DXVECTOR4	Hotspot;	// x hotspot limit, y cookie strength, z source buffer is linear
	};
	FlashlightStruct	Constants;

	NiSpotLight* SpotLight;
	bool	spotLightActive;
	int		selectedPass;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();

	void	GetFlashlightViewProj();
};