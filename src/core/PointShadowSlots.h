#pragma once

// Keeps every shadow-casting point light in the same cubemap slot from one frame to the next.
//
// ShaderManager::GetNearbyLights ranks the lights by their distance to the player. Handing rank r to slot r
// means that when the player walks and two lights of similar distance swap ranks, both swap slots, and each
// swap makes both cubemaps be redrawn at once (a slot's cubemap describes one light; see
// PointShadowSchedule.h) -- work that PointShadowInterval was meant to save. The order of the slots does not
// change the image (PointShadows.fx adds up the slots), so a light keeps the slot it had and only newcomers
// take free ones. Kept free of engine types so tests/point_shadow_schedule.cpp can check it.
constexpr int PointShadowSlotsMax = 16;

// previous[0..slots): the light that held each slot on the last call (nullptr = free).
// ranked[0..count):   the lights that get a slot now, nearest first; distinct, non-null, count <= slots <= PointShadowSlotsMax.
// out[0..slots):      each ranked light in the slot it held before, if it still holds one; the others in the lowest
//                     free slots in rank order; slots nobody uses are nullptr.
inline void AssignStablePointShadowSlots(const void* const* previous, const void* const* ranked, int count, int slots,
	const void** out)
{
	bool placed[PointShadowSlotsMax] = {};
	for (int s = 0; s < slots; s++) out[s] = nullptr;
	for (int s = 0; s < slots; s++) {
		if (!previous[s]) continue;
		for (int r = 0; r < count; r++) {
			if (placed[r] || ranked[r] != previous[s]) continue;
			out[s] = ranked[r];
			placed[r] = true;
			break;
		}
	}
	int freeSlot = 0;
	for (int r = 0; r < count; r++) {
		if (placed[r]) continue;
		while (freeSlot < slots && out[freeSlot]) freeSlot++;
		if (freeSlot >= slots) return; // more lights than slots: the caller broke the contract, drop the farthest
		out[freeSlot++] = ranked[r];
	}
}
