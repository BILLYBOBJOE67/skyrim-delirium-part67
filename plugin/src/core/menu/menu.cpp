// The in-game menu (SKSE Menu Framework: F1, Mod Control Panel, Skyrim Delirium). One page per settings tab and one
// for presets, each under a bar: undo this session, defaults. The host registers the pages (host/main.cpp) and
// calls Menu on the framework's render thread. An edit applies at once; when the slider is let go it is logged as
// "menu: Section.key old -> new" and kept (SkyrimDelirium.ini is written: nobody needs to open it).
#include "menu/menu.h"
#include "settings/table.h"

#include <cfloat>
#include <climits>

namespace delirium
{
	namespace menu
	{
		namespace
		{
			std::string                           g_msg;
			bool                                  g_msgBad{};
			std::chrono::steady_clock::time_point g_msgAt;

			std::string g_editName, g_editWas;  // the setting being dragged, and its value before
			std::map<std::string, std::string> g_typing;  // text fields being typed in (applied when left)
			std::chrono::steady_clock::time_point g_defaultsAsked;  // "Defaults" needs a second click

			const char* FloatFormat(double a_span)
			{
				return a_span <= 2.0 ? "%.2f" : a_span <= 20.0 ? "%.1f" : "%.0f";
			}

			std::string LimitText(const settings::Setting& a_s)
			{
				const auto& l = a_s.limit;
				if (l.atLeast) {
					return "at least the setting before it";
				}
				const bool lo = std::isfinite(l.lo), hi = std::isfinite(l.hi);
				if (lo && hi) {
					return std::format("{} to {}", l.lo, l.hi);
				}
				return lo ? std::format("{} or more", l.lo) : hi ? std::format("up to {}", l.hi) : "";
			}

			// one setting: its widget, its name (highlighted off the default), a reset button
			void Row(Engine& a_e, const settings::Setting& a_s)
			{
				using namespace ImGuiMCP;
				const std::string name = settings::Name(a_s);
				const std::string was = settings::Text(a_s);
				const std::string def = settings::Text(a_s, settings::Default(a_s));
				const std::string_view key = a_s.key;
				std::string tip = a_s.help;
				tip += std::format("\n\nDefault: {}", def.empty() ? "(empty)" : def);
				if (const auto l = LimitText(a_s); !l.empty()) {
					tip += std::format("    Limits: {}", l);
				}
				if (a_s.flags & settings::kRestart) {
					tip += "\nTakes effect on the next game start.";
				}
				if (!(a_s.flags & settings::kPreset)) {
					tip += "\nNot part of presets.";
				}
				const char* capLabel = nullptr;  // the setting that caps this one
				if (a_s.cappedBy) {
					for (const auto& r : a_e.SettingsTable()) {
						if (const auto* p = std::get_if<float*>(&r.value); p && *p == a_s.cappedBy) {
							capLabel = r.label;
						}
					}
					tip += std::format("\nThe push it gives stops at '{}' ({}): past it, raise that too.", capLabel ? capLabel : "its cap", *a_s.cappedBy);
				}

				PushID(name.c_str());
				SetNextItemWidth(GetContentRegionAvail().x * 0.42f);
				settings::Any v = settings::Get(a_s);
				bool          changed = false, done = false;
				double        lo = a_s.limit.lo, hi = a_s.limit.hi;
				if (a_s.limit.atLeast) {
					lo = *a_s.limit.atLeast + a_s.limit.gap;
				}
				std::visit(
					[&](auto* a_p) {
						using T = std::remove_pointer_t<decltype(a_p)>;
						if constexpr (std::is_same_v<T, bool>) {
							bool b = *a_p;
							if (Checkbox("##v", &b)) {
								v = b;
								changed = done = true;
							}
						} else if constexpr (std::is_same_v<T, float>) {
							float f = *a_p;
							if (std::isfinite(lo) && std::isfinite(hi)) {
								if (key.find("Chance") != std::string_view::npos) {
									float pc = f * 100.0f;
									if (SliderFloat("##v", &pc, static_cast<float>(lo * 100.0), static_cast<float>(hi * 100.0), "%.0f %%", ImGuiSliderFlags_AlwaysClamp)) {
										f = pc / 100.0f;
										changed = true;
									}
								} else if (SliderFloat("##v", &f, static_cast<float>(lo), static_cast<float>(hi), FloatFormat(hi - lo), ImGuiSliderFlags_AlwaysClamp)) {
									changed = true;
								}
							} else {
								const float speed = static_cast<float>(std::max(std::abs(a_s.def), 1.0) * 0.005);
								if (DragFloat("##v", &f, speed, std::isfinite(lo) ? static_cast<float>(lo) : -FLT_MAX, std::isfinite(hi) ? static_cast<float>(hi) : FLT_MAX,
										std::abs(a_s.def) >= 20.0 ? "%.0f" : "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
									changed = true;
								}
							}
							v = f;
							done = IsItemDeactivatedAfterEdit();
						} else if constexpr (std::is_same_v<T, std::string>) {
							char buf[512]{};
							const auto it = g_typing.find(name);
							const std::string& shown = it != g_typing.end() ? it->second : *a_p;
							shown.copy(buf, sizeof(buf) - 1);
							if (InputText("##v", buf, sizeof(buf))) {
								g_typing[name] = buf;
							}
							if (IsItemDeactivatedAfterEdit()) {
								v = std::string(buf);
								changed = done = true;
								g_typing.erase(name);
							} else if (!IsItemActive()) {
								g_typing.erase(name);
							}
						} else {
							int i = static_cast<int>(*a_p);
							const int ilo = std::isfinite(lo) ? static_cast<int>(lo) : INT_MIN / 2;
							const int ihi = std::isfinite(hi) ? static_cast<int>(hi) : INT_MAX / 2;
							if (key.ends_with("Key")) {
								changed = InputScalar("##v", ImGuiDataType_S32, &i, nullptr, nullptr, "%X", ImGuiInputTextFlags_CharsHexadecimal);
							} else if (std::isfinite(lo) && std::isfinite(hi)) {
								changed = SliderInt("##v", &i, ilo, ihi, "%d", ImGuiSliderFlags_AlwaysClamp);
							} else {
								changed = DragInt("##v", &i, 0.2f, ilo, ihi, "%d", ImGuiSliderFlags_AlwaysClamp);
							}
							v = static_cast<long long>(i);
							done = IsItemDeactivatedAfterEdit();
						}
					},
					a_s.value);
				Help(tip);
				if (changed) {
					if (g_editName != name) {
						g_editName = name;
						g_editWas = was;
					}
					a_e.SetSetting(a_s, v);
				}
				if (done && g_editName == name) {
					a_e.NoteEdit(name, g_editWas, settings::Text(a_s));
					g_editName.clear();
				}
				SameLine();
				const bool off = settings::Text(a_s) != def;
				if (off) {
					TextColored(kChanged, "%s", a_s.label);
				} else {
					TextUnformatted(a_s.label);
				}
				Help(tip);
				if (a_s.flags & settings::kRestart) {
					SameLine();
					TextDisabled("(next start)");
				}
				if (const auto* f = std::get_if<float*>(&a_s.value); a_s.cappedBy && f && **f > *a_s.cappedBy) {
					SameLine();
					TextColored(kBad, "capped at %.0f by '%s'", *a_s.cappedBy, capLabel ? capLabel : "its cap");
					Help("Past this, more does nothing: the push stops at that setting. Raise it too (same page).");
				}
				if (off) {
					SameLine();
					if (SmallButton("default")) {
						const auto before = settings::Text(a_s);
						a_e.SetSetting(a_s, settings::Default(a_s));
						a_e.NoteEdit(name, before, settings::Text(a_s));
					}
					Help(std::format("Back to {}", def.empty() ? "(empty)" : def));
				}
				PopID();
			}

			// a button that asks for a second click before it acts
			bool Confirmed(const char* a_label, const char* a_again, std::chrono::steady_clock::time_point& a_asked)
			{
				const auto now = std::chrono::steady_clock::now();
				const bool asking = now - a_asked < std::chrono::seconds(4);
				if (!ImGuiMCP::Button(asking ? a_again : a_label)) {
					return false;
				}
				a_asked = asking ? std::chrono::steady_clock::time_point{} : now;
				return asking;
			}

			void Bar(Engine& a_e)
			{
				using namespace ImGuiMCP;
				static std::chrono::steady_clock::time_point undoAsked;
				if (Confirmed("Undo this session", "Click again: undo", undoAsked)) {
					a_e.RevertSession();
					Say("back to the settings the game started with");
				}
				Help("Every change since the game started, undone");
				SameLine();
				if (Confirmed("Defaults", "Click again: all defaults", g_defaultsAsked)) {
					a_e.DefaultSettings("the defaults (menu)");
					Say("every setting and reaction row at the mod's default");
				}
				Help("Every setting that presets hold, and the reaction table, back to the mod's defaults. Files, keys and collision stay.");
				SameLine();
				if (!a_e.SaveError().empty()) {
					TextColored(kBad, "couldn't keep the changes: %s", a_e.SaveError().c_str());
				} else {
					TextDisabled("changes apply at once and are kept");
				}
				if (!g_msg.empty() && std::chrono::steady_clock::now() - g_msgAt < std::chrono::seconds(8)) {
					TextColored(g_msgBad ? kBad : kGood, "%s", g_msg.c_str());
				}
				Separator();
			}

			// a min / max pair as one range: the low end's row (rangeLabel) and the row after it (at least the low end)
			void RangeRow(Engine& a_e, const settings::Setting& a_lo, const settings::Setting& a_hi)
			{
				using namespace ImGuiMCP;
				static std::string wasLo, wasHi, editing;  // the pair being dragged, and its values before
				const std::string  nameLo = settings::Name(a_lo), nameHi = settings::Name(a_hi);
				float*             lo = std::get<float*>(a_lo.value);
				float*             hi = std::get<float*>(a_hi.value);
				const std::string  defLo = settings::Text(a_lo, settings::Default(a_lo)), defHi = settings::Text(a_hi, settings::Default(a_hi));
				const std::string  tip = std::format("{}\n{}\n\nDefault: {} to {}", a_lo.help, a_hi.help, defLo, defHi);
				PushID(nameLo.c_str());
				SetNextItemWidth(GetContentRegionAvail().x * 0.42f);
				float       a = *lo, b = *hi;
				const float span = static_cast<float>(std::max({ std::abs(a_lo.def), std::abs(a_hi.def), 1.0 }));
				const float min = std::isfinite(a_lo.limit.lo) ? static_cast<float>(a_lo.limit.lo) : -FLT_MAX;
				const float max = std::isfinite(a_hi.limit.hi) ? static_cast<float>(a_hi.limit.hi) : FLT_MAX;
				const char* fmt = span >= 20.0f ? "%.0f" : "%.2f";
				if (DragFloatRange2("##r", &a, &b, span * 0.005f, min, max, fmt, fmt, ImGuiSliderFlags_AlwaysClamp)) {
					if (editing != nameLo) {
						editing = nameLo;
						wasLo = settings::Text(a_lo);
						wasHi = settings::Text(a_hi);
					}
					a_e.SetSetting(a_lo, a);  // the low end first: the high end is limited against it
					a_e.SetSetting(a_hi, b);
				}
				Help(tip);
				if (IsItemDeactivatedAfterEdit() && editing == nameLo) {
					a_e.NoteEdit(nameLo, wasLo, settings::Text(a_lo));
					a_e.NoteEdit(nameHi, wasHi, settings::Text(a_hi));
					editing.clear();
				}
				SameLine();
				const bool off = settings::Text(a_lo) != defLo || settings::Text(a_hi) != defHi;
				if (off) {
					TextColored(kChanged, "%s", a_lo.rangeLabel);
				} else {
					TextUnformatted(a_lo.rangeLabel);
				}
				Help(tip);
				if (off) {
					SameLine();
					if (SmallButton("default")) {
						const auto bl = settings::Text(a_lo), bh = settings::Text(a_hi);
						a_e.SetSetting(a_lo, settings::Default(a_lo));
						a_e.SetSetting(a_hi, settings::Default(a_hi));
						a_e.NoteEdit(nameLo, bl, settings::Text(a_lo));
						a_e.NoteEdit(nameHi, bh, settings::Text(a_hi));
					}
					Help(std::format("Back to {} to {}", defLo, defHi));
				}
				PopID();
			}

			// rows under section titles; a pair (rangeLabel, then the row at least it) as one range
			void Rows(Engine& a_e, const std::vector<const settings::Setting*>& a_rows)
			{
				const char* section = nullptr;
				for (std::size_t i = 0; i < a_rows.size(); ++i) {
					const auto& s = *a_rows[i];
					if (!section || std::strcmp(section, s.section) != 0) {
						section = s.section;
						ImGuiMCP::SeparatorText(section);
					}
					const auto* f = std::get_if<float*>(&s.value);
					if (s.rangeLabel && f && i + 1 < a_rows.size() && a_rows[i + 1]->limit.atLeast == *f && std::holds_alternative<float*>(a_rows[i + 1]->value)) {
						RangeRow(a_e, s, *a_rows[i + 1]);
						++i;
						continue;
					}
					Row(a_e, s);
				}
			}

			void Settings(Engine& a_e, settings::Tab a_tab)
			{
				const auto                           table = a_e.SettingsTable();
				std::vector<const settings::Setting*> main, detail;
				for (const auto& s : table) {
					if (s.tab == a_tab && !(s.flags & settings::kHidden)) {
						((s.flags & settings::kDetail) ? detail : main).push_back(&s);
					}
				}
				Rows(a_e, main);
				if (!detail.empty()) {
					ImGuiMCP::Spacing();
					const bool open = ImGuiMCP::CollapsingHeader(std::format("Fine tuning ({})###fine", detail.size()).c_str());
					Help("The rest of this page's settings: the finer points of how hard, how far and when. The defaults are tuned; "
						 "a changed one shows its name highlighted.");
					if (open) {
						Rows(a_e, detail);
					}
				}
			}

			void Guide(Engine& a_e)
			{
				using namespace ImGuiMCP;
				SeparatorText("How a hit brings her into Delirium");
				TextWrapped(
					"Arrows, blows, staggers, explosions and shouts always do. A spell's missile does by chance (Difficulty: "
					"'Spell: chance at full health' and 'near death', in between by her health), and a channelled spell (flames, "
					"sparks, frostbite) by a chance per second. Once she is in, every hit plays until she is let go.");
				SeparatorText("What she does");
				TextWrapped(
					"The Reactions page decides it, one row per situation: standing, an element, thrown, hit while down, on the "
					"ground, dying. Open a row to pick its behaviour and the layers on top of it. Its words are below.");
				SeparatorText("How hard");
				TextWrapped(
					"Strength sets each push (arrow, stagger, blow). Three caps keep them in bounds: 'Hit push at most' for "
					"arrows and spells, 'Blow push at most' for melee, 'Stagger push at most' for staggers. A push set past "
					"its cap shows a warning, because the cap wins.");
				SeparatorText("The menu");
				BulletText("Every change applies at once and is saved to SkyrimDelirium.ini.");
				BulletText("'Fine tuning' on each tab holds the settings you rarely need. A range slider sets two values: drag either end.");
				BulletText("Presets saves what differs from the defaults under a name and loads it again. 'Defaults' resets everything.");
				BulletText("Hover a setting for what it does. A setting marked '(next start)' applies at the next game start.");
				BulletText("SkyrimDeliriumCore.log (Documents\\My Games\\Skyrim Special Edition\\SKSE) lists every hit, chance roll "
						   "and menu change.");
				SeparatorText("The reaction table");
				a_e.MenuReactionsHelp();
			}

			void Presets(Engine& a_e)
			{
				using namespace ImGuiMCP;
				static std::vector<std::string>              names;
				static std::chrono::steady_clock::time_point listed;
				static char                                  newName[64]{};
				const auto                                   now = std::chrono::steady_clock::now();
				if (now - listed > std::chrono::seconds(2)) {
					names = Engine::PresetNames();
					listed = now;
				}
				TextWrapped(
					"A preset holds how Delirium plays: which reactions, how hard, how often (the chances that bring an NPC into "
					"Delirium), how long, and the reaction table. Not the files, keys or collision set-up. It keeps only what differs "
					"from the defaults, so it is short and can be shared: the files are in Data\\SKSE\\Plugins\\SkyrimDelirium\\Presets.");
				TextWrapped("Loading one sets every preset setting to its default, then to the preset's values. It stays loaded in the next game.");
				SeparatorText("Presets");
				if (names.empty()) {
					TextDisabled("none yet");
				}
				for (const auto& n : names) {
					PushID(n.c_str());
					if (SmallButton("Load")) {
						std::string m;
						const bool  ok = a_e.LoadPreset(n, m);
						Say(m, !ok);
					}
					SameLine();
					TextUnformatted(n.c_str());
					PopID();
				}
				SeparatorText("Save the settings as a preset");
				SetNextItemWidth(GetContentRegionAvail().x * 0.42f);
				InputText("##name", newName, sizeof(newName));
				SameLine();
				const std::string name = newName;
				const bool        exists = std::find(names.begin(), names.end(), name) != names.end();
				BeginDisabled(name.find_first_not_of(' ') == std::string::npos);
				if (Button(exists ? "Replace" : "Save")) {
					std::string m;
					const bool  ok = a_e.SavePreset(name, m);
					Say(m, !ok);
					listed = {};
				}
				EndDisabled();
				if (exists) {
					SameLine();
					TextColored(kChanged, "replaces the preset of that name");
				}
			}
		}

		void Say(std::string a_msg, bool a_error)
		{
			g_msg = std::move(a_msg);
			g_msgBad = a_error;
			g_msgAt = std::chrono::steady_clock::now();
		}

		void Help(const std::string& a_text)
		{
			using namespace ImGuiMCP;
			if (IsItemHovered(ImGuiHoveredFlags_ForTooltip) && BeginTooltip()) {
				PushTextWrapPos(GetFontSize() * 32.0f);
				TextUnformatted(a_text.c_str());
				PopTextWrapPos();
				EndTooltip();
			}
		}
	}

	void Engine::Menu(int a_page)
	{
		using enum settings::Tab;
		menu::Bar(*this);
		switch (a_page) {
		case 0:
			menu::Settings(*this, Reactions);
			MenuReactions();
			break;
		case 1:
			menu::Settings(*this, Strength);
			break;
		case 2:
			menu::Settings(*this, Difficulty);
			break;
		case 3:
			menu::Settings(*this, Timing);
			break;
		case 4:
			menu::Settings(*this, Advanced);
			break;
		case 5:
			menu::Presets(*this);
			break;
		case 6:
			menu::Guide(*this);
			break;
		default:
			break;
		}
	}
}
