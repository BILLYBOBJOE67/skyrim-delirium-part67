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

		void Read(const CSimpleIniA& a_ini, const Setting& a_s)
		{
			std::visit(
				[&](auto* a_p) {
					using T = std::remove_pointer_t<decltype(a_p)>;
					if constexpr (std::is_same_v<T, bool>) {
						*a_p = a_ini.GetBoolValue(a_s.section, a_s.key, a_s.def != 0.0);
					} else if constexpr (std::is_same_v<T, float>) {
						*a_p = Limited(a_s, static_cast<float>(a_ini.GetDoubleValue(a_s.section, a_s.key, a_s.def)));
					} else if constexpr (std::is_same_v<T, std::string>) {
						const char* def = a_s.oldKey ? a_ini.GetValue(a_s.section, a_s.oldKey, a_s.defText) : a_s.defText;
						*a_p = a_ini.GetValue(a_s.section, a_s.key, def);
					} else {
						long v = a_ini.GetLongValue(a_s.section, a_s.key, static_cast<long>(a_s.def));
						if (a_s.limit.hi != kNoLimit) {
							v = std::clamp(v, static_cast<long>(a_s.limit.lo), static_cast<long>(a_s.limit.hi));
						} else if (a_s.limit.lo != -kNoLimit) {
							v = std::max(static_cast<long>(a_s.limit.lo), v);
						}
						*a_p = static_cast<T>(v);
					}
				},
				a_s.value);
		}
	}

	void Engine::LoadSettings()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile("Data/SKSE/Plugins/SkyrimDelirium.ini");
		const auto table = SettingsTable();
		for (const auto& s : table) {
			settings::Read(ini, s);
		}
		LoadReactions(ini);

		// a key no setting reads is a typo or an old name: say so
		CSimpleIniA::TNamesDepend sections;
		ini.GetAllSections(sections);
		for (const auto& sec : sections) {
			CSimpleIniA::TNamesDepend keys;
			ini.GetAllKeys(sec.pItem, keys);
			for (const auto& k : keys) {
				const bool row = _stricmp(sec.pItem, "Reactions") == 0 && !(k.pItem[0] == 'f' && std::isupper(static_cast<unsigned char>(k.pItem[1])));
				const bool known = row || std::any_of(table.begin(), table.end(), [&](const auto& s) {
					return _stricmp(s.section, sec.pItem) == 0 && (_stricmp(s.key, k.pItem) == 0 || (s.oldKey && _stricmp(s.oldKey, k.pItem) == 0));
				});
				if (!known) {
					logger::warn("[{}] {}: no such setting (a typo, or one the mod no longer has)", sec.pItem, k.pItem);
				}
			}
		}

		logger::info("muscle gain on Skyrim's ragdoll {:.2f}, joint damping {:.2f}, its joint friction x{:.2f} while driven, Delirium's masses {}", gain_, damp_,
			friction_, matchMass_);
		logger::info("[Spells] start chance: missiles {:.0f}-{:.0f} %, channels {:.0f}-{:.0f} % a second (full health - near death)", spellChance_[0] * 100.0f,
			spellChance_[1] * 100.0f, channelChance_[0] * 100.0f, channelChance_[1] * 100.0f);
		logger::info("oomph: from x{:.2f} to x{:.2f} damage scale; twist x(1 + {:.1f}), lean 0.3 + {:.1f}, push x(1 + {:.1f}); reels from {:.2f} ({:.1f} + {:.1f} x oomph s), falls from {:.2f}",
			oomphFrom_, oomphTo_, oomphSpin_, oomphLean_, oomphPush_, reelAt_, reelTime_, reelTimeAdd_, fallAt_);
		logger::info("explosions: push {} {:.0f} N s at the centre, {:.0f}% at the edge, up {:.2f}; thrown at {:.1f} m/s (x damage scale x nearness), up {:.2f} per, from {:.1f} to {:.1f} m/s; high fall from {:.1f} m/s, tumble {:.0f} N s per m/s",
			blasts_, blastPush_, blastFar_ * 100.0f, blastUp_, flySpeed_, flyUp_, flyMin_, flyMax_, highFallAt_, tumble_);
		logger::info("down: writhes {:.1f} + {:.1f} x oomph s, curls up {:.0f}%; melee oomph from x{:.1f} to x{:.1f} a plain blow, knocked down from {:.2f}, head whip {:.2f}",
			writheTime_, writheAdd_, foetalChance_ * 100.0f, meleeOomphFrom_, meleeOomphTo_, meleeKnockAt_, headWhip_);
		logger::info("melee: {} push {:.0f} N s (power x{:.1f}, bash x{:.1f}, sneak x{:.1f}, blocked x{:.1f}), weight {:.0f} = x1 (x{:.1f}-{:.1f}), flinch under {:.0f}; stagger blow x(1 + {:.1f} x magnitude)",
			melee_, meleePush_, meleePower_, meleeBash_, meleeSneak_, meleeBlocked_, meleeWeightRef_, meleeHeftMin_, meleeHeftMax_, flinchBelow_, staggerHitBoost_);
		logger::info("staggers: played by Delirium {}, push {:.0f} N s at magnitude 1 ({:.0f}% at 0); hits push x{:.2f}-{:.2f}, x1 at {:.0f} damage, ^{:.2f}",
			staggers_, staggerPush_, staggerMin_ * 100.0f, damageMin_, damageMax_, damageRef_, damageExp_);
		logger::info("spells: shot reaction {}, burn {:.1f} s, shock {:.1f} s", spells_, burnTime_, shockTime_);
		logger::info("arrows: shot reaction {}, push {:.0f} N s, wounded below {:.0f}% health", arrows_, arrowPush_, wounded_ * 100.0f);
		logger::info("settings: GTA IV file '{}', character '{}', push key {:#x}, release key {:#x}, push {} N s, follow animation {}, take over Skyrim's ragdolls {}",
			fredPath_, buildPath_, pushKey_, releaseKey_, push_, followAnimation_, takeOver_);
	}

	void Engine::LoadReactions(const CSimpleIniA& ini)
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
		rules_.clear();
		for (const auto& [k, v] : kTable) {
			rules_[k] = ParseRule(v);
		}
		CSimpleIniA::TNamesDepend keys;
		ini.GetAllKeys("Reactions", keys);
		int own = 0;
		for (const auto& k : keys) {
			std::string key = k.pItem;
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (key.empty() || (k.pItem[0] == 'f' && std::isupper(static_cast<unsigned char>(k.pItem[1])))) {
				continue;  // a setting (fHeavyOomph), not a row
			}
			rules_[key] = ParseRule(ini.GetValue("Reactions", k.pItem, ""));
			++own;
		}
		static constexpr std::string_view kKnown[]{ "shot", "flinch", "step", "none", "spin", "headsnap", "collapse", "push", "burn", "shock", "chill", "catch",
			"foetal", "windmill", "rollup", "highfall", "thrash", "tumble", "react", "time", "writhe", "roll", "still", "amp", "period", "shiver" };
		for (const auto& [k, r] : rules_) {
			for (const auto& [n, v] : r.layers) {
				if (std::find(std::begin(kKnown), std::end(kKnown), n) == std::end(kKnown)) {
					logger::warn("[Reactions] {}: unknown behaviour '{}'", k, n);
				}
			}
		}
		logger::info("reaction table: {} rows ({} from the ini)", rules_.size(), own);
	}

}
