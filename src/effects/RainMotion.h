#pragma once

// New rain effect: real world-space streak geometry (not a fullscreen post pass), so
// motion-reactivity is expressed as actual transforms on actual geometry rather than a
// procedural screen-space field. Deliberately independent of RainEffect/Precipitations.fx.hlsl --
// no shared state, no shared constants, no shared shader file.
//
// Streak placement uses a camera-following, toroidally-wrapped volume: every vertex derives its
// streak's local offset purely from a hash of its own instance index (no per-instance CPU data,
// no authored positions), then wraps that offset into a box re-centered on the LIVE camera
// position every frame. This is what keeps the rain from ever "running out" no matter how far
// the player walks -- there is no fixed origin or bounded extent to walk out of.
class RainMotionEffect : public EffectRecord
{
public:
	RainMotionEffect() : EffectRecord("RainMotion") {
		Constants.IntensityAnimator.Initialize(0);
		Constants.havePrevPos = false;
	};
	~RainMotionEffect();

	struct RainMotionStruct {
		Animator		IntensityAnimator;
		D3DXVECTOR4		Data;			// x: intensity, y: effective fall speed, z: streak length, w: streak width
		D3DXVECTOR4		Fall;			// xyz: normalized fall vector (world space), w: signed camera-whip shear
		D3DXVECTOR4		Volume;			// xyz: wrap volume size (Sx, Sy, Sz), w: streak count (float, informational)
		D3DXVECTOR4		Fade;			// x: fade start (fraction of half-extent), y: fade range, z: refraction strength, w: opacity

		// player-velocity tracking (position-delta based -- no velocity field exists on PlayerCharacter)
		NiPoint3		prevPlayerPos;
		bool			havePrevPos;
		D3DXVECTOR2		velocity;
		D3DXVECTOR2		oldVelocity;
		D3DXVECTOR2		oldoldVelocity;

		// camera yaw-delta tracking, same wrap-safe diff/smoothing MotionBlurEffect uses
		float			oldAngleZ;
		float			oldAngularDelta;
		float			oldoldAngularDelta;
	};
	RainMotionStruct	Constants;

	struct SettingsStruct {
		int				StreakCount;
		float			FallSpeed;
		float			StreakLength;
		float			StreakWidth;
		float			VolumeSizeXY;
		float			VolumeSizeZ;
		float			WindDriftSpeed;
		float			WindGustStrength;
		float			PlayerWindInfluence;
		float			CameraWhipShear;
		float			RefractionStrength;
		float			FadeStart;
		float			FadeRange;
		float			Opacity;
		float			TeleportSpeedThreshold;
		float			IntensityIncreaseRate;
		float			IntensityDecreaseRate;
	};
	SettingsStruct		Settings;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer);

private:
	IDirect3DVertexBuffer9*	StreakVertexBuffer = nullptr;
	IDirect3DIndexBuffer9*		StreakIndexBuffer = nullptr;
	int							BuiltStreakCount = 0;

	void	BuildGeometry(int streakCount);
	void	ReleaseGeometry();
};
