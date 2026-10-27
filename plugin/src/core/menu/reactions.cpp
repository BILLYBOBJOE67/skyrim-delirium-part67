// The menu's reaction table editor (Reactions page): every row by situation, editable as text, with the rows a
// situation can have offered to add. A row's syntax is the ini's ([Reactions] in SkyrimDelirium.ini).
#include "shared.h"
#include "menu/menu.h"

namespace delirium
{
	namespace
	{
		constexpr const char* kTypes[]{ "melee", "arrow", "spell", "blast", "stagger", "knock", "shout", "bump", "other" };
		constexpr const char* kElements[]{ "arrow", "frost", "fire", "shock", "magic" };
		constexpr const char* kBands[]{ "light", "heavy", "lethal" };

		struct Phase
		{
			const char* key;    // the rows' first word (fire / shock / frost: the element rows)
			const char* title;
			const char* help;
		};
		constexpr Phase kPhases[]{
			{ "stand", "Standing: a blow while standing",
				"stand.type.strength, then stand.strength, stand.type, stand: the most exact row wins.\n"
				"Base: shot[:health] (Delirium's shot: 100+ stays up, under 100 hurt and falls), flinch (guard up, turned to the attacker), "
				"step (balance, catch it), none (push only).\n"
				"Layers: spin (the jolt twists the spine), headsnap[:x], push:x, collapse (lethal: goes down), burn[:s], shock[:s], chill[:s], throw[:m/s] (thrown as by an explosion).\n"
				"Strength: light (a push under fFlinchBelow), heavy (oomph from fHeavyOomph), lethal (the killing blow). "
				"Shouts: word1, word2, word3 (Fus, Fus Ro, Fus Ro Dah) in place of light / heavy." },
			{ "element", "Elements: what every blow of an element adds",
				"fire / shock / frost: layers every blow of that element gets on top of its row, e.g. burn, shock, chill[:s], throw[:m/s]." },
			{ "fly", "Thrown: in the air",
				"fly.element.far, fly.element, fly.type.far, fly.type, fly.far (from fHighFallAt), fly.\n"
				"Choices by weight: windmill (arms windmill, legs pedal), rollup, highfall, foetal (curled, legs kick); e.g. foetal:50, windmill:30.\n"
				"Layers: thrash (the spine thrashes, as on fire), shock (electrocuted), tumble:x." },
			{ "downhit", "Hit while down",
				"downhit.type, downhit. react (the NPC reacts on the ground; none: only moved), push:x, time:x (how long)." },
			{ "down", "Once down: how the NPC lies",
				"down.element, down. With no row: [Down] fFoetalChance (foetal, else 60% writhe, 40% roll).\n"
				"Choices by weight: writhe, roll, foetal, still. Layers: amp:x, period:s (the writhe's size and s per cycle), shock, shiver." },
			{ "die", "Dying: as the NPC dies",
				"die.held(.element) after dying standing, else die.element, die.type, die. With no row: from [Deaths].\n"
				"shot[:health] (the dying shot; none: no shot), push[:x] (fDeathPush x), catch:chance (arms out to break the fall), "
				"writhe:chance (a weak writhe before limp), amp:x, period:s. E.g. die.fire=shot:40, push, writhe:1, amp:1.6, period:1.5" },
		};

		std::string_view First(std::string_view a_key) { return a_key.substr(0, a_key.find('.')); }

		const char* PhaseOf(std::string_view a_key)
		{
			const auto f = First(a_key);
			return f == "fire" || f == "shock" || f == "frost" ? "element" : nullptr;
		}

		bool InPhase(std::string_view a_key, const Phase& a_p)
		{
			const char* e = PhaseOf(a_key);
			return e ? std::strcmp(a_p.key, e) == 0 : First(a_key) == a_p.key;
		}

		// "stand.arrow.heavy" -> "arrows, heavy". Short: the row's text is drawn beside it (what each word covers: Covers)
		std::string Situation(std::string_view a_key)
		{
			static constexpr std::pair<std::string_view, const char*> kWords[]{ { "melee", "melee hits" }, { "arrow", "arrows" }, { "spell", "spells" },
				{ "blast", "explosions" }, { "stagger", "staggers, no hit" }, { "knock", "knocked down by Skyrim" }, { "shout", "shouts" },
				{ "bump", "run into" }, { "other", "anything else" }, { "frost", "frost" }, { "fire", "fire" }, { "shock", "shock" }, { "magic", "other magic" },
				{ "light", "light" }, { "heavy", "heavy" }, { "lethal", "killing blow" }, { "far", "far (high fall)" }, { "held", "after dying standing" },
				{ "word1", "1st word (Fus)" }, { "word2", "2nd word (Fus Ro)" }, { "word3", "3rd word (Fus Ro Dah)" } };
			if (a_key.find(".stagger.light") != std::string_view::npos) {
				return "weak staggers, no hit";
			}
			std::string out;
			std::size_t s = a_key.find('.');
			if (PhaseOf(a_key)) {
				return std::format("every {} blow", a_key);
			}
			if (s == std::string_view::npos) {
				return "any";
			}
			while (s != std::string_view::npos) {
				const auto e = a_key.find('.', s + 1);
				const auto w = a_key.substr(s + 1, e == std::string_view::npos ? std::string_view::npos : e - s - 1);
				const auto it = std::find_if(std::begin(kWords), std::end(kWords), [&](const auto& p) { return p.first == w; });
				out += (out.empty() ? "" : ", ") + std::string(it != std::end(kWords) ? it->second : w);
				s = e;
			}
			return out;
		}

		// what a row's words cover, for its tooltip. A melee blow that staggers is "melee"; "stagger" is a stagger with no
		// blow behind it (Stagger in reactions/hits.cpp): the two a player needs told apart.
		std::string Covers(std::string_view a_key)
		{
			std::string out;
			const auto  has = [&](std::string_view w) { return (std::string(a_key.substr(a_key.find('.') + 1)) + ".").find(std::string(w) + ".") != std::string::npos; };
			if (a_key.find('.') == std::string_view::npos) {
				return out;
			}
			if (has("melee")) {
				out += "Melee hits: every one, the ones that stagger too (power attacks, bashes, hard swings), pushed by the blow.\n";
			}
			if (has("stagger")) {
				out += "Staggers with no melee hit behind them: an enemy recoiling off your block, a stagger spell or effect, an explosion's stagger.\n";
			}
			if (has("bump")) {
				out += "Run into: you (or anyone) sprinting or running into the NPC, in place of Skyrim's stumble. They still step out of the way.\n";
			}
			if (has("light")) {
				out += has("stagger") ? "Weak: the stagger pushes under Flinch below (Melee, Fine tuning). Most block recoils.\n"
				                      : "Light: the blow pushes under Flinch below (Melee, Fine tuning).\n";
			}
			if (has("word1") || has("word2") || has("word3")) {
				out += "A shout of that many words (read from the spell that hit, not from how hard it staggers). Push:x pushes harder, throw:m/s throws (then the Thrown rows).\n";
			}
			if (has("heavy")) {
				out += "Heavy: the blow's oomph from Heavy from (Reactions). A stagger with no hit is never heavy.\n";
			}
			return out;
		}

		// the rows a situation can have, in the order a player looks for them
		std::vector<std::string> Candidates(const Phase& a_p)
		{
			std::vector<std::string> out;
			const std::string        p = a_p.key;
			if (p == "element") {
				return { "fire", "shock", "frost" };
			}
			out.push_back(p);
			if (p == "stand") {
				for (const char* b : kBands) {
					out.push_back(p + "." + b);
				}
				for (const char* t : kTypes) {
					out.push_back(p + "." + t);
					if (std::strcmp(t, "shout") == 0) {
						for (const char* w : { "word1", "word2", "word3", "lethal" }) {
							out.push_back(p + ".shout." + w);
						}
						continue;
					}
					for (const char* b : kBands) {
						out.push_back(p + "." + t + "." + b);
					}
				}
			} else if (p == "fly") {
				out.push_back("fly.far");
				std::vector<std::string> what(std::begin(kTypes), std::end(kTypes));
				for (const char* e : kElements) {
					if (std::find(what.begin(), what.end(), e) == what.end()) {
						what.emplace_back(e);
					}
				}
				for (const auto& w : what) {
					out.push_back("fly." + w);
					out.push_back("fly." + w + ".far");
				}
			} else if (p == "downhit") {
				for (const char* t : kTypes) {
					out.push_back(p + "." + t);
				}
			} else if (p == "down") {
				for (const char* e : kElements) {
					out.push_back(p + "." + e);
				}
			} else if (p == "die") {
				for (const char* t : kTypes) {
					out.push_back(p + "." + t);
				}
				for (const char* e : kElements) {
					if (std::none_of(std::begin(kTypes), std::end(kTypes), [&](const char* t) { return std::strcmp(t, e) == 0; })) {
						out.push_back(p + "." + e);
					}
				}
				out.push_back("die.held");
				for (const char* e : kElements) {
					out.push_back(std::string("die.held.") + e);
				}
			}
			return out;
		}

		std::map<std::string, std::string> g_typing;  // rows being typed in (applied when the field is left)
		bool                               g_asText{};  // the rows as text instead of controls

		// ---- a row as controls: the words a phase knows, each a choice of base, a layer on or off, or a weight

		enum class Kind
		{
			Base,    // the row's first word: one of the phase's bases (a valued one, e.g. shot:150, also has a slider)
			Flag,    // on or off
			Value,   // on or off, with a number (left out: its default)
			Weight,  // one of several the phase picks from at random, by weight
		};
		struct Word
		{
			const char* name;  // as written in the row ("" for a base that writes nothing: die's "no shot")
			Kind        kind;
			const char* label;
			const char* help;
			float       lo{}, hi{};  // a number's slider (lo == hi: no number)
			float       def{ std::numeric_limits<float>::quiet_NaN() };  // with no number given (NaN: autoText says what)
			const char* autoText{};  // the slider's text with no number given, when it has no single default
		};

		// a row's words as the controls see them: the base, every known word's number (NaN: none given), the rest as written
		struct Parsed
		{
			std::string                  base;
			std::map<std::string, float> on;
			std::vector<std::string>     unknown;
		};

		std::string Num(float a_v)
		{
			return std::format("{}", std::round(a_v * 100.0f) / 100.0f);
		}

		const Word* Find(const std::vector<Word>& a_words, std::string_view a_name, bool a_base)
		{
			for (const auto& w : a_words) {
				if (a_name == w.name && (w.kind == Kind::Base) == a_base) {
					return &w;
				}
			}
			return nullptr;
		}

		// the base a row with none of the phase's bases gets: stand steps, downhit is only moved, die has no shot
		std::string Fallback(const std::vector<Word>& a_words)
		{
			const Word* last = nullptr;
			for (const auto& w : a_words) {
				if (w.kind == Kind::Base) {
					last = &w;
				}
			}
			return last ? last->name : "";
		}

		Parsed Parse(const std::vector<Word>& a_words, const std::string& a_text)
		{
			Parsed     p;
			const bool hasBases = std::any_of(a_words.begin(), a_words.end(), [](const Word& w) { return w.kind == Kind::Base; });
			bool       first = true;
			for (const auto& [n, v] : Engine::RowWords(a_text)) {
				if (first && hasBases && Find(a_words, n, true)) {
					p.base = n;
					if (!std::isnan(v)) {
						p.on[n] = v;
					}
				} else if (Find(a_words, n, false)) {
					p.on[n] = v;
				} else {
					p.unknown.push_back(std::isnan(v) ? n : n + ":" + Num(v));
				}
				first = false;
			}
			if (hasBases && p.base.empty()) {
				p.base = Fallback(a_words);
			}
			return p;
		}

		std::string Write(const std::vector<Word>& a_words, const Parsed& a_p)
		{
			std::vector<std::string> items;
			if (!a_p.base.empty()) {
				const auto it = a_p.on.find(a_p.base);
				items.push_back(it != a_p.on.end() && !std::isnan(it->second) ? a_p.base + ":" + Num(it->second) : a_p.base);
			}
			for (const auto& w : a_words) {
				if (w.kind == Kind::Base) {
					continue;
				}
				if (const auto it = a_p.on.find(w.name); it != a_p.on.end()) {
					items.push_back(std::isnan(it->second) ? std::string(w.name) : std::string(w.name) + ":" + Num(it->second));
				}
			}
			items.insert(items.end(), a_p.unknown.begin(), a_p.unknown.end());
			std::string out;
			for (const auto& i : items) {
				out += (out.empty() ? "" : ", ") + i;
			}
			return out;
		}
	}

	namespace
	{
		// what each phase's rows can say (the engine reads exactly these: react.cpp, dying.cpp, body/update.cpp)
		std::vector<Word> Words(std::string_view a_phase, float a_burn, float a_shock, float a_chill, float a_dyingHealth)
		{
			using enum Kind;
			const Word burn{ "burn", Value, "Burns", "The NPC also burns for this many s", 0.0f, 10.0f, a_burn };
			const Word shock{ "shock", Value, "Shocked", "The NPC is also shocked for this many s", 0.0f, 10.0f, a_shock };
			const Word chill{ "chill", Value, "Chilled", "The NPC is also chilled (slowed, shivering) for this many s", 0.0f, 10.0f, a_chill };
			const Word thrown{ "throw", Value, "Throw", "Thrown along the blow at this many m/s, as an explosion throws (then the Thrown rows play); "
							   "5 is a hard shove, 9 an explosion's most, 15 across the room", 0.0f, 20.0f, 6.0f };
			if (a_phase == "stand") {
				return {
					{ "shot", Base, "Shot", "Delirium's shot: wounded where it hit, fighting to stay up. Health 100 and up stays up and catches a fall, under 100 is hurt and falls",
						0.0f, 200.0f, std::numeric_limits<float>::quiet_NaN(), "health: from the NPC's" },
					{ "flinch", Base, "Flinch", "Guard up, turned to whoever did it (with no one: a step)" },
					{ "none", Base, "Push only", "No behaviour: only the push moves the NPC" },
					{ "step", Base, "Step", "Keeps balance with catch steps; past what the steps can catch the NPC falls" },
					{ "spin", Flag, "Spin", "The jolt twists the spine; a harder hit twists more" },
					{ "headsnap", Value, "Head snap", "The head snaps back with the blow, x this", 0.0f, 3.0f, 1.0f },
					{ "push", Value, "Push", "The push, x this", 0.0f, 3.0f, 1.0f },
					{ "collapse", Flag, "Collapse", "Killing blow rows: the NPC stops fighting it and goes down" },
					burn, shock, chill, thrown,
				};
			}
			if (a_phase == "element") {
				return { burn, shock, chill, thrown };
			}
			if (a_phase == "fly") {
				return {
					{ "foetal", Weight, "Curled up", "Curled up, legs kick", 0.0f, 100.0f, 1.0f },
					{ "windmill", Weight, "Windmill", "Arms windmill, legs pedal, the spine writhes", 0.0f, 100.0f, 1.0f },
					{ "rollup", Weight, "Rolled up", "Rolled up, legs pedal", 0.0f, 100.0f, 1.0f },
					{ "highfall", Weight, "High fall", "Delirium's high fall: arms and legs out, bracing for the landing", 0.0f, 100.0f, 1.0f },
					{ "thrash", Flag, "Thrash", "The windmill's spine thrashes, as on fire" },
					{ "shock", Flag, "Shocked", "Electrocuted in the air" },
					{ "tumble", Value, "Tumble", "The head-first tumble, x this", 0.0f, 3.0f, 1.0f },
				};
			}
			if (a_phase == "downhit") {
				return {
					{ "react", Base, "Reacts", "The NPC reacts on the ground (curls, writhes) for a while" },
					{ "none", Base, "Only moved", "The blow only moves the part it hit" },
					{ "push", Value, "Push", "The push, x this (a body on the ground takes less: 0.6)", 0.0f, 3.0f, 1.0f },
					{ "time", Value, "Time", "How long the NPC reacts, x this", 0.0f, 3.0f, 1.0f },
				};
			}
			if (a_phase == "down") {
				return {
					{ "writhe", Weight, "Writhe", "Writhing on the ground", 0.0f, 100.0f, 1.0f },
					{ "roll", Weight, "Roll", "Writhing and rolling over", 0.0f, 100.0f, 1.0f },
					{ "foetal", Weight, "Curled up", "Curled up", 0.0f, 100.0f, 1.0f },
					{ "still", Weight, "Still", "Lies still", 0.0f, 100.0f, 1.0f },
					{ "amp", Value, "Writhe size", "How big the writhe is", 0.0f, 3.0f, std::numeric_limits<float>::quiet_NaN(), "from how hard" },
					{ "period", Value, "Writhe speed", "s per writhe cycle (smaller is faster)", 0.1f, 3.0f, std::numeric_limits<float>::quiet_NaN(), "from how hard" },
					{ "shock", Flag, "Convulses", "Convulsing (electrocuted)" },
					{ "shiver", Flag, "Shivers", "Shivering" },
				};
			}
			if (a_phase == "die") {
				return {
					{ "shot", Base, "Dying shot", "Reaching for the wound, breaking the fall, at this health (0: a limp flop, under 100 hurt)",
						0.0f, 99.0f, a_dyingHealth },
					{ "", Base, "No shot", "No dying shot: the NPC goes straight to what follows" },
					{ "push", Value, "Push", "Pushed away from the killer, x [Deaths] Death push", 0.0f, 4.0f, 1.0f },
					{ "catch", Value, "Break the fall", "Chance the arms go out to break the fall", 0.0f, 1.0f, 1.0f },
					{ "writhe", Value, "Writhe first", "Chance of a weak writhe on the ground before the NPC goes limp", 0.0f, 1.0f, 1.0f },
					{ "amp", Value, "Writhe size", "That writhe's size", 0.0f, 3.0f, std::numeric_limits<float>::quiet_NaN(), "the default" },
					{ "period", Value, "Writhe speed", "That writhe's s per cycle", 0.1f, 3.0f, std::numeric_limits<float>::quiet_NaN(), "the default" },
				};
			}
			return {};
		}

		// a number's slider: with none given it shows the default (or what decides it) and dragging gives one
		bool Number(const Word& a_w, float& a_v, bool& a_done)
		{
			using namespace ImGuiMCP;
			const bool  chance = a_w.hi <= 1.0f && a_w.lo >= 0.0f && (std::string_view(a_w.name) == "catch" || std::string_view(a_w.name) == "writhe");
			const bool  given = !std::isnan(a_v);
			float       shown = given ? a_v : !std::isnan(a_w.def) ? a_w.def : (a_w.lo + a_w.hi) * 0.5f;
			std::string fmt;
			if (given || !a_w.autoText) {
				fmt = chance ? "%.0f %%" : a_w.hi - a_w.lo > 20.0f ? "%.0f" : "%.2f";
				if (!given) {
					fmt += "  (default)";
				}
			} else {
				fmt = a_w.autoText;
			}
			if (chance) {
				shown *= 100.0f;
			}
			SetNextItemWidth(GetContentRegionAvail().x * 0.35f);
			const bool c = SliderFloat("##n", &shown, chance ? a_w.lo * 100.0f : a_w.lo, chance ? a_w.hi * 100.0f : a_w.hi, fmt.c_str(), ImGuiSliderFlags_AlwaysClamp);
			if (c) {
				a_v = chance ? shown / 100.0f : shown;
			}
			a_done = IsItemDeactivatedAfterEdit();
			return c;
		}
	}

	namespace
	{
		std::string g_dragRow, g_dragWas;  // the row whose slider is held, and its text before

		// one row's controls; true when the text changed (a_done: the edit is over, log it)
		bool Controls(const std::vector<Word>& a_words, std::string& a_text, bool& a_done)
		{
			using namespace ImGuiMCP;
			Parsed p = Parse(a_words, a_text);
			bool   changed = false;
			a_done = false;
			// the base: a choice, with its number if it has one
			if (const Word* b = Find(a_words, p.base, true)) {
				SetNextItemWidth(GetContentRegionAvail().x * 0.25f);
				if (BeginCombo("##base", b->label)) {
					for (const auto& w : a_words) {
						if (w.kind == Kind::Base && Selectable(w.label, p.base == w.name)) {
							if (p.base != w.name) {
								p.on.erase(p.base);
								p.base = w.name;
								changed = a_done = true;
							}
						}
						if (w.kind == Kind::Base) {
							menu::Help(w.help);
						}
					}
					EndCombo();
				}
				menu::Help(std::string(b->help) + "\n\nHow the NPC reacts: the row's base behaviour. The layers below add to it.");
				if (b->hi > b->lo) {
					SameLine();
					auto  it = p.on.find(p.base);
					float v = it != p.on.end() ? it->second : std::numeric_limits<float>::quiet_NaN();
					bool  done = false;
					if (Number(*b, v, done)) {
						p.on[p.base] = v;
						changed = true;
					}
					a_done |= done;
					menu::Help(b->help);
					if (!std::isnan(v)) {
						SameLine();
						if (SmallButton("auto##b")) {
							p.on.erase(p.base);
							changed = a_done = true;
						}
						menu::Help(b->autoText ? std::format("Back to {}", b->autoText) : "Back to the default");
					}
				}
			}
			// the layers and weights
			int n = 0;
			for (const auto& w : a_words) {
				if (w.kind == Kind::Base) {
					continue;
				}
				PushID(w.name);
				auto       it = p.on.find(w.name);
				bool       on = it != p.on.end();
				const bool valued = w.hi > w.lo;
				if (!valued && n++ % 4 != 0) {
					SameLine(0.0f, 18.0f);  // flags side by side
				}
				if (Checkbox(w.label, &on)) {
					if (on) {
						p.on[w.name] = std::numeric_limits<float>::quiet_NaN();
					} else {
						p.on.erase(w.name);
					}
					changed = a_done = true;
				}
				menu::Help(w.kind == Kind::Weight ? std::format("{}\n\nOne of the ticked ones plays, picked at random by weight.", w.help) : w.help);
				if (valued) {
					n = 0;
					if (on) {
						SameLine(GetFontSize() * 9.0f);
						float v = p.on[w.name];
						bool  done = false;
						if (Number(w, v, done)) {
							p.on[w.name] = v;
							changed = true;
						}
						a_done |= done;
						menu::Help(w.help);
						if (!std::isnan(v) && w.kind != Kind::Weight) {
							SameLine();
							if (SmallButton("default")) {
								p.on[w.name] = std::numeric_limits<float>::quiet_NaN();
								changed = a_done = true;
							}
						}
					}
				}
				PopID();
			}
			if (!p.unknown.empty()) {
				std::string list;
				for (const auto& u : p.unknown) {
					list += (list.empty() ? "" : ", ") + u;
				}
				TextColored(menu::kBad, "not known, ignored: %s", list.c_str());
				menu::Help("Words this phase doesn't use (a typo, or one for another phase). Kept in the row; 'Edit as text' to remove them.");
			}
			if (changed) {
				a_text = Write(a_words, p);
			}
			return changed;
		}
	}

	void Engine::MenuReactionsHelp()
	{
		using namespace ImGuiMCP;
		TextWrapped(
			"Each row says what happens in one situation. A blow looks for the most exact row there is: for a standing hit, "
			"stand.<type>.<strength>, then stand.<strength>, then stand.<type>, then stand. So 'stand.arrow.heavy' (a hard arrow "
			"hit) wins over 'stand.arrow' (any arrow), which wins over 'stand' (anything). Add a row only where you want a "
			"situation to differ; remove it to fall back to the less exact one.");
		BulletText("Types: melee, arrow, spell, blast (explosion), stagger, knock (Skyrim knocked the NPC down), shout, other.");
		BulletText("Staggers: one from a melee hit (a power attack, a bash, a hard swing) is a melee row, pushed by the blow. "
				   "'stagger' rows are staggers with no melee hit behind them: an enemy recoiling off your block, a stagger spell "
				   "or effect, an explosion's stagger. Most block recoils are weak, so they use stand.stagger.light.");
		BulletText("Strength: light (a push under Melee 'Flinch below'), heavy (from 'Heavy from' oomph), lethal (the killing blow).");
		BulletText("Shouts: one row per word (stand.shout.word1, word2, word3); throw:m/s on a word's row throws, then the Thrown rows pick the fall.");
		BulletText("Elements (fire, shock, frost): their row's layers go on top of every blow of that element.");
		BulletText("The Core log names the row each hit used, e.g. [stand.arrow: shot, spin, headsnap].");
		for (const auto& ph : kPhases) {
			if (!TreeNode(ph.title)) {
				continue;
			}
			TextWrapped("%s", ph.help);
			for (const auto& w : Words(ph.key, burnTime_, shockTime_, channelLinger_, dyingHealth_)) {
				const char* kind = w.kind == Kind::Base ? "base" : w.kind == Kind::Weight ? "pick by weight" : w.hi > w.lo ? "with a number" : "on / off";
				BulletText("%s (%s, %s): %s", w.label, *w.name ? w.name : "nothing written", kind, w.help);
			}
			TreePop();
		}
		SeparatorText("As text");
		TextWrapped(
			"'Edit as text' on the Reactions page shows the rows as the ini writes them: the base first, then layers, separated "
			"by commas; name:number gives a number, e.g.  shot:150, spin, headsnap:1.5  or, for a throw,  foetal:50, windmill:30. "
			"A word left without a number takes its default.");
	}

	// One row on its own (the Quick page). A row that isn't in the table shows the row deciding it now (the order PickRule
	// and Fly look them up in); a change adds it, so only that situation changes.
	void Engine::MenuRow(const std::string& a_key, const char* a_label)
	{
		using namespace ImGuiMCP;
		const Phase* ph = nullptr;
		for (const auto& p : kPhases) {
			if (InPhase(a_key, p)) {
				ph = &p;
				break;
			}
		}
		if (!ph) {
			return;
		}
		const auto rows = Rows();
		const auto text = [&](const std::string& a_k) -> const std::string* {
			const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.first == a_k; });
			return it != rows.end() ? &it->second : nullptr;
		};
		const std::string* own = text(a_key);
		std::string        t = own ? *own : "", from;
		if (!own) {
			// phase.type.strength: stand looks at phase.strength before phase.type, fly the other way round
			std::vector<std::string> next;
			const auto               a = a_key.find('.'), b = a_key.find('.', a == std::string::npos ? a : a + 1);
			if (b != std::string::npos) {
				const std::string p = a_key.substr(0, a), ty = a_key.substr(0, b), st = p + a_key.substr(b);
				next = First(a_key) == "fly" ? std::vector{ ty, st, p } : std::vector{ st, ty, p };
			} else if (a != std::string::npos) {
				next = { a_key.substr(0, a) };
			}
			for (const auto& k : next) {
				if (const auto* x = text(k)) {
					t = *x;
					from = k;
					break;
				}
			}
		}
		PushID(a_key.c_str());
		const bool open = TreeNodeEx("##q", ImGuiTreeNodeFlags_AllowOverlap, "%s", a_label);
		const std::string tip = std::format("{}[Reactions] {}={}{}", Covers(a_key), a_key, t.empty() ? "(nothing)" : t,
			own ? "" : std::format("\n\nNo row of its own: {} decides it now. A change here adds one for this situation only.",
						   from.empty() ? "nothing" : "'" + from + "'"));
		menu::Help(tip);
		SameLine(GetContentRegionAvail().x * 0.35f);
		TextDisabled("%s%s", t.empty() ? "(nothing)" : t.c_str(), own ? "" : "   (shared)");
		menu::Help(tip);
		const char* builtIn{};
		if (own && IsBuiltInRow(a_key, &builtIn) && t != builtIn) {
			SameLine();
			if (SmallButton("built-in")) {
				ResetRow(a_key);
				NoteEdit("Reactions." + a_key, "'" + t + "'", std::format("'{}' (built in)", builtIn));
			}
			menu::Help(std::format("Back to the mod's own: {}", builtIn));
		} else if (own && !IsBuiltInRow(a_key)) {
			SameLine();
			if (SmallButton("shared")) {
				ResetRow(a_key);
				NoteEdit("Reactions." + a_key, "'" + t + "'", "(removed)");
			}
			menu::Help("Remove this situation's own row: the less exact row decides it again");
		}
		if (open) {
			std::string now = t;
			bool        done = false;
			if (Controls(Words(ph->key, burnTime_, shockTime_, channelLinger_, dyingHealth_), now, done)) {
				if (g_dragRow != a_key) {
					g_dragRow = a_key;
					g_dragWas = own ? "'" + t + "'" : "(none)";
				}
				SetRow(a_key, now);
			}
			if (done && g_dragRow == a_key) {
				NoteEdit("Reactions." + a_key, g_dragWas, "'" + now + "'");
				g_dragRow.clear();
			}
			TreePop();
		}
		PopID();
	}

	void Engine::MenuReactions()
	{
		using namespace ImGuiMCP;
		SeparatorText("Reaction table");
		TextWrapped(
			"What each situation does. A blow uses the most exact row there is for it (e.g. arrows, heavy blow before arrows before "
			"any). Open a row to change it; the Help page explains the rest.");
		Checkbox("Edit as text", &g_asText);
		menu::Help("Each row as the ini writes it: a base behaviour, then layers, comma-separated; name:value gives a number. "
				   "For copying rows, or words the controls don't show.");
		const auto rows = Rows();
		const auto text = [&](const std::string& a_key) -> const std::string* {
			const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.first == a_key; });
			return it != rows.end() ? &it->second : nullptr;
		};
		for (const auto& ph : kPhases) {
			PushID(ph.key);
			const bool shown = CollapsingHeader(ph.title, ph.key == std::string_view("stand") ? ImGuiTreeNodeFlags_DefaultOpen : 0);
			menu::Help(ph.help);
			if (!shown) {
				PopID();
				continue;
			}
			const auto words = Words(ph.key, burnTime_, shockTime_, channelLinger_, dyingHealth_);
			for (const auto& [k, t] : rows) {
				if (!InPhase(k, ph)) {
					continue;
				}
				PushID(k.c_str());
				const char* builtIn{};
				const bool  isBuiltIn = IsBuiltInRow(k, &builtIn);
				bool        rowOpen = false;
				if (g_asText) {
					AlignTextToFramePadding();
					TextUnformatted(Situation(k).c_str());
					menu::Help(Covers(k) + k);
					SameLine(GetContentRegionAvail().x * 0.3f);
					SetNextItemWidth(GetContentRegionAvail().x * 0.55f);
					char       buf[256]{};
					const auto it = g_typing.find(k);
					(it != g_typing.end() ? it->second : t).copy(buf, sizeof(buf) - 1);
					if (InputText("##row", buf, sizeof(buf))) {
						g_typing[k] = buf;
					}
					if (IsItemDeactivatedAfterEdit()) {
						g_typing.erase(k);
						if (buf != t) {
							SetRow(k, buf);
							NoteEdit("Reactions." + k, "'" + t + "'", "'" + std::string(buf) + "'");
						}
					} else if (!IsItemActive()) {
						g_typing.erase(k);
					}
					menu::Help(std::format("{}[Reactions] {}={}", Covers(k), k, t));
				} else {
					rowOpen = TreeNodeEx("##row", ImGuiTreeNodeFlags_AllowOverlap, "%s", Situation(k).c_str());
					menu::Help(std::format("{}[Reactions] {}={}", Covers(k), k, t.empty() ? "(nothing)" : t));
					SameLine(GetContentRegionAvail().x * 0.35f);
					TextDisabled("%s", t.empty() ? "(nothing)" : t.c_str());
				}
				if (isBuiltIn && t != builtIn) {
					SameLine();
					if (SmallButton("built-in")) {
						ResetRow(k);
						NoteEdit("Reactions." + k, "'" + t + "'", std::format("'{}' (built in)", builtIn));
					}
					menu::Help(std::format("Back to the mod's own: {}", builtIn));
				} else if (!isBuiltIn) {
					SameLine();
					if (SmallButton("remove")) {
						ResetRow(k);
						NoteEdit("Reactions." + k, "'" + t + "'", "(removed)");
					}
					menu::Help("Remove this row: the next less exact row decides");
				}
				if (const auto bad = UnknownBehaviours(ParseRule(t)); !bad.empty() && g_asText) {
					std::string list;
					for (const auto& b : bad) {
						list += (list.empty() ? "" : ", ") + b;
					}
					TextColored(menu::kBad, "    unknown: %s (ignored)", list.c_str());
				}
				if (rowOpen) {
					// changes apply as they are made; a slider's is logged when it is let go
					std::string now = t;
					bool        done = false;
					if (Controls(words, now, done)) {
						if (g_dragRow != k) {
							g_dragRow = k;
							g_dragWas = t;
						}
						SetRow(k, now);
					}
					if (done && g_dragRow == k) {
						NoteEdit("Reactions." + k, "'" + g_dragWas + "'", "'" + now + "'");
						g_dragRow.clear();
					}
					TreePop();
				}
				PopID();
			}
			// rows the situation could have: the new one starts as the row that decides it now
			SetNextItemWidth(GetContentRegionAvail().x * 0.42f);
			if (BeginCombo("##add", "Add a row for...", ImGuiComboFlags_HeightLarge)) {
				for (const auto& c : Candidates(ph)) {
					if (text(c)) {
						continue;
					}
					if (Selectable(std::format("{}   ({})", Situation(c), c).c_str())) {
						std::string seed;
						for (std::string k = c; !k.empty(); k = k.find('.') == std::string::npos ? "" : k.substr(0, k.rfind('.'))) {
							if (const auto* t = text(k)) {
								seed = *t;
								break;
							}
						}
						if (seed.empty() && First(c) == "die") {
							seed = c.starts_with("die.held") ? std::format("writhe:{}", dyingWrithe_) :
							                                   std::format("shot:{}, push, catch:{}, writhe:{}", dyingHealth_, dyingCatch_, dyingWrithe_);
						}
						SetRow(c, seed);
						NoteEdit("Reactions." + c, "(none)", std::format("'{}' (added)", seed));
					}
				}
				EndCombo();
			}
			menu::Help("A more exact row for one situation, starting as the row that decides it now");
			PopID();
		}
	}
}
