#pragma once

// The line between the host (SkyrimDelirium.dll: SKSE entry, hooks, keys, command file) and the core
// (SkyrimDeliriumCore.dll: the engine). The host loads a copy of the core, so the core can be rebuilt and
// swapped while the game runs ("reload" in the command file). Bump kCoreApi when this struct changes.
namespace delirium
{
	inline constexpr std::uint32_t kCoreApi = 2;

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
	};

	using GetCoreApi = const CoreApi* (*)();
}
