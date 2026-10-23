#pragma once

// The settings table: every setting once, with where it lives in SkyrimDelirium.ini, its default, its limits and how
// the menu shows it. Loading and saving the ini, presets and the menu all work from it (table.cpp).
#include "shared.h"

#include <variant>

namespace delirium::settings
{
	// the menu's tabs
	enum class Tab
	{
		Reactions,   // what happens: each situation on or off, which behaviours
		Strength,    // how hard: pushes, damage scaling, how far past a plain hit
		Difficulty,  // how often: the chance a hit brings her into Delirium
		Timing,      // how long: burning, writhing, dying, going limp
		Advanced,    // Skyrim's ragdoll, world collision, test keys, files
	};

	enum Flags : unsigned
	{
		kNone = 0,
		kPreset = 1 << 0,   // saved in presets (tuning, not set-up: no files, keys or collision)
		kHidden = 1 << 1,   // not written in the shipped ini (dev settings); the ini may still set it
		kRestart = 1 << 2,  // read once at start-up: takes effect on the next game start or core reload
		kDetail = 1 << 3,   // fine tuning: the menu shows it under its tab's closed "Fine tuning" fold
	};

	using Value = std::variant<bool*, int*, std::uint32_t*, std::size_t*, float*, std::string*>;

	inline constexpr const char* kIniPath = "Data/SKSE/Plugins/SkyrimDelirium.ini";
	inline constexpr const char* kPresetDir = "Data/SKSE/Plugins/SkyrimDelirium/Presets";

	inline constexpr double kNoLimit = std::numeric_limits<double>::infinity();

	// a number's limits: lo / hi (either may be open), or at least another setting (+ gap)
	struct Limit
	{
		double       lo{ -kNoLimit }, hi{ kNoLimit };
		const float* atLeast{};
		float        gap{};
	};
	inline constexpr Limit kAny{};
	constexpr Limit Range(double a_lo, double a_hi) { return { a_lo, a_hi }; }
	constexpr Limit Min(double a_lo) { return { a_lo }; }
	inline Limit    AtLeast(const float& a_other, float a_gap = 0.0f) { return { -kNoLimit, kNoLimit, &a_other, a_gap }; }

	struct Setting
	{
		const char* section;
		const char* key;
		Value       value;
		double      def;      // a number's or bool's default
		const char* defText;  // a string's
		Limit       limit;
		Tab         tab;
		unsigned    flags;
		const char* label;  // the menu's name for it
		const char* help;   // its tooltip
		const char* oldKey{};  // an older name the ini may still use
		const float* cappedBy{};  // another setting that caps what this one gives (the menu warns past it)
		const char*  rangeLabel{};  // the low end of a pair (the next row is at least it): the menu shows both as one range
	};

	// the value read from the ini (or its default), within its limits
	void Read(const CSimpleIniA& a_ini, const Setting& a_s);
	// the same without setting it (limits against another setting use that one's value now)
	Any Parse(const CSimpleIniA& a_ini, const Setting& a_s);
	// a float within its limits (a_v as read)
	float Limited(const Setting& a_s, float a_v);
	// a number within its limits (whole numbers as the ini reader limits them)
	Any Limited(const Setting& a_s, const Any& a_v);

	Any         Get(const Setting& a_s);                     // its value now
	void        Put(const Setting& a_s, const Any& a_v);     // set it (a_v of its own kind, already within limits)
	Any         Default(const Setting& a_s);
	std::string Text(const Setting& a_s, const Any& a_v);    // as the ini writes it (1/0, 0x41 for keys, shortest float)
	inline std::string Text(const Setting& a_s) { return Text(a_s, Get(a_s)); }
	std::string Name(const Setting& a_s);                    // Section.key, as the log and the settings check name it
}
