#include "ImGuiWidgets.h"
#include "imgui.h"
#include "imgui_internal.h" // ImClamp

namespace ImGuiWidgets {

	// ---- shared +/- quick-adjust state --------------------------------------

	static bool s_plusPressed  = false;
	static bool s_minusPressed = false;

	void SetStepKeys(bool plusPressed, bool minusPressed) {
		s_plusPressed  = plusPressed;
		s_minusPressed = minusPressed;
	}

	// ---- numeric widgets -----------------------------------------------------

	bool FloatSlider(const char* label, float* value, float min, float max, float step, bool showStepButtons) {
		float dragStep = step > 0.0f ? step : 0.001f;
		bool changed = ImGui::DragFloat(label, value, dragStep, min, max, "%.4f");

		if (ImGui::IsItemHovered() && (s_plusPressed || s_minusPressed)) {
			*value += s_plusPressed ? dragStep : -dragStep;
			if (max > min) *value = ImClamp(*value, min, max);
			changed = true;
		}

		if (showStepButtons) {
			ImGui::SameLine();
			if (ImGui::SmallButton("-")) {
				*value -= dragStep;
				if (max > min) *value = ImClamp(*value, min, max);
				changed = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("+")) {
				*value += dragStep;
				if (max > min) *value = ImClamp(*value, min, max);
				changed = true;
			}
		}

		return changed;
	}

	bool IntDrag(const char* label, int* value, int step) {
		int dragStep = step != 0 ? step : 1;
		bool changed = ImGui::DragInt(label, value, (float)dragStep);

		if (ImGui::IsItemHovered() && (s_plusPressed || s_minusPressed)) {
			*value += s_plusPressed ? dragStep : -dragStep;
			changed = true;
		}

		return changed;
	}

	bool EnumCombo(const char* label, const std::vector<std::string>& labels, int* index) {
		if (labels.empty()) return false;

		int clamped = ImClamp(*index, 0, (int)labels.size() - 1);
		bool changed = false;
		int newIndex = clamped;

		if (ImGui::BeginCombo(label, labels[clamped].c_str())) {
			for (int i = 0; i < (int)labels.size(); i++) {
				bool selected = (clamped == i);
				if (ImGui::Selectable(labels[i].c_str(), selected)) newIndex = i;
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		if (ImGui::IsItemHovered() && (s_plusPressed || s_minusPressed))
			newIndex = ImClamp(clamped + (s_plusPressed ? 1 : -1), 0, (int)labels.size() - 1);

		if (newIndex != *index) {
			*index = newIndex;
			changed = true;
		}

		return changed;
	}

	bool Checkbox(const char* label, bool* value) {
		return ImGui::Checkbox(label, value);
	}

	// ---- colour triple ---------------------------------------------------------

	bool ColorTriplePicker(const char* label, float rgb[3], ColorTripleState* state) {
		if (!state->seeded) {
			float r = rgb[0], g = rgb[1], b = rgb[2];
			float sc = r > g ? r : g;
			if (b > sc) sc = b;
			if (sc > 0.0f) {
				state->scale  = sc;
				state->col[0] = r / sc;
				state->col[1] = g / sc;
				state->col[2] = b / sc;
			} else {
				state->scale  = 1.0f;
				state->col[0] = state->col[1] = state->col[2] = 0.0f;
			}
			state->seeded = true;
		}

		ImGui::PushID(label);
		bool changed = false;

		// Raw R field, kept in sync with the normalised colour + intensity split.
		{
			float rawR = state->col[0] * state->scale;
			bool rChanged = ImGui::DragFloat("R", &rawR, 0.001f, 0.0f, 0.0f, "%.4f");
			if (ImGui::IsItemHovered() && (s_plusPressed || s_minusPressed)) {
				rawR += s_plusPressed ? 0.1f : -0.1f;
				rChanged = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("-##r")) { rawR -= 0.1f; rChanged = true; }
			ImGui::SameLine();
			if (ImGui::SmallButton("+##r")) { rawR += 0.1f; rChanged = true; }
			if (rChanged && state->scale > 0.0f) {
				state->col[0] = rawR / state->scale;
				changed = true;
			}
		}

		if (ImGui::ColorPicker3("##col", state->col,
			ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview))
			changed = true;

		ImGui::Text("Intensity");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		if (ImGui::DragFloat("##intensity", &state->scale, 0.01f, 0.0f, 0.0f, "%.4f"))
			changed = true;
		if (ImGui::IsItemHovered() && (s_plusPressed || s_minusPressed)) {
			state->scale += s_plusPressed ? 0.1f : -0.1f;
			changed = true;
		}

		ImGui::PopID();

		if (changed) {
			rgb[0] = state->col[0] * state->scale;
			rgb[1] = state->col[1] * state->scale;
			rgb[2] = state->col[2] * state->scale;
		}

		return changed;
	}

	// ---- key binding -----------------------------------------------------------

	static const struct { int dik; const char* name; } kDIKTable[] = {
		{ 0x01, "Escape" },
		{ 0x02, "1" }, { 0x03, "2" }, { 0x04, "3" }, { 0x05, "4" }, { 0x06, "5" },
		{ 0x07, "6" }, { 0x08, "7" }, { 0x09, "8" }, { 0x0A, "9" }, { 0x0B, "0" },
		{ 0x0C, "Minus (-)" }, { 0x0D, "Equals (=)" }, { 0x0E, "Backspace" },
		{ 0x0F, "Tab" },
		{ 0x10, "Q" }, { 0x11, "W" }, { 0x12, "E" }, { 0x13, "R" }, { 0x14, "T" },
		{ 0x15, "Y" }, { 0x16, "U" }, { 0x17, "I" }, { 0x18, "O" }, { 0x19, "P" },
		{ 0x1A, "[ Left Bracket" }, { 0x1B, "] Right Bracket" }, { 0x1C, "Enter" },
		{ 0x1D, "Left Ctrl" },
		{ 0x1E, "A" }, { 0x1F, "S" }, { 0x20, "D" }, { 0x21, "F" }, { 0x22, "G" },
		{ 0x23, "H" }, { 0x24, "J" }, { 0x25, "K" }, { 0x26, "L" },
		{ 0x27, "Semicolon (;)" }, { 0x28, "Apostrophe (')" }, { 0x29, "Grave (`)" },
		{ 0x2A, "Left Shift" }, { 0x2B, "Backslash (\\)" },
		{ 0x2C, "Z" }, { 0x2D, "X" }, { 0x2E, "C" }, { 0x2F, "V" },
		{ 0x30, "B" }, { 0x31, "N" }, { 0x32, "M" },
		{ 0x33, "Comma (,)" }, { 0x34, "Period (.)" }, { 0x35, "Slash (/)" },
		{ 0x36, "Right Shift" }, { 0x37, "Numpad *" }, { 0x38, "Left Alt" },
		{ 0x39, "Space" }, { 0x3A, "Caps Lock" },
		{ 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" },
		{ 0x3F, "F5" }, { 0x40, "F6" }, { 0x41, "F7" }, { 0x42, "F8" },
		{ 0x43, "F9" }, { 0x44, "F10" },
		{ 0x45, "Num Lock" }, { 0x46, "Scroll Lock" },
		{ 0x47, "Numpad 7" }, { 0x48, "Numpad 8" }, { 0x49, "Numpad 9" },
		{ 0x4A, "Numpad -" },
		{ 0x4B, "Numpad 4" }, { 0x4C, "Numpad 5" }, { 0x4D, "Numpad 6" },
		{ 0x4E, "Numpad +" },
		{ 0x4F, "Numpad 1" }, { 0x50, "Numpad 2" }, { 0x51, "Numpad 3" },
		{ 0x52, "Numpad 0" }, { 0x53, "Numpad ." },
		{ 0x57, "F11" }, { 0x58, "F12" },
		{ 0x9C, "Numpad Enter" }, { 0x9D, "Right Ctrl" },
		{ 0xB5, "Numpad /" }, { 0xB7, "Print Screen" }, { 0xB8, "Right Alt" },
		{ 0xC5, "Pause" }, { 0xC7, "Home" },
		{ 0xC8, "Up Arrow" }, { 0xC9, "Page Up" }, { 0xCB, "Left Arrow" },
		{ 0xCD, "Right Arrow" }, { 0xCF, "End" },
		{ 0xD0, "Down Arrow" }, { 0xD1, "Page Down" },
		{ 0xD2, "Insert" }, { 0xD3, "Delete" },
	};

	const char* DikName(int dik) {
		for (auto& e : kDIKTable)
			if (e.dik == dik) return e.name;
		return "Unknown";
	}

	static void RenderDIKPopup() {
		if (!ImGui::BeginPopup("DIKReference")) return;
		ImGui::Text("DirectInput Scancodes");
		ImGui::Separator();
		if (ImGui::BeginTable("diktbl", 2,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
			ImVec2(220.0f, 320.0f)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Code");
			ImGui::TableSetupColumn("Key");
			ImGui::TableHeadersRow();
			for (auto& e : kDIKTable) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0); ImGui::Text("0x%02X (%d)", e.dik, e.dik);
				ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(e.name);
			}
			ImGui::EndTable();
		}
		ImGui::EndPopup();
	}

	bool KeyBindPicker(const char* label, int* dik) {
		ImGui::PushID(label);
		int value = *dik;
		bool changed = ImGui::DragInt(label, &value, 1.0f, 0, 255);
		if (ImGui::IsItemHovered() && (s_plusPressed || s_minusPressed)) {
			value = ImClamp(value + (s_plusPressed ? 1 : -1), 0, 255);
			changed = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", DikName(value));
		ImGui::SameLine();
		if (ImGui::SmallButton("(?)"))
			ImGui::OpenPopup("DIKReference");
		RenderDIKPopup();
		ImGui::PopID();

		if (changed) *dik = value;
		return changed;
	}

	// ---- file cycle picker ------------------------------------------------------

	bool FilePicker(const char* label, const std::vector<std::string>& files, int* index) {
		if (files.empty()) return false;

		int n = (int)files.size();
		int clamped = ((*index % n) + n) % n;

		ImGui::PushID(label);
		ImGui::BeginGroup();
		ImGui::Text("%s", label);
		ImGui::SameLine();
		int delta = 0;
		if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) delta = -1;
		ImGui::SameLine();
		float nameStartX = ImGui::GetCursorPosX();
		ImGui::TextUnformatted(files[clamped].c_str());
		ImGui::SameLine(nameStartX + 220.0f);
		if (ImGui::ArrowButton("##next", ImGuiDir_Right)) delta = 1;
		ImGui::EndGroup();
		if (ImGui::IsItemHovered() && (s_plusPressed || s_minusPressed))
			delta = s_plusPressed ? 1 : -1;
		ImGui::PopID();

		if (delta == 0) return false;

		*index = ((clamped + delta) % n + n) % n;
		return true;
	}

	// ---- chrome ------------------------------------------------------------------

	void TooltipIfHovered(const char* text) {
		if (!text || !text[0]) return;
		if (!ImGui::IsItemHovered()) return;
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	bool RevertButton(bool isDirty, const char* tooltipText) {
		ImGui::SameLine();
		if (!isDirty) ImGui::BeginDisabled();
		bool clicked = ImGui::SmallButton("~");
		if (!isDirty) ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltipText);
		return clicked;
	}

} // namespace ImGuiWidgets
