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
