#pragma once

#include "imgui.h"

#ifndef ImGuiChildFlags_Border
#define ImGuiChildFlags_Border ImGuiChildFlags_Borders
#endif

#ifndef ImGuiWindowFlags_DragScrolling
#define ImGuiWindowFlags_DragScrolling (1 << 29)
#endif

#ifndef ImGuiWindowFlags_NavFlattened
#define ImGuiWindowFlags_NavFlattened 0
#endif

static inline float ImFontLegacySize(const ImFont *font)
{
	return font ? font->LegacySize : ImGui::GetFontSize();
}
