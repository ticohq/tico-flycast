// dear imgui: wrappers for C++ standard library (STL) types (std::string, etc.)

// This is also an example of how you may wrap your own similar types.
// TL;DR; this is using the ImGuiInputTextFlags_CallbackResize facility,
// which also demonstrated in 'Dear ImGui Demo->Widgets->Text Input->Resize Callback'.

// Changelog:
// - v0.10: Initial version. Added InputText() / InputTextMultiline() calls with std::string

// Usage:
// {
//   #include "misc/cpp/imgui_stdlib.h"
//   #include "misc/cpp/imgui_stdlib.cpp" // <-- If you want to include implementation without messing with your project/build.
//   [...]
//   std::string my_string;
//   ImGui::InputText("my string", &my_string);
// }

// See more C++ related extension (fmt, RAII, syntactic sugar) on Wiki:
//   https://github.com/ocornut/imgui/wiki/Useful-Extensions#cness

#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_stdlib.h"

namespace
{
struct InputTextCallback_UserData
{
	std::string *str;
	ImGuiInputTextCallback chainCallback;
	void *chainCallbackUserData;
};

int InputTextCallback(ImGuiInputTextCallbackData *data)
{
	InputTextCallback_UserData *userData = static_cast<InputTextCallback_UserData *>(data->UserData);
	if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
	{
		std::string *str = userData->str;
		str->resize(data->BufTextLen);
		data->Buf = str->data();
		return 0;
	}
	if (userData->chainCallback)
	{
		data->UserData = userData->chainCallbackUserData;
		return userData->chainCallback(data);
	}
	return 0;
}
}

namespace ImGui
{
bool InputText(const char *label, std::string *str, ImGuiInputTextFlags flags,
		ImGuiInputTextCallback callback, void *user_data)
{
	IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
	flags |= ImGuiInputTextFlags_CallbackResize;

	InputTextCallback_UserData callbackData;
	callbackData.str = str;
	callbackData.chainCallback = callback;
	callbackData.chainCallbackUserData = user_data;
	return InputText(label, str->data(), str->capacity() + 1, flags,
			InputTextCallback, &callbackData);
}
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif // #ifndef IMGUI_DISABLE
