// Offline settings check: loads <folder>\Data\SKSE\Plugins\SkyrimDelirium.ini the way the core does and writes
// every setting and reaction row to <out>. tools\check-settings.ps1 runs it on tests\settings\*.ini and compares the
// result with tests\settings\*.expected.txt. Then saving is checked on the same ini (a copy):
//   a preset saved and loaded again gives the same settings,
//   the defaults saved to the ini and read again give the defaults, and a second save changes no byte.
#include "shared.h"
#include "settings/table.h"

#include <fstream>
#include <iostream>

// engine.cpp (not linked here): what start-up applied once, applied again after a change
void delirium::Engine::ApplyChanged(std::span<const std::string>) {}

namespace
{
	std::string Bytes(const char* a_path)
	{
		std::ifstream in(a_path, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	bool Same(const char* a_what, const std::string& a_want, const std::string& a_got)
	{
		if (a_want == a_got) {
			return true;
		}
		std::istringstream w(a_want), g(a_got);
		std::string        lw, lg;
		for (int shown = 0; shown < 8;) {
			const bool more = static_cast<bool>(std::getline(w, lw)) | static_cast<bool>(std::getline(g, lg));
			if (!more) {
				break;
			}
			if (lw != lg) {
				std::cerr << "  " << a_what << ": want '" << lw << "', got '" << lg << "'\n";
				++shown;
			}
			lw.clear();
			lg.clear();
		}
		return false;
	}
}

int main(int argc, char** argv)
{
	if (argc != 3) {
		std::cerr << "usage: SettingsCheck <folder holding Data\SKSE\Plugins\SkyrimDelirium.ini> <out.txt>\n";
		return 2;
	}
	spdlog::set_level(spdlog::level::warn);  // warnings (unknown keys, unknown behaviours) still show
	std::filesystem::current_path(argv[1]);
	auto engine = std::make_unique<delirium::Engine>();
	engine->LoadSettings();
	const std::string loaded = engine->DumpSettings();
	{
		std::ofstream out(argv[2], std::ios::binary);
		out << loaded;
		if (!out) {
			return 1;
		}
	}

	spdlog::set_level(spdlog::level::err);  // the round trips log every change: only failures matter here
	bool        ok = true;
	std::string msg;
	ok &= engine->SavePreset("_check", msg);
	engine->DefaultSettings("check");
	const std::string defaults = engine->DumpSettings();
	ok &= engine->SaveIni(msg);
	{
		auto fresh = std::make_unique<delirium::Engine>();
		fresh->LoadSettings();
		ok &= Same("defaults saved, read again", defaults, fresh->DumpSettings());
	}
	const std::string bytes = Bytes(delirium::settings::kIniPath);
	ok &= engine->SaveIni(msg);
	ok &= Same("second save", bytes, Bytes(delirium::settings::kIniPath));
	ok &= engine->LoadPreset("_check", msg);
	ok &= Same("preset saved, loaded again", loaded, engine->DumpSettings());
	if (!ok) {
		std::cerr << "saving failed: " << msg << "\n";
		return 3;
	}
	return 0;
}
