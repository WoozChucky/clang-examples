#include "EditorUIImpl.h"
#include <imgui.h>
#include <array>

namespace {

bool UI_DragFloat(nlohmann::json& o, const char* k, float speed) {
    if (!o.contains(k) || !o[k].is_number()) return false;
    float v = o[k].get<float>();
    if (ImGui::DragFloat(k, &v, speed)) { o[k] = v; return true; }
    return false;
}

// Reads N float members from a json OBJECT under `k` using the given member keys (e.g. X,Y,Z),
// draws a DragFloatN, writes back. Returns changed. No-op if shape mismatches.
template <int N>
bool DragVecN(nlohmann::json& o, const char* k, const char* const (&members)[N], float speed) {
    if (!o.contains(k) || !o[k].is_object()) return false;
    float v[N];
    for (int i = 0; i < N; ++i) {
        if (!o[k].contains(members[i]) || !o[k][members[i]].is_number()) return false;
        v[i] = o[k][members[i]].get<float>();
    }
    bool changed = false;
    if      constexpr (N == 2) changed = ImGui::DragFloat2(k, v, speed);
    else if constexpr (N == 3) changed = ImGui::DragFloat3(k, v, speed);
    else if constexpr (N == 4) changed = ImGui::DragFloat4(k, v, speed);
    if (changed) for (int i = 0; i < N; ++i) o[k][members[i]] = v[i];
    return changed;
}
bool UI_DragFloat2(nlohmann::json& o, const char* k, float s){ static const char* m[2]={"X","Y"};       return DragVecN(o,k,m,s); }
bool UI_DragFloat3(nlohmann::json& o, const char* k, float s){ static const char* m[3]={"X","Y","Z"};   return DragVecN(o,k,m,s); }
bool UI_DragFloat4(nlohmann::json& o, const char* k, float s){ static const char* m[4]={"X","Y","Z","W"};return DragVecN(o,k,m,s); }

bool UI_InputInt(nlohmann::json& o, const char* k) {
    if (!o.contains(k) || !o[k].is_number_integer()) return false;
    int v = o[k].get<int>();
    if (ImGui::InputInt(k, &v)) { o[k] = v; return true; }
    return false;
}
bool UI_Checkbox(nlohmann::json& o, const char* k) {
    if (!o.contains(k) || !o[k].is_boolean()) return false;
    bool v = o[k].get<bool>();
    if (ImGui::Checkbox(k, &v)) { o[k] = v; return true; }
    return false;
}
bool UI_Combo(nlohmann::json& o, const char* k, const char* const* labels, int count) {
    if (!o.contains(k) || !o[k].is_number_integer()) return false;
    int v = o[k].get<int>();
    if (v < 0 || v >= count) return false;
    if (ImGui::Combo(k, &v, labels, count)) { o[k] = v; return true; }
    return false;
}
// Color: try {R,G,B[,A]} then {X,Y,Z[,W]} member sets.
template <int N>
bool ColorN(nlohmann::json& o, const char* k) {
    if (!o.contains(k) || !o[k].is_object()) return false;
    static const char* rgba[4] = {"R","G","B","A"};
    static const char* xyzw[4] = {"X","Y","Z","W"};
    const char* const* m = (o[k].contains("R")) ? rgba : xyzw;
    float v[N];
    for (int i = 0; i < N; ++i) {
        if (!o[k].contains(m[i]) || !o[k][m[i]].is_number()) return false;
        v[i] = o[k][m[i]].get<float>();
    }
    bool changed = (N == 4) ? ImGui::ColorEdit4(k, v) : ImGui::ColorEdit3(k, v);
    if (changed) for (int i = 0; i < N; ++i) o[k][m[i]] = v[i];
    return changed;
}
bool UI_ColorEdit3(nlohmann::json& o, const char* k){ return ColorN<3>(o,k); }
bool UI_ColorEdit4(nlohmann::json& o, const char* k){ return ColorN<4>(o,k); }

bool UI_ComboMapped(nlohmann::json& o, const char* k,
                    const char* const* labels, const int* values, int count) {
    if (!o.contains(k) || !o[k].is_number_integer()) return false;
    const int cur = o[k].get<int>();
    int idx = 0;
    for (int i = 0; i < count; ++i) if (values[i] == cur) { idx = i; break; }
    if (ImGui::Combo(k, &idx, labels, count)) { o[k] = values[idx]; return true; }
    return false;
}

void UI_Text(const char* t){ ImGui::TextUnformatted(t); }
void UI_Separator(){ ImGui::Separator(); }
void UI_SameLine(){ ImGui::SameLine(); }
bool UI_Button(const char* l){ return ImGui::Button(l); }

} // namespace

const EditorUI& EditorUIInstance() {
    static const EditorUI ui{
        &UI_DragFloat, &UI_DragFloat2, &UI_DragFloat3, &UI_DragFloat4,
        &UI_InputInt, &UI_Checkbox, &UI_Combo, &UI_ColorEdit3, &UI_ColorEdit4,
        &UI_Text, &UI_Separator, &UI_SameLine, &UI_Button, &UI_ComboMapped
    };
    return ui;
}
