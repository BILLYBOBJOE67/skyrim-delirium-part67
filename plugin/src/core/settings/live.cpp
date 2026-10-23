// Settings while the game runs: SkyrimDelirium.ini read again when it changes, the menu's edits, back to the
// defaults. Every change is logged as Section.key old -> new, and what start-up applied once is applied again.
#include "settings/table.h"

namespace delirium
{
	Engine::SettingsSnap Engine::Snapshot()
	{
		SettingsSnap s;
		for (const auto& row : SettingsTable()) {
			s.values.push_back(settings::Text(row));
		}
		std::scoped_lock l(lock_);
		for (const auto& [k, r] : rules_) {
			s.rows[k] = r.text;
		}
		return s;
	}

	void Engine::LogChanges(const SettingsSnap& a_before, const char* a_why)
	{
		const auto               table = SettingsTable();
		const SettingsSnap       now = Snapshot();
		std::vector<std::string> names;
		for (std::size_t i = 0; i < table.size() && i < a_before.values.size(); ++i) {
			if (a_before.values[i] != now.values[i]) {
				names.push_back(settings::Name(table[i]));
				logger::info("  {} {} -> {}{}", names.back(), a_before.values[i], now.values[i], table[i].flags & settings::kRestart ? " (on the next start)" : "");
			}
		}
		for (const auto& [k, t] : now.rows) {
			const auto it = a_before.rows.find(k);
			if (it == a_before.rows.end()) {
				logger::info("  [Reactions] {} = '{}' (new row)", k, t);
			} else if (it->second != t) {
				logger::info("  [Reactions] {} '{}' -> '{}'", k, it->second, t);
			}
		}
		for (const auto& [k, t] : a_before.rows) {
			if (!now.rows.contains(k)) {
				logger::info("  [Reactions] {} '{}' gone", k, t);
			}
		}
		if (now == a_before) {
			logger::info("  nothing changed");
		}
		unsaved_ = now != saved_;
		ApplyChanged(names);
		AutoSave();
	}

	void Engine::AutoSave()
	{
		if (unsaved_) {
			std::string msg;
			SaveIni(msg);
		}
	}

	void Engine::RevertSession()
	{
		const SettingsSnap before = Snapshot();
		const char*        why = "back to the settings the game started with";
		logger::info("settings: {}", why);
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadData(startText_.data(), startText_.size());
		{
			std::scoped_lock l(lock_);
			ReadSettings(ini);
		}
		LogChanges(before, why);
	}

	void Engine::ReloadSettings(const char* a_why)
	{
		const SettingsSnap before = Snapshot();
		logger::info("settings: {}", a_why);
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile(settings::kIniPath);
		{
			std::scoped_lock l(lock_);
			ReadSettings(ini);
		}
		saved_ = Snapshot();
		std::error_code ec;
		savedTime_ = std::filesystem::last_write_time(settings::kIniPath, ec);
		LogChanges(before, a_why);
	}

	void Engine::SettingsFileChanged()
	{
		std::error_code ec;
		const auto      t = std::filesystem::last_write_time(settings::kIniPath, ec);
		if (ec || t == savedTime_) {
			return;  // our own save (or no file)
		}
		ReloadSettings("SkyrimDelirium.ini changed: read again");
	}

	bool Engine::SetSetting(const settings::Setting& a_s, const settings::Any& a_v)
	{
		std::scoped_lock l(lock_);
		const auto       was = settings::Text(a_s);
		settings::Put(a_s, settings::Limited(a_s, a_v));
		// settings that must be at least this one (fHeftMax >= fHeftMin) follow it
		if (const auto* f = std::get_if<float*>(&a_s.value)) {
			for (const auto& row : SettingsTable()) {
				if (row.limit.atLeast == *f) {
					settings::Put(row, settings::Limited(row, settings::Get(row)));
				}
			}
		}
		return settings::Text(a_s) != was;
	}

	void Engine::SetRow(const std::string& a_key, const std::string& a_text)
	{
		std::scoped_lock l(lock_);
		rules_[a_key] = ParseRule(a_text);
	}

	bool Engine::IsBuiltInRow(const std::string& a_key, const char** a_text)
	{
		for (const auto& [k, v] : BuiltInRows()) {
			if (a_key == k) {
				if (a_text) {
					*a_text = v;
				}
				return true;
			}
		}
		return false;
	}

	void Engine::ResetRow(const std::string& a_key)
	{
		std::scoped_lock l(lock_);
		if (const char* text{}; IsBuiltInRow(a_key, &text)) {
			rules_[a_key] = ParseRule(text);
		} else {
			rules_.erase(a_key);
		}
	}

	std::vector<std::pair<std::string, std::string>> Engine::Rows()
	{
		std::vector<std::pair<std::string, std::string>> out;
		{
			std::scoped_lock l(lock_);
			for (const auto& [k, r] : rules_) {
				out.emplace_back(k, r.text);
			}
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	void Engine::LoadPresetIni(const CSimpleIniA& a_ini)
	{
		std::scoped_lock l(lock_);
		for (const auto& row : SettingsTable()) {
			if (row.flags & settings::kPreset) {
				settings::Read(a_ini, row);
			}
		}
		LoadReactions(a_ini);
	}

	void Engine::DefaultSettings(const char* a_why)
	{
		const SettingsSnap before = Snapshot();
		logger::info("settings: {}", a_why);
		CSimpleIniA none;
		LoadPresetIni(none);
		LogChanges(before, a_why);
	}

	void Engine::NoteEdit(const std::string& a_name, const std::string& a_was, const std::string& a_now)
	{
		logger::info("menu: {} {} -> {}", a_name, a_was, a_now);
		unsaved_ = Snapshot() != saved_;
		const std::string names[]{ a_name };
		ApplyChanged(names);
		AutoSave();
	}
}
