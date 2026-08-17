#include "Flashlight.h"


void FlashlightEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_FlashLightViewProjTransform", (D3DXVECTOR4*)&Constants.FlashlightViewProj);
	TheShaderManager->RegisterConstant("TESR_FlashLightPosition", &Constants.Position);
	TheShaderManager->RegisterConstant("TESR_FlashLightDirection", &Constants.Direction);
	TheShaderManager->RegisterConstant("TESR_FlashLightColor", &Constants.Color);
	TheShaderManager->RegisterConstant("TESR_FlashLightTuning", &Constants.Tuning);
	TheShaderManager->RegisterConstant("TESR_FlashLightComposite", &Constants.Composite);
};

void FlashlightEffect::UpdateSettings() {

	Settings.attachToWeapon = TheSettingManager->GetSettingI("Shaders.Flashlight.Main", "AttachToWeapon");
	Settings.renderShadows = TheSettingManager->GetSettingI("Shaders.Flashlight.Main", "RenderShadows");
	selectedPass = Settings.renderShadows;

	Settings.Offset = NiPoint3(
		TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "OffsetX"),
		TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "OffsetY"),
		TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "OffsetZ")
	);

	Settings.Color = NiColor(
		TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "ColorR"),
		TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "ColorG"),
		TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "ColorB")
	);
	Settings.Dimmer = TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "Dimmer");
	Settings.ConeAngle = TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "Angle");
	Settings.Distance = TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "Distance");

	Settings.NearFade = TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "NearFade");
	Settings.HotspotLimit = TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "HotspotLimit");
	Settings.CookieStrength = TheSettingManager->GetSettingF("Shaders.Flashlight.Main", "CookieStrength");
	Settings.softEdges = TheSettingManager->GetSettingI("Shaders.Flashlight.Main", "SoftEdges");

	Settings.MaterialLight.Enabled = TheSettingManager->GetSettingI("Shaders.Flashlight.MaterialLight", "Enabled");
	Settings.MaterialLight.Intensity = TheSettingManager->GetSettingF("Shaders.Flashlight.MaterialLight", "Intensity");
	Settings.MaterialLight.Specular = TheSettingManager->GetSettingF("Shaders.Flashlight.MaterialLight", "Specular");
	Settings.MaterialLight.NormalStrength = TheSettingManager->GetSettingF("Shaders.Flashlight.MaterialLight", "NormalStrength");
	Settings.MaterialLight.MaxGeometry = TheSettingManager->GetSettingI("Shaders.Flashlight.MaterialLight", "MaxGeometry");
	Settings.MaterialLight.DebugMode = TheSettingManager->GetSettingI("Shaders.Flashlight.MaterialLight", "DebugMode");

	// With RenderPreTonemapping the effect chain draws onto the game's HDR scene surface,
	// whose content is already linear. Combine has to know, so it can skip the sRGB
	// decode that is only correct on the post tonemapping LDR surface.
	float sourceIsLinear = TheSettingManager->SettingsMain.Main.RenderPreTonemapping ? 1.0f : 0.0f;

	// These come purely from settings, so they are published here rather than in
	// UpdateConstants, which only runs while the effect is enabled
	Constants.Tuning = D3DXVECTOR4(Settings.NearFade, Settings.softEdges ? 1.0f : 0.0f, Settings.HotspotLimit, Settings.CookieStrength);
	Constants.Composite = D3DXVECTOR4(sourceIsLinear, 0.0f, 0.0f, 0.0f);

	if (!SpotLight)
		SpotLight = NiSpotLight::CreateObject();
};

void FlashlightEffect::UpdateConstants() {
	
	if (!SpotLight) return;

	// based on JIP https://github.com/jazzisparis/JIP-LN-NVSE/blob/5a30ac4356ea0e93b9ff357b5031b1e420240a4d/functions_jip/jip_fn_ui.h#L1469
	UInt32 lightIsOn = ThisStdCall<bool>(0x822B90, &Player->magicTarget, (void*)&(*(MagicItem**)(ThisStdCall<uint8_t*>(0x93CCD0, NULL) + 0x18)), 1);

	spotLightActive = Enabled && lightIsOn;
	if (!spotLightActive) {
		// disable light by setting it to 0 dimmer & radius
		SpotLight->Dimmer = 0;
		SpotLight->Spec = NiColor(0, 0, 0);
		SpotLight->CastShadows = false;
		PublishLightConstants(false);
		return;
	}

	NiPoint3 WeaponPos;
	NiMatrix33 WeaponRot;
	bool melee = false;
	if (Player->process->IsWeaponOut() && Player->ActorSkinInfo) {
		melee = !Player->ActorSkinInfo->WeaponForm ||
			Player->ActorSkinInfo->WeaponForm->weaponType == TESObjectWEAP::WeaponType::kWeapType_HandToHandMelee ||
			Player->ActorSkinInfo->WeaponForm->weaponType == TESObjectWEAP::WeaponType::kWeapType_OneHandMelee ||
			Player->ActorSkinInfo->WeaponForm->weaponType == TESObjectWEAP::WeaponType::kWeapType_TwoHandMelee ||
			Player->ActorSkinInfo->WeaponForm->weaponType == TESObjectWEAP::WeaponType::kWeapType_OneHandGrenade ||
			Player->ActorSkinInfo->WeaponForm->weaponType == TESObjectWEAP::WeaponType::kWeapType_OneHandMine ||
			Player->ActorSkinInfo->WeaponForm->weaponType == TESObjectWEAP::WeaponType::kWeapType_OneHandLunchboxMine ||
			Player->ActorSkinInfo->WeaponForm->weaponType == TESObjectWEAP::WeaponType::kWeapType_OneHandThrown;
	}

	// The bone the light rides must be chosen from what is equipped, never from the weapon
	// animation state. IsAiming() is false during the attack and follow-through states, so
	// testing it here made the light switch between two bones with different positions and
	// different forward axes on alternating frames while firing - that is the snapping.
	if (Player->isThirdPerson) {
		if (Settings.attachToWeapon && !melee && Player->process->IsWeaponOut() && Player->ActorSkinInfo && Player->ActorSkinInfo->WeaponNode) {
			WeaponPos = Player->ActorSkinInfo->WeaponNode->m_worldTransform.pos;
			WeaponRot = Player->ActorSkinInfo->WeaponNode->m_worldTransform.rot;
		}
		else if (Player->ActorSkinInfo && Player->ActorSkinInfo->HeadNode) {
			// matrix that will rotate 90 degrees on the Z then X axis
			NiMatrix33 rotation = NiMatrix33();
			rotation.data[0][0] = 0;
			rotation.data[0][1] = -1;
			rotation.data[0][2] = 0;
			rotation.data[1][0] = 0;
			rotation.data[1][1] = 0;
			rotation.data[1][2] = 1;
			rotation.data[2][0] = -1;
			rotation.data[2][1] = 0;
			rotation.data[2][2] = 0;

			WeaponPos = Player->ActorSkinInfo->HeadNode->m_worldTransform.pos;
			WeaponRot = Player->ActorSkinInfo->HeadNode->m_worldTransform.rot;
			rotation = rotation * WeaponRot; // we place the rotation matrix in the referential of the bone
			WeaponRot = WeaponRot * rotation; // we apply it
		}
		else {
			WeaponPos = WorldSceneGraph->camera->m_worldTransform.pos;
			WeaponRot = WorldSceneGraph->camera->m_worldTransform.rot;
		}
	}
	else {
		if (Settings.attachToWeapon && !melee && Player->process->IsWeaponOut() && Player->firstPersonSkinInfo && Player->firstPersonSkinInfo->WeaponNode) {
			WeaponPos = Player->firstPersonSkinInfo->WeaponNode->m_worldTransform.pos;
			WeaponRot = Player->firstPersonSkinInfo->WeaponNode->m_worldTransform.rot;
		}
		else {
			WeaponPos = WorldSceneGraph->camera->m_worldTransform.pos;
			WeaponRot = WorldSceneGraph->camera->m_worldTransform.rot;
		}
	}

	// rotate offset in the direction of the cone
	NiPoint3 offset = WeaponRot * Settings.Offset;
	WeaponPos.x += offset.x;
	WeaponPos.y += offset.y;
	WeaponPos.z += offset.z;

	if (spotLightActive) {
		// find and disable pipboy light
		NiNode* PlayerNode = Player->GetNode();
		for (UINT32 i = 0; i < PlayerNode->m_children.capacity; i++) {
			NiAVObject* childNode = PlayerNode->m_children.data[i];
			if (childNode) {
				if (childNode->GetRTTI() == (void*)0x11F4A98) {
					childNode->m_flags |= childNode->APP_CULLED;
				}
			}
		}
	}

	SpotLight->CastShadows = Settings.renderShadows;
	SpotLight->Diff = Settings.Color;
	SpotLight->Dimmer = Settings.Dimmer * 10.0;
	SpotLight->m_worldTransform.pos = WeaponPos;
	SpotLight->m_worldTransform.rot = WeaponRot;
	SpotLight->m_worldTransform.scale = 1.0f;
	SpotLight->OuterSpotAngle = Settings.ConeAngle;
	SpotLight->Spec = NiColor(Settings.Distance, 0, 0); // radius in r channel

	// Same packing as the shared TESR_SpotLight* constants: w carries radius, cone angle and dimmer
	Constants.Position = D3DXVECTOR4(WeaponPos.x, WeaponPos.y, WeaponPos.z, Settings.Distance);
	Constants.Direction = D3DXVECTOR4(WeaponRot.data[0][0], WeaponRot.data[1][0], WeaponRot.data[2][0], Settings.ConeAngle);
	Constants.Color = D3DXVECTOR4(Settings.Color.r, Settings.Color.g, Settings.Color.b, Settings.Dimmer);

	GetFlashlightViewProj();
	PublishLightConstants(true);
};

// The shared TESR_SpotLight* constants are published here rather than by the caller,
// because UpdateConstants runs more than once per frame - at frame start, again from
// GetNearbyLights during the shadow pass, and again after the world render. Each call
// re-reads the weapon bone. Publishing from the caller left the cone constants holding an
// earlier read than TESR_FlashLightViewProjTransform, so the projected cookie tracked a
// different snapshot than the cone it is projected into and slid against the beam as the
// weapon moved. Writing both from the same read removes that by construction.
void FlashlightEffect::PublishLightConstants(bool abActive) {
	if (!abActive) {
		D3DXVECTOR4 Empty = D3DXVECTOR4(0, 0, 0, 0);
		TheShaderManager->SpotLightPosition[0] = Empty;
		TheShaderManager->SpotLightDirection[0] = Empty;
		TheShaderManager->SpotLightColor[0] = Empty;
		TheShaderManager->VolumetricData = Empty;
		return;
	}

	TheShaderManager->SpotLightPosition[0] = SpotLight->m_worldTransform.pos.toD3DXVEC4();
	TheShaderManager->SpotLightPosition[0].w = SpotLight->Spec.r; // radius
	TheShaderManager->SpotLightDirection[0] = D3DXVECTOR4(
		SpotLight->m_worldTransform.rot.data[0][0],
		SpotLight->m_worldTransform.rot.data[1][0],
		SpotLight->m_worldTransform.rot.data[2][0],
		SpotLight->OuterSpotAngle); // outside angle of the light cone
	TheShaderManager->SpotLightColor[0] = D3DXVECTOR4(SpotLight->Diff.r, SpotLight->Diff.g, SpotLight->Diff.b, SpotLight->Dimmer);

	// The beam march needs parallax to produce a visible shaft. In first person the light
	// sits on the camera, so every view ray runs down the beam and there is nothing to see;
	// give the march a virtual origin pushed right and down in camera space while the
	// surface pool and the cookie keep the true position. w carries the first person
	// strength floor, which also blends the cookie flat in the march so the parallax
	// mismatch between the two positions does not show.
	FlashlightBeamEffect* Beam = TheShaderManager->Effects.FlashlightBeam;
	TheShaderManager->VolumetricData = TheShaderManager->SpotLightPosition[0];
	TheShaderManager->VolumetricData.w = 0.0f;
	if (Beam && Player && !Player->isThirdPerson && WorldSceneGraph && WorldSceneGraph->camera) {
		NiMatrix33& rot = WorldSceneGraph->camera->m_worldTransform.rot;
		float offR = Beam->Settings.FirstPersonOffsetRight;
		float offD = Beam->Settings.FirstPersonOffsetDown;
		TheShaderManager->VolumetricData.x += rot.data[0][2] * offR - rot.data[0][1] * offD;
		TheShaderManager->VolumetricData.y += rot.data[1][2] * offR - rot.data[1][1] * offD;
		TheShaderManager->VolumetricData.z += rot.data[2][2] * offR - rot.data[2][1] * offD;
		TheShaderManager->VolumetricData.w = Beam->Settings.FirstPersonStrength;
	}
}

bool FlashlightEffect::ShouldRender() { return SpotLight && spotLightActive; };


void FlashlightEffect::GetFlashlightViewProj() {
	if (!SpotLight) return;
	
	D3DXVECTOR3 Up = D3DXVECTOR3(0, 0, 1);
	D3DXVECTOR3 Eye = SpotLight->m_worldTransform.pos.toD3DXVEC3();
	D3DXVECTOR3 Direction = D3DXVECTOR3(SpotLight->m_worldTransform.rot.data[0][0], SpotLight->m_worldTransform.rot.data[1][0], SpotLight->m_worldTransform.rot.data[2][0]);
	D3DXVECTOR3 At = Eye + Direction;

	float Radius = SpotLight->Spec.r;
	D3DXMATRIX View, Proj;
	D3DXMatrixPerspectiveFovRH(&Proj, D3DXToRadian(SpotLight->OuterSpotAngle * 2), 1.0f, 0.1f, Radius);
	D3DXMatrixLookAtRH(&View, &Eye, &At, &Up);

	Constants.FlashlightViewProj = View * Proj;
}