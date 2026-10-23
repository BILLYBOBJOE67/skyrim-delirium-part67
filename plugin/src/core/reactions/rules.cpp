// The reaction table: a row's behaviours and values, and which row a blow picks.
#include "shared.h"

namespace delirium
{
	bool Engine::Rule::Has(std::string_view a_name) const
	{
		return base == a_name || std::any_of(layers.begin(), layers.end(), [&](const auto& l) { return l.first == a_name; });
	}

	float Engine::Rule::Value(std::string_view a_name, float a_or) const
	{
		for (const auto& [n, v] : layers) {
			if (n == a_name && !std::isnan(v)) {
				return v;
			}
		}
		return a_or;
	}

	Engine::Rule Engine::ParseRule(std::string_view a_text)
	{
		// "shot:150, spin, headsnap": the first is the base behaviour, the rest its layers; name:value gives a number
		Rule r;
		r.text = a_text;
		std::size_t i = 0;
		while (i <= a_text.size()) {
			const std::size_t e = std::min(a_text.find(',', i), a_text.size());
			std::string       item(a_text.substr(i, e - i));
			i = e + 1;
			std::erase_if(item, [](unsigned char c) { return std::isspace(c); });
			std::transform(item.begin(), item.end(), item.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (item.empty()) {
				continue;
			}
			float             v = std::numeric_limits<float>::quiet_NaN();
			const std::size_t c = item.find(':');
			if (c != std::string::npos) {
				try {
					v = std::stof(item.substr(c + 1));
				} catch (...) {
				}
				item.resize(c);
			}
			if (r.layers.empty() && r.base.empty()) {
				r.base = item;
			}
			r.layers.emplace_back(item, v);
		}
		return r;
	}

	const Engine::Rule& Engine::PickRule(const char* a_phase, const Blow& a_blow, bool a_lethal, std::string& a_key) const
	{
		// the most exact row there is: phase.type.strength, phase.strength, phase.type, phase
		const char*       type = TypeName(a_blow.type);
		const char*       band = a_lethal ? "lethal" : a_blow.push < flinchBelow_ ? "light" : a_blow.oomph >= heavyOomph_ ? "heavy" : "";
		const std::string p(a_phase);
		std::vector<std::string> tries;
		if (*band) {
			tries.push_back(p + "." + type + "." + band);
			tries.push_back(p + "." + band);
		}
		tries.push_back(p + "." + type);
		tries.push_back(p);
		for (const auto& k : tries) {
			if (auto it = rules_.find(k); it != rules_.end()) {
				a_key = k;
				return it->second;
			}
		}
		static const Rule kStep = ParseRule("step");
		a_key = "(none: step)";
		return kStep;
	}

	const Engine::Rule* Engine::FindRule(std::initializer_list<std::string> a_keys, std::string& a_key) const
	{
		for (const auto& k : a_keys) {
			if (auto it = rules_.find(k); it != rules_.end()) {
				a_key = k;
				return &it->second;
			}
		}
		return nullptr;
	}

	const char* Engine::TypeName(int a_type)
	{
		static constexpr const char* kNames[]{ "other", "melee", "arrow", "spell", "blast", "stagger", "knock", "shout" };
		return kNames[std::clamp(a_type, 0, 7)];
	}

	const char* Engine::ElementName(int a_element)
	{
		static constexpr const char* kNames[]{ "arrow", "frost", "fire", "shock", "magic" };
		return a_element >= 0 && a_element < 5 ? kNames[a_element] : "";
	}

	int Engine::Choose(const Rule& a_rule, std::initializer_list<std::string_view> a_names)
	{
		float total = 0.0f;
		for (const auto& [n, v] : a_rule.layers) {
			if (std::find(a_names.begin(), a_names.end(), n) != a_names.end()) {
				total += std::isnan(v) ? 1.0f : std::max(v, 0.0f);
			}
		}
		if (total <= 0.0f) {
			return -1;
		}
		float pick = Rand(0.0f, 1.0f) * total;
		int   last = -1;
		for (const auto& [n, v] : a_rule.layers) {
			const auto it = std::find(a_names.begin(), a_names.end(), n);
			if (it == a_names.end()) {
				continue;
			}
			const float w = std::isnan(v) ? 1.0f : std::max(v, 0.0f);
			last = static_cast<int>(it - a_names.begin());
			if (w > 0.0f && pick < w) {
				return last;
			}
			pick -= w;
		}
		return last;
	}

	Engine::Rule Engine::ElementRule(int a_element) const
	{
		const char* n = a_element == kFire ? "fire" : a_element == kShock ? "shock" : a_element == kFrost ? "frost" : nullptr;
		if (n) {
			if (auto it = rules_.find(n); it != rules_.end()) {
				return it->second;
			}
		}
		return {};
	}

}
