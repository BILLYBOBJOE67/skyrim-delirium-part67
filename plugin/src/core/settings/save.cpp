// Saving: the live settings into SkyrimDelirium.ini, and presets (Presets\<name>.ini: only what differs from the
// defaults, so a preset is short and can be shared). The ini is edited line by line: only the lines whose value
// differs are rewritten, comments, order and spacing stay; a setting the file lacks goes at the end of its section.
#include "settings/table.h"

namespace delirium
{
	namespace
	{
		struct Edit
		{
			std::string section, key, value;
			bool        remove{};
			bool        done{};
		};

		std::string Trim(std::string_view a_s)
		{
			const auto b = a_s.find_first_not_of(" \t\r");
			if (b == std::string_view::npos) {
				return {};
			}
			return std::string(a_s.substr(b, a_s.find_last_not_of(" \t\r") - b + 1));
		}

		bool Same(std::string_view a, std::string_view b) { return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0; }

		// a_text with the edits made: a key's line rewritten (or removed), missing keys after their section's last key
		std::string ApplyEdits(const std::string& a_text, std::vector<Edit>& a_edits)
		{
			const std::string        eol = a_text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
			std::vector<std::string> lines;
			for (std::size_t s = 0; s < a_text.size();) {
				const auto e = a_text.find('\n', s);
				std::string l = a_text.substr(s, e == std::string::npos ? std::string::npos : e - s);
				if (!l.empty() && l.back() == '\r') {
					l.pop_back();
				}
				lines.push_back(std::move(l));
				s = e == std::string::npos ? a_text.size() : e + 1;
			}
			std::vector<std::string>                     out;
			std::string                                  section;
			std::map<std::string, std::size_t>           lastKey;  // section (lower case) -> out index after its last key line
			const auto lower = [](std::string a_s) {
				std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return a_s;
			};
			for (const auto& l : lines) {
				const std::string t = Trim(l);
				if (t.size() > 1 && t.front() == '[' && t.back() == ']') {
					section = Trim(std::string_view(t).substr(1, t.size() - 2));
					lastKey[lower(section)] = out.size() + 1;
					out.push_back(l);
					continue;
				}
				const auto eq = l.find('=');
				if (t.empty() || t.front() == ';' || t.front() == '#' || eq == std::string::npos) {
					out.push_back(l);
					continue;
				}
				const std::string key = Trim(std::string_view(l).substr(0, eq));
				Edit*             hit = nullptr;
				for (auto& e : a_edits) {
					if (Same(e.section, section) && Same(e.key, key)) {  // every line of a key the file has twice
						hit = &e;
						break;
					}
				}
				if (hit) {
					hit->done = true;
					if (hit->remove) {
						continue;
					}
					// keep "key=" (or "key = ") as written
					auto v = eq + 1;
					while (v < l.size() && l[v] == ' ') {
						++v;
					}
					out.push_back(l.substr(0, v) + hit->value);
				} else {
					out.push_back(l);
				}
				lastKey[lower(section)] = out.size();
			}
			// keys the file lacks: after their section's last key (sections in the file, from the bottom up)
			std::vector<std::pair<std::size_t, std::string>> inserts;
			for (auto& e : a_edits) {
				if (e.done || e.remove) {
					continue;
				}
				if (const auto it = lastKey.find(lower(e.section)); it != lastKey.end()) {
					inserts.emplace_back(it->second, e.key + "=" + e.value);
					e.done = true;
				}
			}
			std::stable_sort(inserts.begin(), inserts.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (auto it = inserts.begin(); it != inserts.end();) {
				// keep their own order within one place
				auto end = std::find_if(it, inserts.end(), [&](const auto& x) { return x.first != it->first; });
				std::vector<std::string> block;
				for (auto j = it; j != end; ++j) {
					block.push_back(j->second);
				}
				out.insert(out.begin() + static_cast<std::ptrdiff_t>(it->first), block.begin(), block.end());
				it = end;
			}
			// sections the file lacks: at its end
			std::string tail;
			std::string open;
			for (auto& e : a_edits) {
				if (e.done || e.remove) {
					continue;
				}
				if (!Same(open, e.section)) {
					open = e.section;
					tail += eol + "[" + e.section + "]" + eol;
				}
				tail += e.key + "=" + e.value + eol;
				e.done = true;
			}
			std::string text;
			for (std::size_t i = 0; i < out.size(); ++i) {
				text += out[i];
				if (i + 1 < out.size() || (!a_text.empty() && a_text.back() == '\n')) {
					text += eol;
				}
			}
			if (!tail.empty() && !text.empty() && !text.ends_with('\n')) {
				text += eol;
			}
			return text + tail;
		}

		std::string ReadFile(const std::filesystem::path& a_path)
		{
			std::ifstream in(a_path, std::ios::binary);
			return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		}

		bool WriteFile(const std::filesystem::path& a_path, const std::string& a_text, std::string& a_msg)
		{
			// in place, not a new file renamed over it: under Mod Organizer a renamed file lands in Overwrite, and the
			// ini in the mod folder would stay as it was
			std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
			out << a_text;
			out.flush();
			if (!out) {
				a_msg = std::format("can't write {}", a_path.string());
				return false;
			}
			return true;
		}

		std::string FileName(const std::string& a_name)
		{
			std::string n;
			for (const char c : a_name) {
				n += std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 32 ? '_' : c;
			}
			return Trim(n);
		}
	}

	bool Engine::SaveIni(std::string& a_msg)
	{
		const std::string text = ReadFile(settings::kIniPath);
		CSimpleIniA       file;
		file.SetUnicode();
		file.LoadData(text.data(), text.size());
		std::vector<Edit> edits;
		for (const auto& row : SettingsTable()) {
			const auto now = settings::Text(row);
			if (settings::Text(row, settings::Parse(file, row)) != now) {
				edits.push_back({ row.section, row.key, now });
			}
		}
		// rows: each one whose text differs from what the file gives (the built-in row where it has none)
		std::map<std::string, std::string> inFile;
		{
			CSimpleIniA::TNamesDepend keys;
			file.GetAllKeys("Reactions", keys);
			for (const auto& k : keys) {
				if (IsRowKey(k.pItem)) {
					std::string key = k.pItem;
					std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					inFile[key] = Trim(file.GetValue("Reactions", k.pItem, ""));
				}
			}
		}
		const auto rows = Rows();
		for (const auto& [k, t] : rows) {
			const char* builtIn{};
			const auto  it = inFile.find(k);
			const std::string given = it != inFile.end() ? it->second : IsBuiltInRow(k, &builtIn) ? builtIn : "\x01";
			if (Trim(t) != given) {
				edits.push_back({ "Reactions", k, t });
			}
		}
		for (const auto& [k, t] : inFile) {
			if (std::none_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == k; })) {
				edits.push_back({ "Reactions", k, "", true });
			}
		}
		if (!edits.empty()) {
			if (!WriteFile(settings::kIniPath, ApplyEdits(text, edits), a_msg)) {
				logger::error("settings: {}", a_msg);
				saveError_ = a_msg;
				return false;
			}
		}
		std::error_code ec;
		savedTime_ = std::filesystem::last_write_time(settings::kIniPath, ec);
		saved_ = Snapshot();
		unsaved_ = false;
		saveError_.clear();
		a_msg = edits.empty() ? "SkyrimDelirium.ini already has these settings" : std::format("saved to SkyrimDelirium.ini ({} lines)", edits.size());
		logger::info("settings: {}", a_msg);
		for (const auto& e : edits) {
			logger::info("  [{}] {}{}", e.section, e.key, e.remove ? " removed" : " = " + e.value);
		}
		return true;
	}

	bool Engine::SavePreset(const std::string& a_name, std::string& a_msg)
	{
		const std::string name = FileName(a_name);
		if (name.empty()) {
			a_msg = "give the preset a name";
			return false;
		}
		std::string text = std::format("; Skyrim Delirium preset '{}': only what differs from the defaults (load it in the menu, Presets)\n", name);
		std::string open;
		auto        put = [&](const char* a_section, const std::string& a_line) {
            if (open != a_section) {
                open = a_section;
                text += std::format("\n[{}]\n", a_section);
            }
            text += a_line + "\n";
		};
		for (const auto& row : SettingsTable()) {
			if (row.flags & settings::kPreset) {
				if (const auto now = settings::Text(row); now != settings::Text(row, settings::Default(row))) {
					put(row.section, std::format("{}={}", row.key, now));
				}
			}
		}
		for (const auto& [k, t] : Rows()) {
			const char* builtIn{};
			if (!IsBuiltInRow(k, &builtIn) || Trim(t) != builtIn) {
				put("Reactions", std::format("{}={}", k, t));
			}
		}
		std::error_code ec;
		std::filesystem::create_directories(settings::kPresetDir, ec);
		const auto path = std::filesystem::path(settings::kPresetDir) / (name + ".ini");
		if (!WriteFile(path, text, a_msg)) {
			logger::error("preset: {}", a_msg);
			return false;
		}
		a_msg = std::format("preset '{}' saved", name);
		logger::info("{} ({})", a_msg, path.string());
		return true;
	}

	bool Engine::LoadPreset(const std::string& a_name, std::string& a_msg)
	{
		const auto path = std::filesystem::path(settings::kPresetDir) / (FileName(a_name) + ".ini");
		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(path.c_str()) < 0) {
			a_msg = std::format("no preset {}", path.string());
			logger::warn("preset: {}", a_msg);
			return false;
		}
		const SettingsSnap before = Snapshot();
		const std::string  why = std::format("preset '{}' loaded", a_name);
		logger::info("settings: {}", why);
		LoadPresetIni(ini);
		LogChanges(before, why.c_str());
		a_msg = why;
		return true;
	}

	std::vector<std::string> Engine::PresetNames()
	{
		std::vector<std::string> out;
		std::error_code          ec;
		for (const auto& e : std::filesystem::directory_iterator(settings::kPresetDir, ec)) {
			if (e.path().extension() == ".ini") {
				out.push_back(e.path().stem().string());
			}
		}
		std::sort(out.begin(), out.end());
		return out;
	}
}
