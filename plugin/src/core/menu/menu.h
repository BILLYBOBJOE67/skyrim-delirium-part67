#pragma once

// What menu.cpp and reactions.cpp share: the status line and the colours.
#pragma warning(push)
#pragma warning(disable : 4099 5054)  // the framework's own header
#include "third_party/SKSEMenuFramework.h"
#pragma warning(pop)

namespace delirium::menu
{
	inline constexpr ImGuiMCP::ImVec4 kChanged{ 1.0f, 0.78f, 0.3f, 1.0f };  // a value off its default
	inline constexpr ImGuiMCP::ImVec4 kBad{ 1.0f, 0.4f, 0.4f, 1.0f };       // unknown behaviour, failed save
	inline constexpr ImGuiMCP::ImVec4 kGood{ 0.55f, 0.9f, 0.55f, 1.0f };

	void Say(std::string a_msg, bool a_error = false);  // the line under the bar, for a few seconds
	void Help(const std::string& a_text);               // the last item's tooltip, wrapped
}
