#include "core_api.h"

#include <fstream>
#include <thread>

namespace
{
	// ---- the core (SkyrimDeliriumCore.dll): loaded from a temporary copy so it can be rebuilt and swapped in game.
	// Hooks reach it through Use, which holds a shared lock; a reload takes it exclusively, so no hook is inside the
	// old core while it shuts down. Nested hooks on one thread (Skyrim calling a hooked function from inside the
	// core) don't lock again.
	namespace core
	{
		SRWLOCK                          lock = SRWLOCK_INIT;
		const delirium::CoreApi*         api = nullptr;
		HMODULE                          mod = nullptr;
		thread_local int                 depth = 0;
		std::filesystem::path            pluginDir;
		bool                             dataLoaded = false;
		int                              generation = 0;

		struct Use
		{
			const delirium::CoreApi* c;
			bool                     locked;
			Use() : locked(depth++ == 0)
			{
				if (locked) {
					AcquireSRWLockShared(&lock);
				}
				c = api;
			}
			~Use()
			{
				if (locked) {
					ReleaseSRWLockShared(&lock);
				}
				--depth;
			}
			Use(const Use&) = delete;
			Use&                     operator=(const Use&) = delete;
			explicit                 operator bool() const { return c != nullptr; }
			const delirium::CoreApi* operator->() const { return c; }
		};

		bool Load()
		{
			const auto      source = pluginDir / L"SkyrimDelirium" / L"SkyrimDeliriumCore.dll";
			wchar_t         tmp[MAX_PATH]{};
			GetTempPathW(MAX_PATH, tmp);
			const auto      dir = std::filesystem::path(tmp) / L"SkyrimDelirium";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				std::filesystem::remove_all(e.path(), ec);  // old copies (the loaded one stays: it is locked)
			}
			// one folder per load, with the PDB beside it, so a debugger's lock on the last copy never blocks the next
			const auto folder = dir / std::format(L"{}_{}", GetCurrentProcessId(), ++generation);
			std::filesystem::create_directories(folder, ec);
			const auto copy = folder / L"SkyrimDeliriumCore.dll";
			if (!CopyFileW(source.c_str(), copy.c_str(), FALSE)) {
				logger::error("core: can't copy {} (error {})", source.string(), GetLastError());
				return false;
			}
			auto pdb = source;
			CopyFileW(pdb.replace_extension(L".pdb").c_str(), (folder / L"SkyrimDeliriumCore.pdb").c_str(), FALSE);
			const HMODULE m = LoadLibraryW(copy.c_str());
			const auto    get = m ? reinterpret_cast<delirium::GetCoreApi>(GetProcAddress(m, "DeliriumCore_Get")) : nullptr;
			const auto*   a = get ? get() : nullptr;
			if (!a || a->version != delirium::kCoreApi) {
				logger::error("core: {} is not a core for this host (error {}, api {})", copy.string(), GetLastError(), a ? a->version : 0);
				if (m) {
					FreeLibrary(m);
				}
				return false;
			}
			if (!a->init(SKSE::GetTaskInterface(), pluginDir.c_str(), dataLoaded)) {
				logger::error("core: not started (see SkyrimDeliriumCore.log)");
			}
			AcquireSRWLockExclusive(&lock);
			api = a;
			mod = m;
			ReleaseSRWLockExclusive(&lock);
			logger::info("core {} loaded", generation);
			return true;
		}

		// main thread. The old core lets go of every NPC, then is freed two task rounds later: tasks it queued
		// itself run first.
		void Reload()
		{
			AcquireSRWLockExclusive(&lock);
			const auto* old = api;
			const auto  oldMod = mod;
			api = nullptr;
			mod = nullptr;
			ReleaseSRWLockExclusive(&lock);
			if (old) {
				old->shutdown();
			}
			logger::info("core: reloading");
			SKSE::GetTaskInterface()->AddTask([oldMod]() {
				SKSE::GetTaskInterface()->AddTask([oldMod]() {
					if (oldMod) {
						FreeLibrary(oldMod);
					}
					Load();
				});
			});
		}

		// Data/../My Games/.../SKSE/SkyrimDelirium.cmd: lines for the core (see Engine::Command), or "reload".
		// Tools write it under another name and rename it, so it is never read half written.
		void Watch()
		{
			const auto path = *SKSE::log::log_directory() / "SkyrimDelirium.cmd";
			for (;;) {
				Sleep(100);
				std::vector<std::string> lines;
				{
					std::ifstream in(path);
					if (!in) {
						continue;
					}
					for (std::string l; std::getline(in, l);) {
						while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) {
							l.pop_back();
						}
						if (!l.empty()) {
							lines.push_back(l);
						}
					}
				}
				std::error_code ec;
				std::filesystem::remove(path, ec);
				SKSE::GetTaskInterface()->AddTask([lines]() {
					for (const auto& l : lines) {
						if (l == "reload") {
							Reload();
							return;  // the rest waits for the next file: the new core isn't loaded yet
						}
						Use u;
						if (u) {
							u->command(l.c_str());
						}
					}
				});
			}
		}
	}

	// NPC animation runs in per-actor jobs (RunOneActorAnimationUpdateJob, AE id 41453) that call
	// Actor::UpdateAnimationInternal directly, not through the vtable. We hook that call: Skyrim poses the
	// actor, then Delirium steps and overwrites the bones it drives. (The vtable slot 0x7D is never called for NPCs.)
	struct UpdateAnimationInternal
	{
		static void thunk(RE::Actor* a_this, float a_delta)
		{
			func(a_this, a_delta);
			calls.fetch_add(1, std::memory_order_relaxed);
			if (core::Use c; c) {
				c->afterAnimation(a_this, a_delta);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static inline std::atomic<std::uint64_t>      calls{ 0 };
		static inline bool                             installed{ false };

		static void Install()
		{
			constexpr std::size_t           kOffset = 0x74;
			REL::Relocation<std::uintptr_t> job{ REL::ID(41453) };
			const auto                      site = job.address() + kOffset;
			const auto                      base = REL::Module::get().base();
			const auto*                     op = reinterpret_cast<const std::uint8_t*>(site);
			if (*op != 0xE8) {
				logger::error("animation hook: +{:X} is not a call (byte {:02X}); not hooked", site - base, *op);
				return;
			}
			const auto target = site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1);
			func = SKSE::GetTrampoline().write_call<5>(site, thunk);
			installed = true;
			logger::info("animation hook at +{:X} (calls +{:X})", site - base, target - base);
			// every other call to the same update: a path we don't hook would show the plain animation on its frames
			const auto text = REL::Module::get().segment(REL::Segment::textx);
			const auto* b = reinterpret_cast<const std::uint8_t*>(text.address());
			int         more = 0;
			for (std::size_t i = 0; i + 5 <= text.size(); ++i) {
				if (b[i] != 0xE8 && b[i] != 0xE9) {
					continue;
				}
				const auto at = text.address() + i;
				if (at == site || at + 5 + *reinterpret_cast<const std::int32_t*>(b + i + 1) != target) {
					continue;
				}
				++more;
				if (b[i] == 0xE8) {
					SKSE::GetTrampoline().write_call<5>(at, thunk);
					logger::info("animation hook: also hooked the call at +{:X}", at - base);
				} else {
					logger::info("animation hook: jump to it at +{:X} (not hooked)", at - base);
				}
			}
			logger::info("animation hook: {} other call sites", more);
		}
	};

	// Skyrim's per-bone copy of the animation graph's pose into the skeleton nodes (AE id 63856). Two callers:
	// the normal graph update (id 63568) and the post-physics one used while ragdolled (id 63563). Nothing else
	// writes the bone locals, so Delirium's bones go on right after every copy and the plain pose is never drawn.
	struct PoseToNodes
	{
		static void Run(const void* a_poseLocal, void* a_boneNodes, std::uint16_t a_count, bool a_postPhysics)
		{
			func(a_poseLocal, a_boneNodes, a_count);
			if (core::Use c; c && c->busy()) {
				// a_boneNodes is BShkbAnimationGraph::boneNodes (0x160); the graph's holder (0x210) is the actor
				auto* graph = reinterpret_cast<RE::BShkbAnimationGraph*>(static_cast<std::uint8_t*>(a_boneNodes) - 0x160);
				if (graph->holder) {
					c->afterPose(graph->holder, a_postPhysics);
				}
			}
		}
		static void thunk(const void* a_p, void* a_n, std::uint16_t a_c) { Run(a_p, a_n, a_c, false); }
		static void thunkPost(const void* a_p, void* a_n, std::uint16_t a_c) { Run(a_p, a_n, a_c, true); }
		static inline REL::Relocation<decltype(thunk)> func;

		static void Install()
		{
			const auto base = REL::Module::get().base();
			const auto copy = REL::Relocation<std::uintptr_t>{ REL::ID(63856) }.address();
			const std::tuple<REL::ID, std::size_t, void (*)(const void*, void*, std::uint16_t)> sites[]{
				{ REL::ID(63568), 0x382, &thunk }, { REL::ID(63563), 0x285, &thunkPost }
			};
			for (const auto& [id, off, fn] : sites) {
				const auto site = REL::Relocation<std::uintptr_t>{ id }.address() + off;
				const auto* op = reinterpret_cast<const std::uint8_t*>(site);
				if (*op != 0xE8 || site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1) != copy) {
					logger::error("pose hook: +{:X} is not the call to the bone copy +{:X}; not hooked", site - base, copy - base);
					continue;
				}
				func = SKSE::GetTrampoline().write_call<5>(site, fn);
				logger::info("pose hook at +{:X}", site - base);
			}
		}
	};

	// hkbRagdollDriver::driveToPose (call at AE id 63562 + 0x256): Skyrim drives its ragdoll's rigid bodies before the
	// physics step. While Delirium owns a ragdolled NPC we steer those bodies onto Delirium's pose here, so Skyrim's
	// own ragdoll (and with it the actor's position, crosshair, hits and get-up) follows our body. Precision hooks the
	// same call; write_call chains through it.
	struct DriveToPose
	{
		static void thunk(RE::hkbRagdollDriver* a_driver, float a_dt, const void* a_context, void* a_output)
		{
			core::Use c;
			if (c && c->busy() && a_driver && a_driver->character && a_driver->character->behaviorGraph) {
				if (auto* graph = reinterpret_cast<RE::BShkbAnimationGraph*>(a_driver->character->behaviorGraph->userData); graph && graph->holder) {
					auto* holder = graph->holder;
					c->driveRagdoll(holder, a_driver, a_dt, false);
					func(a_driver, a_dt, a_context, a_output);
					// Skyrim's driver may set its own velocities: ours go back on top
					c->driveRagdoll(holder, a_driver, a_dt, true);
					return;
				}
			}
			func(a_driver, a_dt, a_context, a_output);
		}
		static inline REL::Relocation<decltype(thunk)> func;

		static void Install()
		{
			const auto base = REL::Module::get().base();
			const auto site = REL::Relocation<std::uintptr_t>{ REL::ID(63562) }.address() + 0x256;
			if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
				logger::error("ragdoll hook: +{:X} is not a call; not hooked", site - base);
				return;
			}
			func = SKSE::GetTrampoline().write_call<5>(site, thunk);
			logger::info("ragdoll hook at +{:X} (was calling +{:X})", site - base, func.address() - base);
		}
	};

	// Skyrim's knock-state update (AE 39896) asks Actor::IsRagdollMovingSlowEnoughToGetUp (AE 37689) whether a
	// knocked-down actor may get up. While Delirium still moves the body the answer is no; once it has settled
	// Skyrim's own check decides and its vanilla get-up starts from our pose.
	struct GetUpCheck
	{
		static bool thunk(RE::Actor* a_actor)
		{
			if (core::Use c; c && c->busy() && a_actor && c->keepsDown(a_actor)) {
				return false;
			}
			return func(a_actor);
		}
		static inline REL::Relocation<decltype(thunk)> func;

		static void Install()
		{
			const auto base = REL::Module::get().base();
			const auto site = REL::Relocation<std::uintptr_t>{ REL::ID(39896) }.address() + 0x2B6;
			const auto want = REL::Relocation<std::uintptr_t>{ REL::ID(37689) }.address();
			const auto* p = reinterpret_cast<const std::uint8_t*>(site);
			if (p[0] != 0xE8 || site + 5 + *reinterpret_cast<const std::int32_t*>(p + 1) != want) {
				logger::error("get-up hook: +{:X} is not the call to IsRagdollMovingSlowEnoughToGetUp; not hooked", site - base);
				return;
			}
			func = SKSE::GetTrampoline().write_call<5>(site, thunk);
			logger::info("get-up hook at +{:X}", site - base);
		}
	};

	// A hook at a function's first bytes, for functions with many callers: its opening instructions (a_expect, whole
	// instructions with nothing position dependent) are copied into the trampoline followed by a jump back past them,
	// and the function's start jumps to a_thunk. A start that is already a jump (another mod hooked it first) is
	// chained instead. Returns what calls the original, 0 if the bytes were not what we know (nothing is patched).
	std::uintptr_t HookEntry(std::uintptr_t a_fn, std::span<const std::uint8_t> a_expect, void* a_thunk, const char* a_name)
	{
		const auto  base = REL::Module::get().base();
		const auto* p = reinterpret_cast<const std::uint8_t*>(a_fn);
		auto&       t = SKSE::GetTrampoline();
		auto        absJump = [](std::uint8_t* at, std::uintptr_t to) {
			const std::uint8_t op[6]{ 0xFF, 0x25, 0, 0, 0, 0 };  // jmp [rip+0]
			std::memcpy(at, op, 6);
			std::memcpy(at + 6, &to, 8);
		};
		std::uintptr_t orig = 0;
		if (p[0] == 0xE9) {
			const auto target = a_fn + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
			auto*      stub = static_cast<std::uint8_t*>(t.allocate(14));
			absJump(stub, target);
			orig = reinterpret_cast<std::uintptr_t>(stub);
			logger::info("{} hook: +{:X} already jumps to +{:X} (another mod): chained", a_name, a_fn - base, target - base);
		} else if (std::memcmp(p, a_expect.data(), a_expect.size()) == 0) {
			auto* stub = static_cast<std::uint8_t*>(t.allocate(a_expect.size() + 14));
			std::memcpy(stub, a_expect.data(), a_expect.size());
			absJump(stub + a_expect.size(), a_fn + a_expect.size());
			orig = reinterpret_cast<std::uintptr_t>(stub);
		} else {
			logger::error("{} hook: +{:X} starts with unknown bytes ({:02X} {:02X} {:02X} {:02X}); not hooked", a_name, a_fn - base, p[0], p[1], p[2], p[3]);
			return 0;
		}
		t.write_branch<5>(a_fn, reinterpret_cast<std::uintptr_t>(a_thunk));
		logger::info("{} hook at +{:X}", a_name, a_fn - base);
		return orig;
	}

	// Actor::ProcessHitData (AE 38586): every weapon hit an actor takes (melee, arrows, bashes, explosions) with its
	// HitData. The core records it first (point, direction, damage, attacker): Skyrim's stagger is called from inside.
	struct ProcessHitData
	{
		static std::uintptr_t thunk(RE::Actor* a_target, RE::HitData* a_hit)
		{
			if (core::Use c; c && a_target && a_hit) {
				c->onHitData(a_target, a_hit);
			}
			return func(a_target, a_hit);  // whatever it returns passes through
		}
		static inline std::uintptr_t (*func)(RE::Actor*, RE::HitData*){};

		static void Install()
		{
			// mov rax, rsp; push rbp; push rsi
			static constexpr std::uint8_t kStart[]{ 0x48, 0x8B, 0xC4, 0x55, 0x56 };
			func = reinterpret_cast<decltype(func)>(
				HookEntry(REL::Relocation<std::uintptr_t>{ REL::ID(38586) }.address(), kStart, reinterpret_cast<void*>(&thunk), "hit"));
		}
	};

	// The actor's stagger (AE 37710; the hit's stagger, Fus, magic stagger effects, area staggers, a knock that can't
	// ragdoll): Skyrim's runs as always (it stops what she was doing), Delirium then plays it on the body.
	struct Stagger
	{
		static bool thunk(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source)
		{
			const bool was = a_target && a_target->AsActorState()->IsStaggered();
			const bool r = func(a_target, a_magnitude, a_source);
			if (core::Use c; c && a_target) {
				c->onStagger(a_target, a_magnitude, a_source, !was && a_target->AsActorState()->IsStaggered());
			}
			return r;
		}
		static inline bool (*func)(RE::Actor*, float, RE::Actor*){};

		static void Install()
		{
			// mov rax, rsp; push rdi; sub rsp, 0C0h
			static constexpr std::uint8_t kStart[]{ 0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00 };
			func = reinterpret_cast<decltype(func)>(
				HookEntry(REL::Relocation<std::uintptr_t>{ REL::ID(37710) }.address(), kStart, reinterpret_cast<void*>(&thunk), "stagger"));
		}
	};

	// An NPC's death (Character::KillImpl, vtable 0x10E; the player's is PlayerCharacter's, not hooked): the core may
	// hold it back a few seconds, so she dies on her feet in Delirium first, then calls it again itself
	struct Kill
	{
		static void thunk(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage, bool a_sendEvent, bool a_ragdollInstant)
		{
			if (core::Use c; c && a_this && c->onKill(a_this, a_attacker, a_damage, a_sendEvent, a_ragdollInstant)) {
				return;
			}
			func(a_this, a_attacker, a_damage, a_sendEvent, a_ragdollInstant);
		}
		static inline REL::Relocation<decltype(thunk)> func;

		static void Install()
		{
			REL::Relocation<std::uintptr_t> vt{ RE::VTABLE_Character[0] };
			func = vt.write_vfunc(0x10E, thunk);
			logger::info("kill hook on Character's vtable (slot 10E)");
		}
	};

	class KeySink : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
		{
			if (!a_events || RE::UI::GetSingleton()->GameIsPaused()) {
				return RE::BSEventNotifyControl::kContinue;
			}
			for (auto* e = *a_events; e; e = e->next) {
				if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || e->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
					continue;
				}
				if (auto* b = e->AsButtonEvent(); b && b->IsDown()) {
					if (const auto key = b->GetIDCode(); key == 0x41 || key == 0x42) {
						logger::info("animation hook calls so far: {}{}", UpdateAnimationInternal::calls.load(),
							UpdateAnimationInternal::installed ? "" : " (hook not installed)");
					}
					if (core::Use c; c) {
						c->onKey(b->GetIDCode());
					}
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			{
				core::dataLoaded = true;
				if (core::Use c; c) {
					c->dataLoaded();
				}
				static KeySink keys;
				RE::BSInputDeviceManager::GetSingleton()->AddEventSink<RE::InputEvent*>(&keys);
				std::thread(core::Watch).detach();
				logger::info("input and command file installed");
			}
			break;
		default:
			break;
		}
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 0, 1, 0, 0 },
	.Name = "SkyrimDelirium"sv,
	.Author = "Youcef"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .trampoline = false });
	logger::info("SkyrimDelirium {} loaded", DELIRIUM_VERSION);
	{
		HMODULE m{};
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&core::Load), &m);
		wchar_t p[MAX_PATH]{};
		GetModuleFileNameW(m, p, MAX_PATH);
		core::pluginDir = std::filesystem::path(p).parent_path();
	}
	core::Load();
	// CommonLib allocates the trampoline once (later requests are ignored): room for every hook (14 bytes each, the
	// animation hook's extra call sites too) and the entry hooks' copied openings
	SKSE::AllocTrampoline(512);
	UpdateAnimationInternal::Install();  // does nothing per frame until a body is active
	PoseToNodes::Install();
	DriveToPose::Install();
	GetUpCheck::Install();
	ProcessHitData::Install();
	Stagger::Install();
	Kill::Install();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
