// SkyrimDelirium.ini: every setting and the reaction table rows.
#include "shared.h"

namespace delirium
{
	void Engine::LoadSettings()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile("Data/SKSE/Plugins/SkyrimDelirium.ini");
		// GTA IV's Fred.art from the player's own install (optional; sCharacterFile is the older name), else the mod's own
		fredPath_ = ini.GetValue("Data", "sGTAFile", ini.GetValue("Data", "sCharacterFile", ""));
		buildPath_ = ini.GetValue("Data", "sCharacter", "Data/SKSE/Plugins/SkyrimDelirium/skyrim_character.json");
		rigPath_ = ini.GetValue("Data", "sRig", "Data/SKSE/Plugins/SkyrimDelirium/skyrim_character_rig.txt");
		pushKey_ = static_cast<std::uint32_t>(ini.GetLongValue("Test", "iPushKey", 0x41));
		releaseKey_ = static_cast<std::uint32_t>(ini.GetLongValue("Test", "iReleaseKey", 0x3C));
		push_ = static_cast<float>(ini.GetDoubleValue("Test", "fPush", 80.0));
		autoRelease_ = ini.GetBoolValue("Test", "bAutoRelease", true);
		followAnimation_ = ini.GetBoolValue("Test", "bFollowAnimation", true);
		recordPoses_ = ini.GetBoolValue("Test", "bRecordPoses", false);
		standingLegs_ = ini.GetBoolValue("Test", "bStandingLegs", true);
		collision_ =ini.GetBoolValue("Collision", "bWorldCollision", true);
		colRadius_ = static_cast<float>(ini.GetDoubleValue("Collision", "fRadius", 600.0));
		colHeight_ = static_cast<float>(ini.GetDoubleValue("Collision", "fHeight", 300.0));
		colMaxTris_ = static_cast<std::size_t>(std::max(1000L, ini.GetLongValue("Collision", "iMaxTriangles", 40000)));
		takeOver_ = ini.GetBoolValue("Ragdoll", "bTakeOver", true);
		gain_ = static_cast<float>(ini.GetDoubleValue("Ragdoll", "fTorqueGain", 1.0));
		damp_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Ragdoll", "fJointDamping", 0.0)), 0.0f, 1.0f);
		friction_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Ragdoll", "fJointFriction", 0.0)), 0.0f, 1.0f);
		matchMass_ = ini.GetBoolValue("Ragdoll", "bMatchMass", true);
		matchContact_ = ini.GetBoolValue("Ragdoll", "bMatchContact", true);
		aboutJoint_ = ini.GetBoolValue("Ragdoll", "bTurnAboutJoint", true);
		noKillMoves_ = ini.GetBoolValue("Deaths", "bNoKillMoves", true);
		noKillCams_ = ini.GetBoolValue("Deaths", "bNoKillCams", true);
		dyingHealth_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fDyingHealth", 70.0)), 0.0f, 99.0f);
		limpMin_ = std::max(0.2f, static_cast<float>(ini.GetDoubleValue("Deaths", "fLimpAfterMin", 1.8)));
		limpMax_ = std::max(limpMin_, static_cast<float>(ini.GetDoubleValue("Deaths", "fLimpAfterMax", 3.0)));
		dyingWrithe_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fWritheChance", 0.5)), 0.0f, 1.0f);
		dyingCatch_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fCatchFallChance", 0.5)), 0.0f, 1.0f);
		deathPush_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fDeathPush", 1.5)), 0.0f, 6.0f);
		deathMaxSpeed_ = std::max(1.0f, static_cast<float>(ini.GetDoubleValue("Deaths", "fDeathMaxSpeed", 6.0)));
		dyingReact_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fShotTime", 1.2)), 0.0f, 5.0f);
		dyingTime_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fDyingTime", 3.0)), 0.0f, 10.0f);
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
			heavyOomph_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Reactions", "fHeavyOomph", 0.5)), 0.0f, 1.01f);
			shoutThrowFrom_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Shouts", "fThrowFrom", 0.6)), 0.0f, 1.01f);
			shoutFly_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Shouts", "fFlySpeed", 9.0)));
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
		collapseMin_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fCollapseAfterMin", 0.8)), 0.0f, 10.0f);
		collapseMax_ = std::max(collapseMin_, static_cast<float>(ini.GetDoubleValue("Deaths", "fCollapseAfterMax", 1.6)));
		logger::info("muscle gain on Skyrim's ragdoll {:.2f}, joint damping {:.2f}, its joint friction x{:.2f} while driven, Delirium's masses {}", gain_, damp_,
			friction_, matchMass_);
		arrows_ = ini.GetBoolValue("Arrows", "bShotReaction", true);
		arrowPush_ = static_cast<float>(ini.GetDoubleValue("Arrows", "fPush", 55.0));
		spells_ = ini.GetBoolValue("Spells", "bShotReaction", true);
		burnTime_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Spells", "fBurnTime", 2.5)));
		shockTime_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Spells", "fShockTime", 2.0)));
		channelLinger_ = std::max(0.1f, static_cast<float>(ini.GetDoubleValue("Spells", "fChannelLinger", 0.6)));
		const auto chance = [&](const char* a_key, double a_or) { return std::clamp(static_cast<float>(ini.GetDoubleValue("Spells", a_key, a_or)), 0.0f, 1.0f); };
		spellChance_[0] = chance("fStartChance", 0.3);
		spellChance_[1] = chance("fStartChanceHurt", 0.5);
		channelChance_[0] = chance("fChannelChance", 0.1);
		channelChance_[1] = chance("fChannelChanceHurt", 0.2);
		logger::info("[Spells] start chance: missiles {:.0f}-{:.0f} %, channels {:.0f}-{:.0f} % a second (full health - near death)", spellChance_[0] * 100.0f,
			spellChance_[1] * 100.0f, channelChance_[0] * 100.0f, channelChance_[1] * 100.0f);
		staggers_ = ini.GetBoolValue("Stagger", "bReplace", true);
		staggerPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Stagger", "fPush", 90.0)));
		staggerMin_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Stagger", "fMinShare", 0.4)), 0.0f, 1.0f);
		damageRef_ = std::max(0.1f, static_cast<float>(ini.GetDoubleValue("Hits", "fDamageRef", 15.0)));
		damageExp_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Hits", "fExponent", 0.8)), 0.0f, 2.0f);
		damageMin_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fScaleMin", 0.4)));
		damageMax_ = std::max(damageMin_, static_cast<float>(ini.GetDoubleValue("Hits", "fScaleMax", 3.0)));
		staggerHitBoost_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Stagger", "fHitBoost", 0.5)));
		oomphFrom_ = static_cast<float>(ini.GetDoubleValue("Hits", "fOomphFrom", 1.0));
		oomphTo_ = std::max(oomphFrom_ + 0.01f, static_cast<float>(ini.GetDoubleValue("Hits", "fOomphTo", 2.5)));
		oomphSpin_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fOomphSpin", 2.0)));
		oomphLean_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fOomphLean", 0.9)));
		oomphPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fOomphPush", 1.0)));
		reelAt_ = static_cast<float>(ini.GetDoubleValue("Hits", "fReelAt", 0.3));
		fallAt_ = static_cast<float>(ini.GetDoubleValue("Hits", "fFallAt", 1.01));
		reelTime_ = std::max(0.1f, static_cast<float>(ini.GetDoubleValue("Hits", "fReelTime", 0.8)));
		reelTimeAdd_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fReelTimeAdd", 1.6)));
		logger::info("oomph: from x{:.2f} to x{:.2f} damage scale; twist x(1 + {:.1f}), lean 0.3 + {:.1f}, push x(1 + {:.1f}); reels from {:.2f} ({:.1f} + {:.1f} x oomph s), falls from {:.2f}",
			oomphFrom_, oomphTo_, oomphSpin_, oomphLean_, oomphPush_, reelAt_, reelTime_, reelTimeAdd_, fallAt_);
		melee_ = ini.GetBoolValue("Melee", "bReact", true);
		blasts_ = ini.GetBoolValue("Explosions", "bPush", true);
		blastPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fPush", 90.0)));
		blastFar_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Explosions", "fFarShare", 0.4)), 0.0f, 1.0f);
		blastUp_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Explosions", "fUp", 0.25)), 0.0f, 1.0f);
		flySpeed_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fFlySpeed", 3.0)));
		flyUp_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fFlyUp", 0.65)));
		flyMin_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fFlyMin", 2.0)));
		flyMax_ = std::max(flyMin_, static_cast<float>(ini.GetDoubleValue("Explosions", "fFlyMax", 9.0)));
		highFallAt_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fHighFallAt", 4.5)));
		tumble_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fTumble", 10.0)));
		writheTime_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Down", "fWritheTime", 1.0)));
		writheAdd_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Down", "fWritheAdd", 1.2)));
		foetalChance_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Down", "fFoetalChance", 0.3)), 0.0f, 1.0f);
		meleeOomphFrom_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fOomphFrom", 1.3)));
		meleeOomphTo_ = std::max(meleeOomphFrom_ + 0.1f, static_cast<float>(ini.GetDoubleValue("Melee", "fOomphTo", 4.0)));
		meleeKnockAt_ = static_cast<float>(ini.GetDoubleValue("Melee", "fKnockAt", 1.01));
		headWhip_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fHeadWhip", 0.2)));
		meleeMaxPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fMaxPush", 160.0)));
		hitMaxPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fMaxPush", 150.0)));
		pelvisShare_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Hits", "fPelvisShare", 0.0)), 0.0f, 1.0f);
		maxSteps_ = std::clamp(static_cast<int>(ini.GetLongValue("Hits", "iMaxSteps", 10)), 0, 100);
		logger::info("explosions: push {} {:.0f} N s at the centre, {:.0f}% at the edge, up {:.2f}; thrown at {:.1f} m/s (x damage scale x nearness), up {:.2f} per, from {:.1f} to {:.1f} m/s; high fall from {:.1f} m/s, tumble {:.0f} N s per m/s",
			blasts_, blastPush_, blastFar_ * 100.0f, blastUp_, flySpeed_, flyUp_, flyMin_, flyMax_, highFallAt_, tumble_);
		logger::info("down: writhes {:.1f} + {:.1f} x oomph s, curls up {:.0f}%; melee oomph from x{:.1f} to x{:.1f} a plain blow, knocked down from {:.2f}, head whip {:.2f}",
			writheTime_, writheAdd_, foetalChance_ * 100.0f, meleeOomphFrom_, meleeOomphTo_, meleeKnockAt_, headWhip_);
		meleePush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fPush", 50.0)));
		meleePower_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fPower", 2.2)));
		meleeBash_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fBash", 1.6)));
		meleeSneak_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fSneak", 1.3)));
		meleeBlocked_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fBlocked", 0.5)));
		meleeWeightRef_ = std::max(0.1f, static_cast<float>(ini.GetDoubleValue("Melee", "fWeightRef", 9.0)));
		meleeHeftMin_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fHeftMin", 0.6)));
		meleeHeftMax_ = std::max(meleeHeftMin_, static_cast<float>(ini.GetDoubleValue("Melee", "fHeftMax", 1.7)));
		meleeSwing_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fSwing", 0.5)));
		meleeChop_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fChop", 0.35)));
		flinchBelow_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fFlinchBelow", 55.0)));
		logger::info("melee: {} push {:.0f} N s (power x{:.1f}, bash x{:.1f}, sneak x{:.1f}, blocked x{:.1f}), weight {:.0f} = x1 (x{:.1f}-{:.1f}), flinch under {:.0f}; stagger blow x(1 + {:.1f} x magnitude)",
			melee_, meleePush_, meleePower_, meleeBash_, meleeSneak_, meleeBlocked_, meleeWeightRef_, meleeHeftMin_, meleeHeftMax_, flinchBelow_, staggerHitBoost_);
		logger::info("staggers: played by Delirium {}, push {:.0f} N s at magnitude 1 ({:.0f}% at 0); hits push x{:.2f}-{:.2f}, x1 at {:.0f} damage, ^{:.2f}",
			staggers_, staggerPush_, staggerMin_ * 100.0f, damageMin_, damageMax_, damageRef_, damageExp_);
		logger::info("spells: shot reaction {}, burn {:.1f} s, shock {:.1f} s", spells_, burnTime_, shockTime_);
		wounded_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Arrows", "fWoundedBelow", 0.2)), 0.0f, 1.0f);
		logger::info("arrows: shot reaction {}, push {:.0f} N s, wounded below {:.0f}% health", arrows_, arrowPush_, wounded_ * 100.0f);
		logger::info("settings: GTA IV file '{}', character '{}', push key {:#x}, release key {:#x}, push {} N s, follow animation {}, take over Skyrim's ragdolls {}",
			fredPath_, buildPath_, pushKey_, releaseKey_, push_, followAnimation_, takeOver_);
	}

}
