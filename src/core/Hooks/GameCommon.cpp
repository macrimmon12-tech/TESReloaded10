#pragma once

NiDX9Renderer* (__thiscall* InitializeRenderer)(NiDX9Renderer*) = (NiDX9Renderer* (__thiscall*)(NiDX9Renderer*))Hooks::InitializeRenderer;
NiDX9Renderer* __fastcall InitializeRendererHook(NiDX9Renderer* This, UInt32 edx) {

	TheRenderManager = (RenderManager*)(*InitializeRenderer)(This);
	TheRenderManager->Initialize();
	InitializeManagers();

	// Vanilla Plus Skin detection has to happen here, not in NVSE's kMessage_DeferredInit --
	// the game creates every SKIN20xx shader (via the CreateVertexShader/CreatePixelShader
	// hooks) as part of the same startup sequence this hook belongs to, well before
	// DeferredInit ever fires. Checking there was always too late: bVPSLoaded would still be
	// false by the time SkinShaders::Templates() got consulted, so every SKIN shader loaded
	// from the old standalone/decompiled files regardless of whether VPS was installed.
	if (GetModuleHandle(L"VanillaPlusSkin.dll")) {
		TheShaderManager->Shaders.Skin->bVPSLoaded = true;
		Logger::Log("Vanilla Plus Skin found, routing SKIN shaders through SkinVPSTemplate");
	}
	else {
		Logger::Log("Vanilla Plus Skin not found");
	}

	return TheRenderManager;

}

PlayerCharacter* (__thiscall* NewPlayerCharacter)(PlayerCharacter*) = (PlayerCharacter* (__thiscall*)(PlayerCharacter*))Hooks::NewPlayerCharacter;
PlayerCharacter* __fastcall NewPlayerCharacterHook(PlayerCharacter* This, UInt32 edx) {

	Player = (*NewPlayerCharacter)(This);
	return Player;

}

SceneGraph* (__thiscall* NewSceneGraph)(SceneGraph*, char*, UInt8, NiCamera*) = (SceneGraph* (__thiscall*)(SceneGraph*, char*, UInt8, NiCamera*))Hooks::NewSceneGraph;
SceneGraph* __fastcall NewSceneGraphHook(SceneGraph* This, UInt32 edx, char* Name, UInt8 IsMinFarPlaneDistance, NiCamera* Camera) {
	
	SceneGraph* SG = (*NewSceneGraph)(This, Name, IsMinFarPlaneDistance, Camera);
	
	if (!strcmp(Name, "World")) WorldSceneGraph = SG;
	return SG;

}

MenuInterfaceManager* (__thiscall* NewMenuInterfaceManager)(MenuInterfaceManager*) = (MenuInterfaceManager* (__thiscall*)(MenuInterfaceManager*))Hooks::NewMenuInterfaceManager;
MenuInterfaceManager* __fastcall NewMenuInterfaceManagerHook(MenuInterfaceManager* This, UInt32 edx) {

	InterfaceManager = (*NewMenuInterfaceManager)(This);
	return InterfaceManager;

}

QueuedModelLoader* (__thiscall* NewQueuedModelLoader)(QueuedModelLoader*) = (QueuedModelLoader* (__thiscall*)(QueuedModelLoader*))Hooks::NewQueuedModelLoader;
QueuedModelLoader* __fastcall NewQueuedModelLoaderHook(QueuedModelLoader* This, UInt32 edx) {

	ModelLoader = (*NewQueuedModelLoader)(This);
	return ModelLoader;

}