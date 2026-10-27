// SkyrimDelirium.ini -> the settings (from the settings table) and the reaction table rows.
#include "settings/table.h"

namespace delirium
{
	namespace settings
	{
		float Limited(const Setting& a_s, float a_v)
		{
			const Limit& l = a_s.limit;
			if (l.hi != kNoLimit) {
				return std::clamp(a_v, static_cast<float>(l.lo), static_cast<float>(l.hi));
			}
			if (l.lo != -kNoLimit) {
				return std::max(static_cast<float>(l.lo), a_v);
			}
			if (l.atLeast) {
				return std::max(l.gap != 0.0f ? *l.atLeast + l.gap : *l.atLeast, a_v);
			}
			return a_v;
		}

		Any Limited(const Setting& a_s, const Any& a_v)
		{
			if (const auto* f = std::get_if<float>(&a_v)) {
				return Limited(a_s, *f);
			}
			if (const auto* n = std::get_if<long long>(&a_v)) {
				auto v = static_cast<long>(*n);
				if (a_s.limit.hi != kNoLimit) {
					v = std::clamp(v, static_cast<long>(a_s.limit.lo), static_cast<long>(a_s.limit.hi));
				} else if (a_s.limit.lo != -kNoLimit) {
					v = std::max(static_cast<long>(a_s.limit.lo), v);
				}
				return static_cast<long long>(v);
			}
			return a_v;
		}

		Any Parse(const CSimpleIniA& a_ini, const Setting& a_s)
		{
			return std::visit(
				[&](auto* a_p) -> Any {
					using T = std::remove_pointer_t<decltype(a_p)>;
					if constexpr (std::is_same_v<T, bool>) {
						return a_ini.GetBoolValue(a_s.section, a_s.key, a_s.def != 0.0);
					} else if constexpr (std::is_same_v<T, float>) {
						return Limited(a_s, static_cast<float>(a_ini.GetDoubleValue(a_s.section, a_s.key, a_s.def)));
					} else if constexpr (std::is_same_v<T, std::string>) {
						const char* def = a_s.oldKey ? a_ini.GetValue(a_s.section, a_s.oldKey, a_s.defText) : a_s.defText;
						return std::string(a_ini.GetValue(a_s.section, a_s.key, def));
					} else {
						return Limited(a_s, Any(static_cast<long long>(a_ini.GetLongValue(a_s.section, a_s.key, static_cast<long>(a_s.def)))));
					}
				},
				a_s.value);
		}

		void Read(const CSimpleIniA& a_ini, const Setting& a_s) { Put(a_s, Parse(a_ini, a_s)); }

		Any Get(const Setting& a_s)
		{
			return std::visit(
				[](auto* a_p) -> Any {
					using T = std::remove_pointer_t<decltype(a_p)>;
					if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, float> || std::is_same_v<T, std::string>) {
						return *a_p;
					} else {
						return static_cast<long long>(*a_p);
					}
				},
				a_s.value);
		}

		void Put(const Setting& a_s, const Any& a_v)
		{
			std::visit(
				[&](auto* a_p) {
					using T = std::remove_pointer_t<decltype(a_p)>;
					if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, float> || std::is_same_v<T, std::string>) {
						if (const auto* v = std::get_if<T>(&a_v)) {
							*a_p = *v;
						}
					} else if (const auto* v = std::get_if<long long>(&a_v)) {
						*a_p = static_cast<T>(*v);
					}
				},
				a_s.value);
		}

		Any Default(const Setting& a_s)
		{
			return std::visit(
				[&](auto* a_p) -> Any {
					using T = std::remove_pointer_t<decltype(a_p)>;
					if constexpr (std::is_same_v<T, bool>) {
						return a_s.def != 0.0;
					} else if constexpr (std::is_same_v<T, float>) {
						return static_cast<float>(a_s.def);
					} else if constexpr (std::is_same_v<T, std::string>) {
						return std::string(a_s.defText ? a_s.defText : "");
					} else {
						return static_cast<long long>(a_s.def);
					}
				},
				a_s.value);
		}

		std::string Text(const Setting& a_s, const Any& a_v)
		{
			if (const auto* b = std::get_if<bool>(&a_v)) {
				return *b ? "1" : "0";
			}
			if (const auto* f = std::get_if<float>(&a_v)) {
				return std::format("{}", *f);
			}
			if (const auto* n = std::get_if<long long>(&a_v)) {
				// the ini reader reads a whole number as a signed 32-bit one: above that, as the negative number it came from
				if (*n > INT32_MAX && *n <= UINT32_MAX) {
					return std::format("{}", static_cast<std::int32_t>(static_cast<std::uint32_t>(*n)));
				}
				const std::string_view k = a_s.key;
				return k.ends_with("Key") ? std::format("0x{:X}", *n) : std::format("{}", *n);
			}
			return std::get<std::string>(a_v);
		}

		std::string Name(const Setting& a_s) { return std::format("{}.{}", a_s.section, a_s.key); }
	}

	void Engine::LoadSettings()
	{
		{
			std::ifstream in(settings::kIniPath, std::ios::binary);
			startText_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadData(startText_.data(), startText_.size());
		ReadSettings(ini);
		LogSettings();
		saved_ = Snapshot();
		std::error_code ec;
		savedTime_ = std::filesystem::last_write_time(settings::kIniPath, ec);
	}

	void Engine::ReadSettings(const CSimpleIniA& ini)
	{
		const auto table = SettingsTable();
		for (const auto& s : table) {
			settings::Read(ini, s);
		}
		LoadReactions(ini);

		// a key no setting reads is a typo or an old name: say so (settings the mod dropped, which did nothing, quietly)
		static constexpr std::pair<const char*, const char*> kRetired[]{ { "Melee", "fKnockAt" }, { "Hits", "fOomphLean" }, { "Hits", "fReelAt" },
			{ "Hits", "fFallAt" }, { "Hits", "fReelTime" }, { "Hits", "fReelTimeAdd" },
			{ "Shouts", "fThrowFrom" }, { "Shouts", "fFlySpeed" } };  // shouts: the stand.shout.word1-3 rows now
		CSimpleIniA::TNamesDepend sections;
		ini.GetAllSections(sections);
		for (const auto& sec : sections) {
			CSimpleIniA::TNamesDepend keys;
			ini.GetAllKeys(sec.pItem, keys);
			for (const auto& k : keys) {
				const bool row = _stricmp(sec.pItem, "Reactions") == 0 && IsRowKey(k.pItem);
				const bool known = row ||
					std::any_of(table.begin(), table.end(), [&](const auto& s) {
						return _stricmp(s.section, sec.pItem) == 0 && (_stricmp(s.key, k.pItem) == 0 || (s.oldKey && _stricmp(s.oldKey, k.pItem) == 0));
					}) ||
					std::any_of(std::begin(kRetired), std::end(kRetired), [&](const auto& r) {
						return _stricmp(r.first, sec.pItem) == 0 && _stricmp(r.second, k.pItem) == 0;
					});
				if (!known) {
					logger::warn("[{}] {}: no such setting (a typo, or one the mod no longer has)", sec.pItem, k.pItem);
				}
			}
		}
	}

	void Engine::LogSettings() const
	{
		logger::info("muscle gain on Skyrim's ragdoll {:.2f}, joint damping {:.2f}, its joint friction x{:.2f} while driven, Delirium's masses {}", gain_, damp_,
			friction_, matchMass_);
		logger::info("[Spells] start chance: missiles {:.0f}-{:.0f} %, channels {:.0f}-{:.0f} % a second (full health - near death)", spellChance_[0] * 100.0f,
			spellChance_[1] * 100.0f, channelChance_[0] * 100.0f, channelChance_[1] * 100.0f);
		logger::info("oomph: from x{:.2f} to x{:.2f} damage scale; twist x(1 + {:.1f}), push x(1 + {:.1f})", oomphFrom_, oomphTo_, oomphSpin_, oomphPush_);
		logger::info("explosions: push {} {:.0f} N s at the centre, {:.0f}% at the edge, up {:.2f}; thrown at {:.1f} m/s (x damage scale x nearness), up {:.2f} per, from {:.1f} to {:.1f} m/s; high fall from {:.1f} m/s, tumble {:.0f} N s per m/s",
			blasts_, blastPush_, blastFar_ * 100.0f, blastUp_, flySpeed_, flyUp_, flyMin_, flyMax_, highFallAt_, tumble_);
		logger::info("down: writhes {:.1f} + {:.1f} x oomph s, curls up {:.0f}%; melee oomph from x{:.1f} to x{:.1f} a plain blow, head whip {:.2f}",
			writheTime_, writheAdd_, foetalChance_ * 100.0f, meleeOomphFrom_, meleeOomphTo_, headWhip_);
		logger::info("melee: {} push {:.0f} N s (power x{:.1f}, bash x{:.1f}, sneak x{:.1f}, blocked x{:.1f}), weight {:.0f} = x1 (x{:.1f}-{:.1f}), flinch under {:.0f}; stagger blow x(1 + {:.1f} x magnitude)",
			melee_, meleePush_, meleePower_, meleeBash_, meleeSneak_, meleeBlocked_, meleeWeightRef_, meleeHeftMin_, meleeHeftMax_, flinchBelow_, staggerHitBoost_);
		logger::info("staggers: played by Delirium {}, push {:.0f} N s at magnitude 1 ({:.0f}% at 0); hits push x{:.2f}-{:.2f}, x1 at {:.0f} damage, ^{:.2f}",
			staggers_, staggerPush_, staggerMin_ * 100.0f, damageMin_, damageMax_, damageRef_, damageExp_);
		logger::info("spells: shot reaction {}, burn {:.1f} s, shock {:.1f} s", spells_, burnTime_, shockTime_);
		logger::info("arrows: shot reaction {}, push {:.0f} N s, wounded below {:.0f}% health", arrows_, arrowPush_, wounded_ * 100.0f);
		logger::info("settings: GTA IV file '{}', character '{}', push key {:#x}, release key {:#x}, push {} N s, follow animation {}, take over Skyrim's ragdolls {}",
			fredPath_, buildPath_, pushKey_, releaseKey_, push_, followAnimation_, takeOver_);
	}

	std::span<const std::pair<const char*, const char*>> Engine::BuiltInRows()
	{
		// the reaction table: built in (today's reactions), each [Reactions] row replaces its own
		static constexpr std::pair<const char*, const char*> kTable[]{
			{ "stand", "step" },
			{ "stand.melee", "shot, spin, headsnap" },
			{ "stand.melee.light", "flinch" },
			{ "stand.arrow", "shot, spin, headsnap" },
			{ "stand.spell", "shot, spin, headsnap" },
			{ "stand.stagger.light", "flinch" },
			{ "stand.blast.light", "flinch" },
			{ "stand.lethal", "shot:150, spin, headsnap, collapse" },
			{ "stand.shout.word1", "step" },
			{ "stand.shout.word2", "step, push:2" },
			{ "stand.shout.word3", "step, throw:9" },
			{ "stand.bump", "step" },
			{ "fire", "burn" },
			{ "shock", "shock" },
			{ "frost", "" },
			{ "fly", "foetal:50, windmill:30, rollup:20" },
			{ "fly.far", "foetal:45, highfall:35, windmill:10, rollup:10" },
			{ "fly.fire", "foetal:75, windmill:25, thrash" },
			{ "downhit", "react, push:0.6" },
			{ "downhit.melee", "react, push:0.6, time:0.8" },
			{ "down.arrow", "writhe" },
			{ "down.magic", "writhe" },
			{ "down.fire", "roll, amp:2.2, period:0.4" },
			{ "down.shock", "writhe, amp:1.3, period:0.3, shock" },
			{ "down.frost", "foetal, shiver" },
			{ "fly.shout", "foetal:50, windmill:30, rollup:20" },
			{ "fly.shout.far", "highfall:60, foetal:25, windmill:15" }
		};
		return kTable;
	}

	bool Engine::IsRowKey(const char* a_key)
	{
		return a_key[0] && !(a_key[0] == 'f' && std::isupper(static_cast<unsigned char>(a_key[1])));  // fHeavyOomph is a setting, not a row
	}

	void Engine::LoadReactions(const CSimpleIniA& ini)
	{
		rules_.clear();
		for (const auto& [k, v] : BuiltInRows()) {
			rules_[k] = ParseRule(v);
		}
		CSimpleIniA::TNamesDepend keys;
		ini.GetAllKeys("Reactions", keys);
		int own = 0;
		for (const auto& k : keys) {
			std::string key = k.pItem;
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (!IsRowKey(k.pItem)) {
				continue;  // a setting (fHeavyOomph), not a row
			}
			rules_[key] = ParseRule(ini.GetValue("Reactions", k.pItem, ""));
			++own;
		}
		for (const auto& [k, r] : rules_) {
			for (const auto& n : UnknownBehaviours(r)) {
				logger::warn("[Reactions] {}: unknown behaviour '{}'", k, n);
			}
		}
		logger::info("reaction table: {} rows ({} from the ini)", rules_.size(), own);
	}

	std::vector<std::string> Engine::UnknownBehaviours(const Rule& a_rule)
	{
		static constexpr std::string_view kKnown[]{ "shot", "flinch", "step", "none", "spin", "headsnap", "collapse", "push", "burn", "shock", "chill", "catch",
			"throw", "foetal", "windmill", "rollup", "highfall", "thrash", "tumble", "react", "time", "writhe", "roll", "still", "amp", "period", "shiver" };
		std::vector<std::string> out;
		for (const auto& [n, v] : a_rule.layers) {
			if (std::find(std::begin(kKnown), std::end(kKnown), n) == std::end(kKnown)) {
				out.push_back(n);
			}
		}
		return out;
	}

}
