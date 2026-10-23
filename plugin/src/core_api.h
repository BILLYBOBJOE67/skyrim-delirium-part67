#pragma once

// The line between the host (SkyrimDelirium.dll: SKSE entry, hooks, keys, command file) and the core
// (SkyrimDeliriumCore.dll: the engine). The host loads a copy of the core, so the core can be rebuilt and
// swapped while the game runs ("reload" in the command file). Bump kCoreApi when this struct changes.
namespace delirium
{
	inline constexpr std::uint32_t kCoreApi = 5;

	struct CoreApi
	{
		std::uint32_t version;
		// a_dataLoaded: the game's data is loaded (kDataLoaded passed or a reload while playing): start the engine.
		// The core never calls SKSE::Init: SKSE kills the game when a plugin asks for its load interface after
		// startup. What it needs from SKSE comes from the host.
		bool (*init)(const SKSE::TaskInterface* a_tasks, const wchar_t* a_pluginDir, bool a_dataLoaded);
		void (*dataLoaded)();
		void (*shutdown)();  // main thread: let go of every NPC, so nothing points into the core afterwards
		bool (*busy)();
		void (*afterAnimation)(RE::Actor* a_actor, float a_delta);
		void (*afterPose)(RE::Actor* a_actor, bool a_postPhysics);
		void (*driveRagdoll)(RE::Actor* a_actor, RE::hkbRagdollDriver* a_driver, float a_dt, bool a_after);
		bool (*keepsDown)(RE::Actor* a_actor);
		void (*onKey)(std::uint32_t a_key);
		void (*command)(const char* a_line);  // main thread: one line of the command file
		// Actor::ProcessHitData (AE 38586), before Skyrim's: every weapon hit on an actor (melee, arrows, bashes,
		// explosions), with its point, direction, damage and attacker
		void (*onHitData)(RE::Actor* a_target, const RE::HitData* a_hit);
		// the actor's stagger (AE 37710), after Skyrim's: a_started is whether it really staggered now (perks,
		// blocks and an ongoing stagger can stop it). a_source may be null (Fus, magic stagger, area staggers)
		void (*onStagger)(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source, bool a_started);
		// Character::KillImpl (vtable 0x10E), before Skyrim's: true holds the death back (the core kills her later itself)
		bool (*onKill)(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent, bool a_ragdollInstant);
		// SKSE Menu Framework's render thread: draw one page of the menu (the host registers the pages, see host/main.cpp)
		void (*menu)(int a_page);
		// main thread: SkyrimDelirium.ini's write time changed (the core reads it again unless the change is its own save)
		void (*settingsFileChanged)();
	};

	using GetCoreApi = const CoreApi* (*)();
}
