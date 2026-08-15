#pragma once

// Pure ImGui widget helpers for the NVR settings overlay.
//
// These functions know ImGui and DirectInput scancodes -- nothing about
// EffectRecord, SettingManager, or TheShaderManager. Callers own persistence
// (reading/writing settings) and any effect-specific state; these functions
// only render a widget against the values they're given and report whether
// something changed. No function in this file may reference TheSettingManager,
// TheShaderManager, EffectRecord, or any SettingManager::Configuration type.
//
// Per docs/refactor-plan.md R3b.

namespace ImGuiWidgets {

	// Feeds the shared +/- quick-adjust key state (hovering a numeric widget
	// while the key is held nudges its value by one `step`) used by
	// FloatSlider/IntDrag/ColorTriplePicker below. Call once per frame, before
	// rendering any widgets, with edge-triggered "just pressed" booleans.
	void SetStepKeys(bool plusPressed, bool minusPressed);

	// User-configurable fallback quick-adjust step (the dev tools panel's
	// "step size" control), used by callers rendering a setting with no
	// annotation-supplied step of its own. Call once per frame alongside
	// SetStepKeys(); defaults to 0.1 if never set.
	void SetDefaultStep(float step);
	float GetDefaultStep();

	// Labelled float control. `value` is read/written in place. `min`/`max`
	// both 0.0 means unbounded (matches ImGui::DragFloat's own convention).
	// `dragStep` is the mouse-drag speed; `quickAdjustStep` is the increment
	// applied by hovering + the +/- key, and by the explicit "-"/"+" buttons
	// when `showStepButtons` is set (these are the same value for an
	// annotated uniform, whose single `step` field controls both per
	// docs/refactor-plan.md -- pass the same value for both to get that).
	// If `outHovered` is supplied, it's set to whether the drag field itself
	// (not the trailing +/- buttons) is hovered this frame -- ImGui's own
	// IsItemHovered() called after this function returns would report on
	// whichever of those was drawn last instead, which is almost never what
	// a caller wants for e.g. a tooltip. Returns true if the value changed.
	bool FloatSlider(const char* label, float* value, float min, float max, float dragStep, float quickAdjustStep, bool showStepButtons, bool* outHovered = nullptr);

	// Labelled int control. `min`/`max` both 0 means unbounded. `useSlider`
	// renders ImGui::SliderInt (clamped drag) instead of the DragInt default --
	// this is what the annotation format's `slider` widget hint means: an
	// explicit slider overriding the drag-int type-default (requires a real
	// min/max to have any effect). Returns true if the value changed.
	bool IntDrag(const char* label, int* value, int min, int max, int step, bool useSlider);

	// Dropdown of `labels`, backed by a zero-based `index`. Returns true if
	// the selection changed. Out-of-range indices are clamped before display.
	bool EnumCombo(const char* label, const std::vector<std::string>& labels, int* index);

	// Checkbox. Returns true if toggled.
	bool Checkbox(const char* label, bool* value);

	// RGB colour-wheel picker plus an intensity drag field, backed by a
	// caller-owned persistent decomposition. Re-deriving hue/saturation from
	// raw RGB every frame pins the picker dot to the wheel's edge and resets
	// the intensity slider, so the (col, scale) split is kept in `state`
	// across calls instead of being recomputed each frame.
	struct ColorTripleState {
		float col[3] = { 0.0f, 0.0f, 0.0f };
		float scale  = 1.0f;
		bool  seeded = false; // false until first-seeded from a caller-supplied rgb
	};
	// `rgb` is the raw (possibly > 1.0) colour in/out. On the first call for a
	// given `state`, `rgb` seeds col/scale; afterwards `state` is authoritative
	// and `rgb` is only written to, never read. If `outHovered` is supplied,
	// it's set to whether the mouse is over any part of this widget (the R
	// field, the wheel, or the intensity field) -- the whole group counts as
	// one hover target, unlike FloatSlider/KeyBindPicker where only the
	// primary field does. Returns true if changed.
	bool ColorTriplePicker(const char* label, float rgb[3], ColorTripleState* state, bool* outHovered = nullptr);

	// DIK scancode field plus a "(?)" popup listing the DirectInput scancode
	// reference table. `dik` is read/written in place (0-255). If `outHovered`
	// is supplied, it's set to whether the scancode field itself (not the
	// trailing name label or "(?)" button) is hovered this frame -- see
	// FloatSlider's `outHovered` for why that distinction matters. Returns
	// true if the value changed.
	bool KeyBindPicker(const char* label, int* dik, bool* outHovered = nullptr);

	// Prev/name/next file-cycle picker. `files` is the list of filenames to
	// cycle through -- this function does no filesystem access, the caller is
	// responsible for scanning the folder. `index` is read/written in place
	// and wraps at either end. Returns true if the selection changed.
	bool FilePicker(const char* label, const std::vector<std::string>& files, int* index);

	// Wrapped tooltip, shown when `hovered` is true and `text` is non-empty/
	// non-null. No-op otherwise. Takes hover state as an explicit parameter
	// rather than calling ImGui::IsItemHovered() itself: that only reports on
	// the single most-recently-submitted ImGui item, which for any widget
	// above that draws more than one (FloatSlider's step buttons,
	// ColorTriplePicker, KeyBindPicker's popup button) is not the item the
	// caller means by "hovered" -- making the caller pass the right bool in
	// (via those functions' own outHovered parameters, or a plain
	// IsItemHovered() call for single-item widgets) avoids silently
	// attaching the tooltip to the wrong element.
	void TooltipIfHovered(bool hovered, const char* text);

	// Small "~" button rendered inline after the previously-drawn item;
	// disabled when `isDirty` is false. Returns true if clicked -- this
	// function doesn't know what "dirty" means, the caller compares current
	// value against whatever snapshot it's keeping.
	bool RevertButton(bool isDirty, const char* tooltipText = "Revert to value at session start");

	// DirectInput scancode -> display name (e.g. 0x1E -> "A"), "Unknown" if
	// not in the table. Exposed so callers can label a bound key without
	// duplicating the table.
	const char* DikName(int dik);

} // namespace ImGuiWidgets
