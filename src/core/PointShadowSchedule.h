#pragma once

// Decides which point-light shadow cubemap slots are redrawn on a frame.
//
// Every slot's cubemap is rendered from scratch each frame by default. With an update interval N > 1
// a slot is redrawn only every N frames (staggered by slot so the cost is spread out) and keeps its
// previous contents in between -- but only while it still describes the same light: the slot must hold
// the same light object at the same position and radius, in the same cell, and the same texture, or it
// is redrawn at once. It also reuses a complete all-static caster set indefinitely and invalidates it
// from a hash of geometry identity, transforms, bounds, visibility and material alpha. A list containing
// actors/animated leaves keeps the old interval schedule. Kept free of engine types so tests can check it.
struct PointShadowSlotState {
	const void* light = nullptr;    // the ShadowSceneLight the slot was last drawn for
	const void* texture = nullptr;  // the slot's cubemap texture (a device reset creates a new one)
	const void* cell = nullptr;     // the cell the player was in
	float x = 0, y = 0, z = 0;      // light position
	float radius = 0;               // light radius as used for the cubemap
	unsigned long long casterHash = 0; // static caster pointers/transforms/visibility/material state
	bool staticCasters = false;      // complete geometry list, with no skinned or wind-animated geometry
	bool valid = false;             // the slot has been drawn at least once
};

// The game rebuilds a light's geometry list and does not promise stable traversal order. Fold each
// caster's own state hash into a commutative set hash so the same casters do not invalidate a cache.
inline unsigned long long PointShadowAddCasterHash(unsigned long long setHash, unsigned long long casterHash)
{
	return setHash + casterHash * 0x9E3779B185EBCA87ULL;
}

// Why a slot is redrawn (None = it keeps its cubemap this frame). When several apply, the first listed wins.
enum class PointShadowRedraw {
	None,
	NewSlot,      // the slot was never drawn (first frame, or the light appeared)
	OtherLight,   // the slot now holds a different light than the one it was drawn for
	OtherTexture, // the cubemap texture was recreated (device reset)
	OtherCell,    // the player changed cell
	Moved,        // the light moved
	Resized,      // the light's radius changed
	CastersChanged, // a fully static caster set changed, or changed between static and dynamic
	Scheduled,    // nothing changed: the regular every-N-frames refresh (every frame at interval 1)
	Count
};

inline PointShadowRedraw PointShadowRedrawReason(const PointShadowSlotState& last, const PointShadowSlotState& now,
	unsigned frame, unsigned slot, unsigned interval)
{
	if (!last.valid) return PointShadowRedraw::NewSlot;
	if (last.light != now.light) return PointShadowRedraw::OtherLight;
	if (last.texture != now.texture) return PointShadowRedraw::OtherTexture;
	if (last.cell != now.cell) return PointShadowRedraw::OtherCell;
	if (last.x != now.x || last.y != now.y || last.z != now.z) return PointShadowRedraw::Moved;
	if (last.radius != now.radius) return PointShadowRedraw::Resized;
	if (last.staticCasters != now.staticCasters) return PointShadowRedraw::CastersChanged;
	if (now.staticCasters && last.casterHash != now.casterHash) return PointShadowRedraw::CastersChanged;
	// A complete, unchanged static caster set is camera-independent, so its existing cubemap is final.
	// Dynamic/unknown sets keep the existing PointShadowInterval schedule unchanged.
	if (now.staticCasters) return PointShadowRedraw::None;
	if (interval <= 1 || (frame + slot) % interval == 0) return PointShadowRedraw::Scheduled;
	return PointShadowRedraw::None;
}

inline bool PointShadowSlotChanged(const PointShadowSlotState& last, const PointShadowSlotState& now)
{
	const PointShadowRedraw why = PointShadowRedrawReason(last, now, 0, 0, 0);
	return why != PointShadowRedraw::Scheduled && why != PointShadowRedraw::None;
}

inline bool PointShadowNeedsRedraw(const PointShadowSlotState& last, const PointShadowSlotState& now,
	unsigned frame, unsigned slot, unsigned interval)
{
	return PointShadowRedrawReason(last, now, frame, slot, interval) != PointShadowRedraw::None;
}
