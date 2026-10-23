// Offline settings check: loads <folder>\Data\SKSE\Plugins\SkyrimDelirium.ini the way the core does and writes
// every setting and reaction row to <out>. tools\check-settings.ps1 runs it on tests\settings\*.ini and compares the
// result with tests\settings\*.expected.txt.
#include "shared.h"

#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
	if (argc != 3) {
		std::cerr << "usage: SettingsCheck <folder holding Data\\SKSE\\Plugins\\SkyrimDelirium.ini> <out.txt>\n";
		return 2;
	}
	spdlog::set_level(spdlog::level::warn);  // warnings (unknown keys, unknown behaviours) still show
	std::filesystem::current_path(argv[1]);
	auto engine = std::make_unique<delirium::Engine>();
	engine->LoadSettings();
	std::ofstream out(argv[2], std::ios::binary);
	out << engine->DumpSettings();
	return out ? 0 : 1;
}
