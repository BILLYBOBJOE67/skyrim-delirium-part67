#include "core_api.h"
#include <spdlog/sinks/basic_file_sink.h>
#include "engine.h"

namespace delirium
{
	std::filesystem::path       g_pluginDir;  // Data/SKSE/Plugins (the core itself runs from a temporary copy)
	const SKSE::TaskInterface* g_tasks{};     // the host's: main-thread tasks
}

namespace
{
	using delirium::Engine;

	bool Init(const SKSE::TaskInterface* a_tasks, const wchar_t* a_pluginDir, bool a_dataLoaded)
	{
		// our own log (fresh on every load): SKSE::Init is not allowed here, see core_api.h
		if (auto dir = SKSE::log::log_directory()) {
			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "SkyrimDeliriumCore.log").string(), true);
			auto log = std::make_shared<spdlog::logger>("core", std::move(sink));
			log->set_level(spdlog::level::info);
			log->flush_on(spdlog::level::info);
			spdlog::set_default_logger(std::move(log));
			spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v");
		}
		delirium::g_tasks = a_tasks;
		delirium::g_pluginDir = a_pluginDir;
		logger::info("core loaded (build {} {})", __DATE__, __TIME__);
		Engine::Get().LoadSettings();
		return !a_dataLoaded || Engine::Get().Init();
	}
	void DataLoaded()
	{
		if (!Engine::Get().Init()) {
			logger::error("not started (see above)");
		}
	}
	void Shutdown() { Engine::Get().Shutdown(); }
	bool Busy() { return Engine::Busy(); }
	void AfterAnimation(RE::Actor* a, float d) { Engine::Get().AfterAnimation(a, d); }
	void AfterPose(RE::Actor* a, bool p) { Engine::Get().AfterPose(a, p); }
	void DriveRagdoll(RE::Actor* a, RE::hkbRagdollDriver* d, float dt, bool after) { Engine::Get().DriveRagdoll(a, d, dt, after); }
	bool KeepsDown(RE::Actor* a) { return Engine::Get().KeepsDown(a); }
	void OnKey(std::uint32_t k) { Engine::Get().OnKey(k); }
	void Command(const char* l) { Engine::Get().Command(l); }
	void OnHitData(RE::Actor* t, const RE::HitData* h) { Engine::Get().OnHitData(t, *h); }
	void OnStagger(RE::Actor* t, float m, RE::Actor* s, bool st) { Engine::Get().OnStagger(t, m, s, st); }
	bool OnKill(RE::Actor* a, RE::Actor* k, float d, bool e, bool r) { return Engine::Get().OnKill(a, k, d, e, r); }

	constexpr delirium::CoreApi kApi{ delirium::kCoreApi, Init, DataLoaded, Shutdown, Busy, AfterAnimation, AfterPose, DriveRagdoll, KeepsDown,
		OnKey, Command, OnHitData, OnStagger, OnKill };
}

extern "C" __declspec(dllexport) const delirium::CoreApi* DeliriumCore_Get() { return &kApi; }
