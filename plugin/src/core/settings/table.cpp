// The settings table's rows: every setting, in the ini's order. A new setting is one row here (and its member in
// engine.h); loading, the check (tools\check-settings.ps1) and later the menu and presets pick it up.
#include "settings/table.h"

namespace delirium
{
	namespace
	{
		using namespace settings;
		constexpr unsigned P = kPreset;

		Setting B(const char* a_sec, const char* a_key, bool& a_v, bool a_def, Tab a_tab, unsigned a_flags, const char* a_label, const char* a_help)
		{
			return { a_sec, a_key, &a_v, a_def ? 1.0 : 0.0, nullptr, kAny, a_tab, a_flags, a_label, a_help };
		}
		Setting F(const char* a_sec, const char* a_key, float& a_v, double a_def, Limit a_lim, Tab a_tab, unsigned a_flags, const char* a_label, const char* a_help)
		{
			return { a_sec, a_key, &a_v, a_def, nullptr, a_lim, a_tab, a_flags, a_label, a_help };
		}
		template <class T>
		Setting I(const char* a_sec, const char* a_key, T& a_v, long a_def, Limit a_lim, Tab a_tab, unsigned a_flags, const char* a_label, const char* a_help)
		{
			return { a_sec, a_key, &a_v, static_cast<double>(a_def), nullptr, a_lim, a_tab, a_flags, a_label, a_help };
		}
		Setting S(const char* a_sec, const char* a_key, std::string& a_v, const char* a_def, Tab a_tab, unsigned a_flags, const char* a_label, const char* a_help,
			const char* a_oldKey = nullptr)
		{
			return { a_sec, a_key, &a_v, 0.0, a_def, kAny, a_tab, a_flags, a_label, a_help, a_oldKey };
		}
	}

	std::vector<settings::Setting> Engine::SettingsTable()
	{
		using enum Tab;
		return {
			// [Data]
			S("Data", "sGTAFile", fredPath_, "", Advanced, kRestart, "GTA IV Fred.art",
				"Full path to Fred.art in your own GTA IV install, for the behaviour closest to GTA IV. Empty: the mod's own character", "sCharacterFile"),
			S("Data", "sCharacter", buildPath_, "Data/SKSE/Plugins/SkyrimDelirium/skyrim_character.json", Advanced, kRestart, "Character file",
				"The mod's own character (body, joints, muscles), relative to the game folder"),
			S("Data", "sRig", rigPath_, "Data/SKSE/Plugins/SkyrimDelirium/skyrim_character_rig.txt", Advanced, kRestart, "Rig file",
				"How the character maps onto the Skyrim skeleton, relative to the game folder"),

			// [Test]
			I("Test", "iPushKey", pushKey_, 0x41, kAny, Advanced, kNone, "Push key", "DirectX scan code of the test push on the NPC under the crosshair (0x41 = F7)"),
			I("Test", "iReleaseKey", releaseKey_, 0x3C, kAny, Advanced, kNone, "Release key", "DirectX scan code that hands every body back to Skyrim (0x3C = F2)"),
			F("Test", "fPush", push_, 80.0, kAny, Advanced, kNone, "Test push", "The test key's push, N s: 80 stumbles, two or three in a row knock her down"),
			B("Test", "bAutoRelease", autoRelease_, true, Advanced, kNone, "Hand back when caught",
				"Hand back to Skyrim's animation once she has caught herself; a fallen body is held until Skyrim's get-up"),
			B("Test", "bFollowAnimation", followAnimation_, true, Advanced, kNone, "Follow the animation",
				"The body holds her live animation pose (guard up, gestures) instead of a fixed stance"),
			B("Test", "bRecordPoses", recordPoses_, false, Advanced, kHidden, "Record poses", "Dev: the held pose each frame while standing, for fit-test"),
			B("Test", "bStandingLegs", standingLegs_, true, Advanced, kHidden, "Standing legs", "Dev: the legs hold the body's own stance, not the animation's"),

			// [Ragdoll]
			B("Ragdoll", "bTakeOver", takeOver_, true, Reactions, P, "Take over Skyrim's ragdolls",
				"Delirium takes over every living NPC Skyrim ragdolls (Fus Ro Dah, pushes, explosions, other mods) until she gets up"),
			F("Ragdoll", "fTorqueGain", gain_, 1.0, kAny, Advanced, P, "Muscle strength", "Delirium's muscles on Skyrim's ragdoll bodies (1 = Delirium's own)"),
			F("Ragdoll", "fJointDamping", damp_, 0.0, Range(0.0, 1.0), Advanced, kHidden, "Joint damping", "Dev: share of a joint's spin its muscles take out each step"),
			F("Ragdoll", "fJointFriction", friction_, 0.0, Range(0.0, 1.0), Advanced, P, "Skyrim's joint friction",
				"Skyrim's own joint friction while Delirium drives (0 = off so the behaviours show, 1 = untouched)"),
			B("Ragdoll", "bMatchMass", matchMass_, true, Advanced, P, "Delirium's masses", "While Delirium drives, Skyrim's bodies weigh what Delirium's parts do"),
			B("Ragdoll", "bMatchContact", matchContact_, true, Advanced, P, "Delirium's grip and bounce", "And grip the ground and bounce as Delirium's parts do"),
			B("Ragdoll", "bTurnAboutJoint", aboutJoint_, true, Advanced, kHidden, "Turn about the joint", "Dev: a muscle turns the limb about its joint"),

			// [Arrows]
			B("Arrows", "bShotReaction", arrows_, true, Reactions, P, "Arrows", "An arrow that hits a living NPC: Delirium's shot reaction (stumble, clutch the wound, catch herself or fall)"),
			F("Arrows", "fPush", arrowPush_, 55.0, kAny, Strength, P, "Arrow push", "The arrow's push at the point it went in, N s (the test push is 80)"),
			F("Arrows", "fWoundedBelow", wounded_, 0.2, Range(0.0, 1.0), Strength, P, "Wounded below", "Under this share of her health a hit takes her down instead of her catching herself"),

			// [Spells]
			B("Spells", "bShotReaction", spells_, true, Reactions, P, "Spells",
				"Destruction spells' missiles hit like arrows: frost as an arrow, fire burns on top, shock jolts"),
			F("Spells", "fBurnTime", burnTime_, 2.5, Min(0.0), Timing, P, "Burning", "Seconds she burns after a fire hit (the arms beat at the flames)"),
			F("Spells", "fShockTime", shockTime_, 2.0, Min(0.0), Timing, P, "Shocked", "Seconds she is shocked after a lightning hit"),
			F("Spells", "fChannelLinger", channelLinger_, 0.6, Min(0.1), Timing, P, "Channel linger",
				"Channelled spells (Flames, Frostbite, Sparks): their reaction lasts this long after the last hit, s"),
			F("Spells", "fStartChance", spellChance_[0], 0.3, Range(0.0, 1.0), Difficulty, P, "Spell: chance at full health",
				"Chance a spell's missile brings her into Delirium at full health (in between by her health). Once in, every hit plays"),
			F("Spells", "fStartChanceHurt", spellChance_[1], 0.5, Range(0.0, 1.0), Difficulty, P, "Spell: chance near death",
				"The same near death. A killing blow and a spell that staggers always do"),
			F("Spells", "fChannelChance", channelChance_[0], 0.1, Range(0.0, 1.0), Difficulty, P, "Channel: chance at full health",
				"Chance per second of a channelled spell, at full health. Once in, every tick keeps her in"),
			F("Spells", "fChannelChanceHurt", channelChance_[1], 0.2, Range(0.0, 1.0), Difficulty, P, "Channel: chance near death", "The same near death"),

			// [Stagger]
			B("Stagger", "bReplace", staggers_, true, Reactions, P, "Staggers",
				"Every stagger (heavy blow, shield bash, Fus, magic, area staggers) is played by Delirium as a push away from the attacker"),
			F("Stagger", "fPush", staggerPush_, 90.0, Min(0.0), Strength, P, "Stagger push", "Push for the strongest stagger (magnitude 1), N s"),
			F("Stagger", "fMinShare", staggerMin_, 0.4, Range(0.0, 1.0), Strength, P, "Light stagger share", "A light stagger (magnitude 0) gets this share of the push"),
			F("Stagger", "fHitBoost", staggerHitBoost_, 0.5, Min(0.0), Strength, P, "Staggering blow boost",
				"A melee blow that staggers pushes as hard as the blow x (1 + this x the stagger's magnitude)"),

			// [Melee]
			B("Melee", "bReact", melee_, true, Reactions, P, "Melee blows",
				"While Delirium has her, every blow reacts: a light one flinches, a heavier one makes her step, past that she falls"),
			F("Melee", "fPush", meleePush_, 50.0, Min(0.0), Strength, P, "Blow push", "Push for a plain blow (iron sword, 15 damage), N s"),
			F("Melee", "fPower", meleePower_, 2.2, Min(0.0), Strength, P, "Power attack", "x the push for a power attack"),
			F("Melee", "fBash", meleeBash_, 1.6, Min(0.0), Strength, P, "Shield bash", "x the push for a shield bash"),
			F("Melee", "fSneak", meleeSneak_, 1.3, Min(0.0), Strength, P, "Sneak attack", "x the push for a sneak attack"),
			F("Melee", "fBlocked", meleeBlocked_, 0.5, Min(0.0), Strength, P, "Blocked blow", "x the push for a blocked blow"),
			F("Melee", "fWeightRef", meleeWeightRef_, 9.0, Min(0.1), Strength, P, "Weapon weight at 1x",
				"Heavier weapons push harder, by the square root of weight / this"),
			F("Melee", "fHeftMin", meleeHeftMin_, 0.6, Min(0.0), Strength, P, "Lightest weapon", "The weight factor at least (dagger 0.6)"),
			F("Melee", "fHeftMax", meleeHeftMax_, 1.7, AtLeast(meleeHeftMin_), Strength, P, "Heaviest weapon", "The weight factor at most (warhammer ~1.7)"),
			F("Melee", "fSwing", meleeSwing_, 0.5, Min(0.0), Strength, P, "Swing sweep", "How much a swing's sideways sweep bends the push (0 = straight away from the attacker)"),
			F("Melee", "fChop", meleeChop_, 0.35, Min(0.0), Strength, P, "Overhead chop", "An overhead chop pushes this much down"),
			F("Melee", "fFlinchBelow", flinchBelow_, 55.0, Min(0.0), Strength, P, "Flinch below", "Blows pushing less than this flinch, harder ones step, N s (the 'light' rows)"),
			F("Melee", "fOomphFrom", meleeOomphFrom_, 1.3, Min(0.0), Strength, P, "Heavy blow from", "A blow this many times a plain one's push starts to be heavy (oomph 0)"),
			F("Melee", "fOomphTo", meleeOomphTo_, 4.0, AtLeast(meleeOomphFrom_, 0.1f), Strength, P, "Heavy blow full", "At this many times it is fully heavy (oomph 1)"),
			F("Melee", "fKnockAt", meleeKnockAt_, 1.01, kAny, Strength, P, "Knocked down from", "From this oomph a blow takes her down (over 1: never)"),
			F("Melee", "fHeadWhip", headWhip_, 0.2, Min(0.0), Strength, P, "Head whip", "Share of a heavy blow's push at the head too (the head snaps)"),
			F("Melee", "fMaxPush", meleeMaxPush_, 160.0, Min(0.0), Strength, P, "Blow push at most",
				"A blow pushes at most this, N s (past it the stagger and fall carry it: one big shove looks limp)"),

			// [Explosions]
			B("Explosions", "bPush", blasts_, true, Reactions, P, "Explosions", "An explosion (Fireball, runes, explosive arrows) pushes everyone it catches away from its centre"),
			F("Explosions", "fPush", blastPush_, 90.0, Min(0.0), Strength, P, "Explosion push", "N s at the centre for a 15-damage explosion ([Hits] damage at 1x)"),
			F("Explosions", "fFarShare", blastFar_, 0.4, Range(0.0, 1.0), Strength, P, "At the edge", "Share of the push at the explosion's edge"),
			F("Explosions", "fUp", blastUp_, 0.25, Range(0.0, 1.0), Strength, P, "Upward", "The push's upward part (0 flat, 1 straight up)"),
			F("Explosions", "fFlySpeed", flySpeed_, 3.0, Min(0.0), Strength, P, "Thrown speed", "m/s she is thrown at, at the centre, for a 15-damage explosion"),
			F("Explosions", "fFlyUp", flyUp_, 0.65, Min(0.0), Strength, P, "Thrown upward", "m/s up per m/s along"),
			F("Explosions", "fFlyMin", flyMin_, 2.0, Min(0.0), Strength, P, "Thrown from", "Under this m/s she only takes the push, not thrown"),
			F("Explosions", "fFlyMax", flyMax_, 9.0, AtLeast(flyMin_), Strength, P, "Thrown at most", "m/s at most"),
			F("Explosions", "fHighFallAt", highFallAt_, 4.5, Min(0.0), Strength, P, "Thrown far from", "From this m/s she is thrown far ('far' rows: Delirium's high fall)"),
			F("Explosions", "fTumble", tumble_, 10.0, Min(0.0), Strength, P, "Tumble", "N s at the head per m/s thrown: she tumbles head first"),

			// [Down]
			F("Down", "fWritheTime", writheTime_, 1.0, Min(0.0), Timing, P, "Writhe", "Knocked down, she writhes this many s + the next x oomph (half for a blow)"),
			F("Down", "fWritheAdd", writheAdd_, 1.2, Min(0.0), Timing, P, "Writhe per oomph", "Extra s of writhing for a harder hit"),
			F("Down", "fFoetalChance", foetalChance_, 0.3, Range(0.0, 1.0), Reactions, P, "Curl up chance", "Chance she curls up on the ground instead (when no down row decides)"),

			// [Hits]
			F("Hits", "fDamageRef", damageRef_, 15.0, Min(0.1), Strength, P, "Damage at 1x", "A hit of this much damage pushes at 1x (arrows, spells, melee, staggers)"),
			F("Hits", "fExponent", damageExp_, 0.8, Range(0.0, 2.0), Strength, P, "Damage curve", "Twice the damage pushes 2^this as hard (1.74x at 0.8)"),
			F("Hits", "fScaleMin", damageMin_, 0.4, Min(0.0), Strength, P, "Damage scale at least", "The damage scale at least"),
			F("Hits", "fScaleMax", damageMax_, 3.0, AtLeast(damageMin_), Strength, P, "Damage scale at most", "The damage scale at most"),
			F("Hits", "fOomphFrom", oomphFrom_, 1.0, kAny, Strength, P, "Hard hit from", "Damage scale where a hit starts jolting harder (oomph 0)"),
			F("Hits", "fOomphTo", oomphTo_, 2.5, AtLeast(oomphFrom_, 0.01f), Strength, P, "Hard hit full", "Damage scale for the full jolt (oomph 1)"),
			F("Hits", "fOomphSpin", oomphSpin_, 2.0, Min(0.0), Strength, P, "Hard hit twist", "The shot twists her x (1 + this x oomph)"),
			F("Hits", "fOomphLean", oomphLean_, 0.9, Min(0.0), Strength, P, "Hard hit lean", "The shot leans her back 0.3 + this x oomph"),
			F("Hits", "fOomphPush", oomphPush_, 1.0, Min(0.0), Strength, P, "Hard hit push", "The push x (1 + this x oomph)"),
			F("Hits", "fReelAt", reelAt_, 0.3, kAny, Strength, P, "Reels from", "From this oomph she reels back on loose legs, then catches herself"),
			F("Hits", "fFallAt", fallAt_, 1.01, kAny, Strength, P, "Reel falls from", "From this oomph the reel takes her down (over 1: only if she can't catch it)"),
			F("Hits", "fReelTime", reelTime_, 0.8, Min(0.1), Timing, P, "Reel", "She reels this many s + the next x oomph"),
			F("Hits", "fReelTimeAdd", reelTimeAdd_, 1.6, Min(0.0), Timing, P, "Reel per oomph", "Extra s of reeling for a harder hit"),
			F("Hits", "fMaxPush", hitMaxPush_, 150.0, Min(0.0), Strength, P, "Hit push at most",
				"A spell's or arrow's push at most, N s (past it one shove knocks her over limp)"),
			F("Hits", "fPelvisShare", pelvisShare_, 0.0, Range(0.0, 1.0), Strength, P, "At the pelvis",
				"Share of a standing hit's push at the pelvis instead of the part hit: catch steps instead of folding at the waist"),
			I("Hits", "iMaxSteps", maxSteps_, 10, Range(0.0, 100.0), Strength, P, "Catch steps", "Most catch steps a hit takes before she stops stepping (Delirium's default 6)"),

			// [Collision]
			B("Collision", "bWorldCollision", collision_, true, Advanced, kNone, "World collision",
				"The body collides with Skyrim's world (floors, stairs, walls, terrain) instead of a flat floor"),
			F("Collision", "fRadius", colRadius_, 600.0, kAny, Advanced, kNone, "Around her", "How far around her to take the world, game units (70 = 1 m)"),
			F("Collision", "fHeight", colHeight_, 300.0, kAny, Advanced, kNone, "Above and below", "How far up and down, game units"),
			I("Collision", "iMaxTriangles", colMaxTris_, 40000, Min(1000.0), Advanced, kNone, "Triangles at most", "The most world triangles taken (performance)"),

			// [Deaths]
			B("Deaths", "bNoKillMoves", noKillMoves_, true, Reactions, P | kRestart, "No killmoves",
				"Skyrim's killmoves off (a save keeps it: `set killmove to 1` brings them back)"),
			B("Deaths", "bNoKillCams", noKillCams_, true, Reactions, P | kRestart, "No kill cams", "Kill cams off"),
			F("Deaths", "fDyingTime", dyingTime_, 3.0, Range(0.0, 10.0), Timing, P, "Dying on her feet",
				"Seconds a killed NPC may die on her feet in Delirium before the death goes through (0 = die at once)"),
			F("Deaths", "fCollapseAfterMin", collapseMin_, 0.8, Range(0.0, 10.0), Timing, P, "Collapses after (min)", "Dying on her feet: s she fights to stay up, at least"),
			F("Deaths", "fCollapseAfterMax", collapseMax_, 1.6, AtLeast(collapseMin_), Timing, P, "Collapses after (max)", "And at most"),
			F("Deaths", "fDyingHealth", dyingHealth_, 70.0, Range(0.0, 99.0), Strength, P, "Killing blow health",
				"The killing blow as Delirium's shot at this health (0 = a limp flop, under 100 = wounded, falls)"),
			F("Deaths", "fLimpAfterMin", limpMin_, 1.8, Min(0.2), Timing, P, "Limp after (min)", "Seconds until the corpse goes limp, at least"),
			F("Deaths", "fLimpAfterMax", limpMax_, 3.0, AtLeast(limpMin_), Timing, P, "Limp after (max)", "And at most"),
			F("Deaths", "fWritheChance", dyingWrithe_, 0.5, Range(0.0, 1.0), Reactions, P, "Death writhe chance", "Chance of a weak writhe on the ground first"),
			F("Deaths", "fCatchFallChance", dyingCatch_, 0.5, Range(0.0, 1.0), Reactions, P, "Break the fall chance", "Chance the arms go out to break the fall"),
			F("Deaths", "fDeathPush", deathPush_, 1.5, Range(0.0, 6.0), Strength, P, "Death push", "m/s away from the killer as she dies (in place of Skyrim's lunge toward him)"),
			F("Deaths", "fDeathMaxSpeed", deathMaxSpeed_, 6.0, Min(1.0), Strength, P, "Death speed cap", "m/s cap on any body as she dies (Skyrim's death throws limbs at 17 m/s)"),
			F("Deaths", "fShotTime", dyingReact_, 1.2, Range(0.0, 5.0), Timing, P, "Dying shot", "Seconds the dying shot (reaching for the wound, breaking the fall) plays before a writhe"),

			// [Shouts]
			F("Shouts", "fThrowFrom", shoutThrowFrom_, 0.6, Range(0.0, 1.01), Strength, P, "Shout throws from",
				"A shout's stagger from this magnitude throws her (1 word ~0.37, 2 ~0.73, 3 ~0.98)"),
			F("Shouts", "fFlySpeed", shoutFly_, 9.0, Min(0.0), Strength, P, "Shout throw speed", "m/s she is thrown at, at magnitude 1"),

			// [Reactions] (the rows themselves are read by LoadReactions)
			F("Reactions", "fHeavyOomph", heavyOomph_, 0.5, Range(0.0, 1.01), Reactions, P, "Heavy from", "From this oomph a blow picks the 'heavy' rows"),
		};
	}

}
