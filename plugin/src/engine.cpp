#include "engine.h"

#include "collision.h"

#include <SimpleIni.h>
#include <fstream>
#include <cstring>
#include <functional>
#include <random>
#include <sstream>
#include <unordered_map>

namespace delirium
{
	extern std::filesystem::path       g_pluginDir;
	extern const SKSE::TaskInterface* g_tasks;
	namespace
	{
		constexpr float kUnit = 0.0142875f;  // metres per game unit
		constexpr float kStep = 1.0f / 60.0f;
		constexpr int   kMaxSteps = 4;
		constexpr float kMaxSpin = 3.0f * 6.2831853f;  // Delirium's MAX_SPIN_NM (rad/s)
		// NM part order's parents (Buttocks is the root)
		constexpr std::array<int, 21> kParent{ -1, 0, 1, 2, 0, 4, 5, 0, 7, 8, 9, 10, 11, 10, 13, 14, 15, 10, 17, 18, 19 };
		constexpr int                 kChest = 10;
		constexpr int                 kHead = 12;
		constexpr float               kBlendOut = 0.4f;  // seconds from Delirium's pose back to the animation
		constexpr int                 kMaxCorpses = 16;  // handed-back corpses whose pose difference is kept; the oldest goes first
		std::mt19937                  g_rng{ std::random_device{}() };
		float                         Rand(float a_lo, float a_hi) { return std::uniform_real_distribution<float>(a_lo, a_hi)(g_rng); }

		Q Slerp(Q a, Q b, float t)
		{
			float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
			if (d < 0) {
				b = { -b.x, -b.y, -b.z, -b.w };
				d = -d;
			}
			float wa = 1 - t, wb = t;
			if (d < 0.9995f) {
				const float th = std::acos(d), sn = std::sin(th);
				wa = std::sin((1 - t) * th) / sn;
				wb = std::sin(t * th) / sn;
			}
			Q         q{ wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w };
			const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
			return { q.x / n, q.y / n, q.z / n, q.w / n };
		}

		// ---- Delirium's C API, loaded at runtime from SKSE/Plugins/SkyrimDelirium/delirium_physics.dll
#define DL_FNS(X)                                                                                           \
	X(nm_last_error) X(nm_init) X(nm_world_create) X(nm_world_destroy) X(nm_world_add_floor) X(nm_build_load) \
	X(nm_agent_create) X(nm_agent_destroy) X(nm_agent_step) X(nm_agent_set_mode) X(nm_agent_get_part_transform) \
	X(nm_agent_set_part_state) X(nm_agent_get_kinetic_energy) X(nm_agent_apply_push) X(nm_msg_begin)           \
	X(nm_msg_set_bool) X(nm_msg_set_float) X(nm_msg_send) X(nm_agent_is_balancing) X(nm_agent_has_fallen) X(nm_agent_set_zero_pose) \
	X(nm_build_scale) X(nm_build_destroy) X(nm_world_add_mesh) X(nm_world_raycast) X(nm_world_triangle_count) X(nm_agent_get_com) X(nm_agent_task_shot) \
	X(nm_agent_get_part_drives) X(nm_build_part_info) X(nm_agent_get_joint_accels)
		struct Api
		{
#define DL_DECL(f) decltype(&::f) f = nullptr;
			DL_FNS(DL_DECL)
#undef DL_DECL
		} dl;

		std::string LastError()
		{
			char b[512]{};
			if (dl.nm_last_error) {
				dl.nm_last_error(b, sizeof(b));
			}
			return b;
		}

		std::filesystem::path PluginDir() { return g_pluginDir; }  // from the host: the core runs from a temporary copy

		bool LoadApi()
		{
			// loaded from a copy beside the core's own temporary copy: the mod's file is never locked, so a rebuilt
			// Delirium goes in with a hot reload like the core
			auto            path = PluginDir() / L"SkyrimDelirium" / L"delirium_physics.dll";
			HMODULE         self{};
			wchar_t         own[MAX_PATH]{};
			std::error_code ec;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&LoadApi), &self) &&
				GetModuleFileNameW(self, own, MAX_PATH)) {
				const auto copy = std::filesystem::path(own).parent_path() / L"delirium_physics.dll";
				if (CopyFileW(path.c_str(), copy.c_str(), FALSE)) {
					path = copy;
				}
			}
			const HMODULE h = LoadLibraryW(path.c_str());
			logger::info("Delirium from {}", path.string());
			if (!h) {
				logger::error("can't load {} (error {})", path.string(), GetLastError());
				return false;
			}
			bool ok = true;
#define DL_LOAD(f)                                                          \
	dl.f = reinterpret_cast<decltype(dl.f)>(GetProcAddress(h, #f));         \
	if (!dl.f) {                                                            \
		logger::error("delirium_physics.dll has no " #f " (old build?)"); \
		ok = false;                                                         \
	}
			DL_FNS(DL_LOAD)
#undef DL_LOAD
			return ok;
		}

		// ---- small math: Skyrim's NiMatrix3 is row-major with world = R * local, Delirium's quaternions are glam's
		Q Mul(const Q& a, const Q& b)
		{
			return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
				a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
		}
		Q  Conj(const Q& q) { return { -q.x, -q.y, -q.z, q.w }; }
		V3 Rot(const Q& q, const V3& v)
		{
			const V3 u{ q.x, q.y, q.z };
			const V3 t{ 2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x) };
			return { v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
				v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
		}
		V3 Add(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		V3 Sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		V3 Scl(const V3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }

		Q FromMat(const RE::NiMatrix3& m)
		{
			const float r00 = m.entry[0][0], r11 = m.entry[1][1], r22 = m.entry[2][2];
			Q           q;
			const float tr = r00 + r11 + r22;
			if (tr > 0) {
				const float s = std::sqrt(tr + 1.0f) * 2;
				q = { (m.entry[2][1] - m.entry[1][2]) / s, (m.entry[0][2] - m.entry[2][0]) / s, (m.entry[1][0] - m.entry[0][1]) / s, 0.25f * s };
			} else if (r00 > r11 && r00 > r22) {
				const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2;
				q = { 0.25f * s, (m.entry[0][1] + m.entry[1][0]) / s, (m.entry[0][2] + m.entry[2][0]) / s, (m.entry[2][1] - m.entry[1][2]) / s };
			} else if (r11 > r22) {
				const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2;
				q = { (m.entry[0][1] + m.entry[1][0]) / s, 0.25f * s, (m.entry[1][2] + m.entry[2][1]) / s, (m.entry[0][2] - m.entry[2][0]) / s };
			} else {
				const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2;
				q = { (m.entry[0][2] + m.entry[2][0]) / s, (m.entry[1][2] + m.entry[2][1]) / s, 0.25f * s, (m.entry[1][0] - m.entry[0][1]) / s };
			}
			const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
			return { q.x / n, q.y / n, q.z / n, q.w / n };
		}

		float Angle(Q a, const Q& b)
		{
			const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
			return 2.0f * std::acos(std::min(d, 1.0f));
		}

		// hkaSkeletonMapper (Havok 2010): hkReferencedObject, then hkaSkeletonMapperData
		struct SimpleMapping
		{
			std::int16_t  boneA, boneB;
			std::uint8_t  pad04[0xC];
			float         t[4], r[4], s[4];  // aFromBTransform: B = A * aFromB
		};
		static_assert(sizeof(SimpleMapping) == 0x40);
		struct SkeletonMapper
		{
			std::uint8_t           pad00[0x10];
			const RE::hkaSkeleton* skeletonA;
			const RE::hkaSkeleton* skeletonB;
			const SimpleMapping*   simple;
			std::int32_t           simpleCount, simpleCap;
		};

		RE::NiMatrix3 ToMat(const Q& q)
		{
			RE::NiMatrix3 m;
			const float   xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
			const float   wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
			m.entry[0][0] = 1 - 2 * (yy + zz);
			m.entry[0][1] = 2 * (xy - wz);
			m.entry[0][2] = 2 * (xz + wy);
			m.entry[1][0] = 2 * (xy + wz);
			m.entry[1][1] = 1 - 2 * (xx + zz);
			m.entry[1][2] = 2 * (yz - wx);
			m.entry[2][0] = 2 * (xz - wy);
			m.entry[2][1] = 2 * (yz + wx);
			m.entry[2][2] = 1 - 2 * (xx + yy);
			return m;
		}

		RE::NiAVObject* Node(RE::NiAVObject* a_root, const std::string& a_name)
		{
			return a_root->GetObjectByName(RE::BSFixedString(a_name.c_str()));
		}

		std::vector<float> Floats(const std::string& s)
		{
			std::vector<float> v;
			std::istringstream in(s);
			for (float f; in >> f;) {
				v.push_back(f);
			}
			return v;
		}

		// Skeleton nodes only, transforms only (world = parent world * local). Attached models (sheathed sword, shield: their
		// own nif root, a BSFadeNode) and meshes are left to Skyrim's own update: rewriting or UpdateWorldData-ing them here
		// made their textures flicker and doubled FSMP hair. Skinned meshes (body, armour, hair) draw from the bones anyway.
		void UpdateTransforms(RE::NiAVObject* a_obj, bool a_start = true)
		{
			auto* node = a_obj->AsNode();
			if (!node || (!a_start && a_obj->AsFadeNode())) {
				return;
			}
			if (a_obj->parent) {
				a_obj->world = a_obj->parent->world * a_obj->local;
			}
			for (auto& ch : node->GetChildren()) {
				if (ch) {
					UpdateTransforms(ch.get(), false);
				}
			}
		}
		void HookUpdate3D(RE::Actor* a_actor);  // below, with AfterRootMove
		void UnhookUpdate3D();
		std::atomic<int> g_count{ 0 };  // active agents, read without the lock by every actor's animation update
		std::atomic<int> g_skip{ 0 };   // ragdolled NPCs left to Skyrim (skip_), same

		// The loading screen (a save loaded, a door): every NPC is let go. Their 3D is rebuilt after it, and an entry
		// left over kept a reloaded NPC on the ground (KeepsDown) with nothing moving her.
		struct LoadSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
					Engine::Get().ReleaseAll("loading screen");
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_loadSink;
		bool g_loadSinkOn = false;
	}

	Engine& Engine::Get()
	{
		static Engine e;
		return e;
	}

	void Engine::LoadSettings()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile("Data/SKSE/Plugins/SkyrimDelirium.ini");
		fredPath_ = ini.GetValue("Data", "sCharacterFile", "");
		buildPath_ = ini.GetValue("Data", "sBody", "Data/SKSE/Plugins/SkyrimDelirium/skyrim_build.txt");
		rigPath_ = ini.GetValue("Data", "sRig", "Data/SKSE/Plugins/SkyrimDelirium/skyrim_rig.txt");
		pushKey_ = static_cast<std::uint32_t>(ini.GetLongValue("Test", "iPushKey", 0x41));
		releaseKey_ = static_cast<std::uint32_t>(ini.GetLongValue("Test", "iReleaseKey", 0x3C));
		push_ = static_cast<float>(ini.GetDoubleValue("Test", "fPush", 80.0));
		autoRelease_ = ini.GetBoolValue("Test", "bAutoRelease", true);
		followAnimation_ = ini.GetBoolValue("Test", "bFollowAnimation", true);
		collision_ = ini.GetBoolValue("Collision", "bWorldCollision", true);
		colRadius_ = static_cast<float>(ini.GetDoubleValue("Collision", "fRadius", 600.0));
		colHeight_ = static_cast<float>(ini.GetDoubleValue("Collision", "fHeight", 300.0));
		colMaxTris_ = static_cast<std::size_t>(std::max(1000L, ini.GetLongValue("Collision", "iMaxTriangles", 40000)));
		takeOver_ = ini.GetBoolValue("Ragdoll", "bTakeOver", true);
		native_ = ini.GetBoolValue("Ragdoll", "bNative", true);
		sync_ = ini.GetBoolValue("Ragdoll", "bSync", false);
		torque_ = ini.GetBoolValue("Ragdoll", "bTorque", true);
		gain_ = static_cast<float>(ini.GetDoubleValue("Ragdoll", "fTorqueGain", 1.0));
		logger::info("torque drive (Delirium's muscles move Skyrim's ragdoll) {}, gain {:.2f}", torque_, gain_);
		strength_ = static_cast<float>(ini.GetDoubleValue("Ragdoll", "fStrength", sync_ ? 1.0 : 0.35));
		logger::info("native ragdoll (taken over ragdolls keep their mass, Skyrim poses the bones) {}, Delirium synced from it {}, steering strength {:.2f} per step",
			native_, sync_, strength_);
		logger::info("settings: character file '{}', push key {:#x}, release key {:#x}, push {} N s, follow animation {}, take over Skyrim's ragdolls {}",
			fredPath_, pushKey_, releaseKey_, push_, followAnimation_, takeOver_);
	}

	bool Engine::Init()
	{
		if (ready_) {
			return true;
		}
		if (!LoadApi()) {
			return false;
		}
		if (fredPath_.empty()) {
			logger::error("no character file: set sCharacterFile in SkyrimDelirium.ini (GTA IV's pc/data/naturalmotion/Fred.art)");
			return false;
		}
		if (dl.nm_init(fredPath_.c_str()) < 0) {
			logger::error("nm_init({}): {}", fredPath_, LastError());
			return false;
		}
		build_ = dl.nm_build_load(buildPath_.c_str());
		if (!build_) {
			logger::error("body {}: {}", buildPath_, LastError());
			return false;
		}
		std::ifstream in(rigPath_);
		if (!in) {
			logger::error("can't read rig {}", rigPath_);
			return false;
		}
		std::string line;
		std::size_t n = 0;
		while (std::getline(in, line)) {
			if (line.rfind("yaw ", 0) == 0) {
				rigYaw_ = std::stof(line.substr(4));
				continue;
			}
			std::vector<std::string> f;
			for (std::size_t s = 0, e; s <= line.size(); s = e + 1) {
				e = line.find('|', s);
				if (e == std::string::npos) {
					e = line.size();
				}
				f.push_back(line.substr(s, e - s));
			}
			if (f.size() != 8 || n >= rig_.size()) {
				continue;
			}
			const auto q = Floats(f[5]);
			const auto a = Floats(f[6]);
			const auto p = Floats(f[7]);
			if (q.size() != 4 || a.size() != 3 || p.size() != 3) {
				continue;
			}
			rig_[n++] = { f[0], f[1], f[3], f[4], f[2] == "1", { q[0], q[1], q[2], q[3] }, { a[0], a[1], a[2] }, { p[0], p[1], p[2] } };
		}
		if (n != rig_.size()) {
			logger::error("rig {}: {} parts, want 21", rigPath_, n);
			return false;
		}
		ready_ = true;
		if (auto* ui = RE::UI::GetSingleton(); ui && !g_loadSinkOn) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_loadSink);
			g_loadSinkOn = true;
		}
		logger::info("ready: body {}, rig {} (build faces {:.2} rad)", buildPath_, rigPath_, rigYaw_);
		return true;
	}

	void Engine::ReleaseAll(const char* a_why)
	{
		std::scoped_lock l(lock_);
		while (!active_.empty()) {
			Release(active_.size() - 1, a_why);
		}
		skip_.clear();
		g_skip = 0;
	}

	void Engine::Shutdown()
	{
		if (auto* ui = RE::UI::GetSingleton(); ui && g_loadSinkOn) {
			ui->RemoveEventSink<RE::MenuOpenCloseEvent>(&g_loadSink);
			g_loadSinkOn = false;
		}
		std::scoped_lock l(lock_);
		while (!active_.empty()) {
			Release(active_.size() - 1, "core unloading");
		}
		UnhookUpdate3D();
		if (build_) {
			dl.nm_build_destroy(build_);
			build_ = nullptr;
		}
		ready_ = false;
		logger::info("core shut down");
	}

	// Command file lines (written by tools, run on the main thread):
	//   key <hex DIK> [formid]   as if the key was pressed, aimed at that NPC (else crosshair / console pick)
	//   push <hex formid> [N s]  push that NPC (optionally with this impulse)
	//   release                  let go of every NPC
	//   reset <hex formid> [dist] stand that NPC in front of the player, facing him
	//   fus <hex formid> [force] Skyrim's own push from the player (as Fus Ro Dah: force 15), no Delirium involved
	//   spawn <hex base> [dist]  place that NPC / leveled NPC in front of the player (3DECD = a melee bandit)
	//   near                     log the NPCs near the player
	//   status                   log what is active

	void Engine::Command(std::string_view a_line)
	{
		std::vector<std::string> w;
		for (std::size_t s = 0, e; s < a_line.size(); s = e + 1) {
			e = a_line.find(' ', s);
			e = e == std::string_view::npos ? a_line.size() : e;
			if (e > s) {
				w.emplace_back(a_line.substr(s, e - s));
			}
		}
		if (w.empty()) {
			return;
		}
		logger::info("command: {}", a_line);
		auto actor = [&](std::size_t i) -> RE::Actor* {
			if (i >= w.size()) {
				return nullptr;
			}
			auto* a = RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(std::stoul(w[i], nullptr, 16)));
			if (!a) {
				logger::warn("command: no actor {}", w[i]);
			}
			return a;
		};
		try {
			if (w[0] == "key" && w.size() >= 2) {
				OnKey(static_cast<std::uint32_t>(std::stoul(w[1], nullptr, 16)), actor(2));
			} else if (w[0] == "push" && w.size() >= 2) {
				const float keep = push_;
				if (w.size() >= 3) {
					push_ = std::stof(w[2]);
				}
				if (auto* a = actor(1)) {
					OnKey(pushKey_, a);
				}
				push_ = keep;
			} else if (w[0] == "release") {
				OnKey(releaseKey_);
			} else if (w[0] == "reset") {
				// stand the NPC 150 units in front of the player, facing the player (a push sends her away from him)
				if (auto* a = actor(1)) {
					{
						std::scoped_lock l(lock_);
						for (std::size_t i = active_.size(); i-- > 0;) {
							if (active_[i]->actor.get().get() == a) {
								Release(i, "reset");
							}
						}
					}
					auto*       player = RE::PlayerCharacter::GetSingleton();
					const float h = player->GetAngleZ();
					const float dist = w.size() >= 3 ? std::stof(w[2]) : 150.0f;
					auto        pos = player->GetPosition() + RE::NiPoint3(std::sin(h), std::cos(h), 0.0f) * dist;
					pos.z += 5.0f;
					a->SetPosition(pos, true);
					a->SetAngle({ 0.0f, 0.0f, h + 3.14159265f });
					logger::info("reset: {:08X} at ({:.0f} {:.0f} {:.0f}), ragdoll {}", a->GetFormID(), pos.x, pos.y, pos.z, a->IsInRagdollState());
				}
			} else if (w[0] == "fus" && w.size() >= 2) {
				if (auto* a = actor(1)) {
					const float force = w.size() >= 3 ? std::stof(w[2]) : 15.0f;
					g_tasks->AddTask([h = a->GetHandle(), force]() {
						auto* player = RE::PlayerCharacter::GetSingleton();
						if (auto act = h.get(); act && player) {
							if (auto* proc = act->GetActorRuntimeData().currentProcess) {
								proc->KnockExplosion(act.get(), player->GetPosition(), force);
							}
						}
					});
				}
			} else if (w[0] == "spawn" && w.size() >= 2) {
				// place a base object (an NPC or leveled NPC) in front of the player
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(static_cast<RE::FormID>(std::stoul(std::string(w[1]), nullptr, 16)));
				if (player && base) {
					if (auto ref = player->PlaceObjectAtMe(base, false)) {
						const float h = player->GetAngleZ();
						const float dist = w.size() >= 3 ? std::stof(std::string(w[2])) : 300.0f;
						auto        pos = player->GetPosition() + RE::NiPoint3(std::sin(h), std::cos(h), 0.0f) * dist;
						pos.z += 5.0f;
						ref->SetPosition(pos);
						ref->SetAngle({ 0.0f, 0.0f, h + 3.14159265f });
						logger::info("spawn: {:08X} '{}' (base {:08X}) at ({:.0f} {:.0f} {:.0f})", ref->GetFormID(), ref->GetName(), base->GetFormID(), pos.x, pos.y,
							pos.z);
					}
				} else {
					logger::info("spawn: no base object {}", w[1]);
				}
			} else if (w[0] == "native" && w.size() >= 2) {
				native_ = w[1] != "0";
				logger::info("native ragdoll {} (for the next takeover)", native_);
			} else if (w[0] == "sync" && w.size() >= 2) {
				sync_ = w[1] != "0";
				logger::info("Delirium synced from Skyrim's ragdoll {}", sync_);
			} else if (w[0] == "torque" && w.size() >= 2) {
				torque_ = w[1] != "0";
				logger::info("torque drive {}", torque_);
			} else if (w[0] == "gain" && w.size() >= 2) {
				gain_ = std::clamp(std::stof(std::string(w[1])), 0.0f, 10.0f);
				logger::info("torque gain {:.2f}", gain_);
			} else if (w[0] == "strength" && w.size() >= 2) {
				strength_ = std::clamp(std::stof(std::string(w[1])), 0.0f, 1.0f);
				logger::info("steering strength {:.2f} per step", strength_);
			} else if (w[0] == "near") {
				// NPCs near the player: id, name, distance
				auto* player = RE::PlayerCharacter::GetSingleton();
				for (auto& h : RE::ProcessLists::GetSingleton()->highActorHandles) {
					if (auto a = h.get(); a && a.get() != player) {
						logger::info("near: {:08X} '{}' {:.0f} units, ragdoll {}, dead {}", a->GetFormID(), a->GetName(),
							a->GetPosition().GetDistance(player->GetPosition()), a->IsInRagdollState(), a->IsDead());
					}
				}
			} else if (w[0] == "status") {
				std::scoped_lock l(lock_);
				logger::info("status: ready {}, {} active", ready_, active_.size());
				for (const auto& a : active_) {
					auto act = a->actor.get();
					logger::info("  {:08X}: age {:.1f} s, knocked {}, held for get-up {:.1f} s, in ragdoll {}", act ? act->GetFormID() : 0, a->age,
						a->knockQueued, a->handover, act && act->IsInRagdollState());
				}
			} else {
				logger::warn("command: unknown '{}'", w[0]);
			}
		} catch (const std::exception& e) {
			logger::warn("command '{}': {}", a_line, e.what());
		}
	}

	void Engine::OnKey(std::uint32_t a_key, RE::Actor* a_target)
	{
		if (!ready_ || (a_key != pushKey_ && a_key != releaseKey_)) {
			return;
		}
		RE::Actor* target = a_target;
		if (auto* pick = target ? nullptr : RE::CrosshairPickData::GetSingleton()) {
			if (auto ref = pick->GetActiveTarget().get()) {
				target = ref->As<RE::Actor>();
			}
		}
		if (!target) {
			if (auto ref = RE::Console::GetSelectedRef()) {
				target = ref->As<RE::Actor>();
			}
		}
		std::scoped_lock l(lock_);
		if (a_key == releaseKey_) {
			for (std::size_t i = active_.size(); i-- > 0;) {
				Release(i, "release key");
			}
			return;
		}
		if (!target || target->IsPlayerRef() || target->IsDead()) {
			logger::info("push key: no living NPC under the crosshair (or selected in the console)");
			return;
		}
		for (auto& a : active_) {
			if (a->actor.get().get() == target) {
				Push(*a, target);
				return;
			}
		}
		if (Activate(target)) {
			Push(*active_.back(), target);
		}
	}

	bool Engine::Activate(RE::Actor* a_actor, bool a_takeover, bool a_death)
	{
		auto* root = a_actor->Get3D(false);
		if (!root) {
			return false;
		}
		HookUpdate3D(a_actor);
		auto act = std::make_unique<Active>();
		act->actor = a_actor->GetHandle();
		act->origin = a_actor->GetPosition();
		for (std::size_t i = 0; i < 21; ++i) {
			act->ref[i] = Node(root, rig_[i].ref);
			act->ja[i] = Node(root, rig_[i].jointA);
			act->jb[i] = Node(root, rig_[i].jointB);
			if (!act->ref[i] || !act->ja[i] || !act->jb[i]) {
				logger::warn("{:08X}: no bone for {} ({})", a_actor->GetFormID(), rig_[i].name, rig_[i].ref);
				return false;
			}
		}
		act->com = Node(root, "NPC COM [COM ]");
		if (!act->com || !act->com->parent) {
			logger::warn("{:08X}: no NPC COM bone", a_actor->GetFormID());
			return false;
		}
		act->comInPelvis = act->ref[0]->world.rotate.Transpose() * act->com->world.rotate;
		// the NPC's size (race height, NPC weight scale): the body and the rig offsets are for a scale-1 skeleton
		act->scale = std::clamp(act->ref[0]->world.scale, 0.3f, 3.0f);
		std::array<V3, 21> pos;
		std::array<Q, 21>  rot;
		std::string        detail;
		const float        gap = PartsFromBones(*act, pos, rot, &detail);
		logger::info("{:08X}: joint vs bone gap, worst {:.3} m:{}", a_actor->GetFormID(), gap, detail);
		// the pose for replaying offline (fit-test REPLAY=): per part, ref bone world rotation rows and the joint bones' positions
		{
			auto rel = [&](RE::NiAVObject* n) {
				const auto w = (n->world.translate - act->origin) * kUnit;
				return V3{ w.x, w.y, w.z };
			};
			std::ostringstream f;
			f << "heading " << a_actor->data.angle.z << " scale " << act->ref[0]->world.scale << "\n";
			for (std::size_t i = 0; i < 21; ++i) {
				const auto& m = act->ref[i]->world.rotate;
				const V3    r = rel(act->ref[i]), a = rel(act->ja[i]), b = rel(act->jb[i]);
				f << rig_[i].name << "|";
				for (int r0 = 0; r0 < 3; ++r0) {
					for (int c = 0; c < 3; ++c) {
						f << m.entry[r0][c] << " ";
					}
				}
				f << "|" << r.x << " " << r.y << " " << r.z << "|" << a.x << " " << a.y << " " << a.z << "|" << b.x << " " << b.y << " " << b.z << "\n";
			}
			std::ofstream(*SKSE::log::log_directory() / "SkyrimDelirium_pose.txt") << f.str();
		}
		const float heading = a_actor->data.angle.z;  // forward = (sin, cos)
		const float yaw = std::atan2(std::cos(heading), std::sin(heading)) - rigYaw_;
		const float zero[3]{};
		if (std::abs(act->scale - 1.0f) > 1e-3f) {
			// a copy of the body at the NPC's size (mass by volume); the agent keeps its own copy
			std::array<float, 21> len;
			len.fill(act->scale);
			NmBuild* sized = dl.nm_build_scale(build_, len.data(), act->scale * act->scale * act->scale);
			if (!sized) {
				logger::error("scale {:.2}: {}", act->scale, LastError());
				return false;
			}
			act->agent = dl.nm_agent_create(sized, zero, yaw, 0.3f);
			dl.nm_build_destroy(sized);
		} else {
			act->agent = dl.nm_agent_create(build_, zero, yaw, 0.3f);
		}
		act->world = dl.nm_world_create();
		if (!act->agent || !act->world) {
			logger::error("agent: {}", LastError());
			return false;
		}
		AddWorld(*act, a_actor, act->origin, false);
		dl.nm_agent_set_mode(act->agent, NM_MODE_LEAD);
		for (int i = 0; i < 21; ++i) {
			const float p[3]{ pos[i].x, pos[i].y, pos[i].z }, q[4]{ rot[i].x, rot[i].y, rot[i].z, rot[i].w };
			dl.nm_agent_set_part_state(act->agent, i, p, q, zero, zero);
		}
		// the posture the muscles and balance hold: the NPC's own pose, not the GTA character's stance
		SetZeroPose(*act, pos, rot);
		// upright (F7): balance. Taken over from Skyrim's ragdoll: already falling, so break the fall
		// (dead: the shot task once Skyrim's velocities are in, see InheritVelocity)
		if (!a_death) {
			dl.nm_msg_begin(act->agent, a_takeover ? "catchFall" : "bodyBalance");
			dl.nm_msg_set_bool(act->agent, "start", 1);
			dl.nm_msg_send(act->agent, act->world);
		}
		act->takeover = a_takeover;
		act->death = a_death;
		act->knockQueued = a_takeover;  // Skyrim's ragdoll state is already on: no knock of our own
		act->native = a_takeover && native_;
		logger::info("{:08X} '{}': active{}; scale {:.2}, pelvis at {:.2} m, head at {:.2} m, heading {:.2}", a_actor->GetFormID(), a_actor->GetName(),
			a_takeover ? " (took over Skyrim's ragdoll)" : "", act->scale, pos[0].z, pos[kHead].z, heading);
		active_.push_back(std::move(act));
		g_count = static_cast<int>(active_.size());
		return true;
	}

	void Engine::AddWorld(Active& a, RE::Actor* a_actor, const RE::NiPoint3& a_center, bool a_quiet)
	{
		// Skyrim's own collision around a_center (floors, walls, stairs, terrain, rocks, trees), in metres from the
		// NPC's origin; a flat floor at its feet if there is none (first world only)
		a.colCenter = a_center;
		bool        ground = false;
		std::size_t tris = 0;
		if (collision_) {
			const auto t0 = std::chrono::steady_clock::now();
			Tris        mesh;
			GatherStats st;
			const bool  ok = GatherStatic(a_actor->GetParentCell(), a_center, a.origin, colRadius_, colHeight_, colMaxTris_, mesh, st);
			if (ok && mesh.Count() > 0) {
				if (dl.nm_world_add_mesh(a.world, mesh.pos.data(), static_cast<std::int32_t>(mesh.pos.size() / 3), mesh.idx.data(),
						static_cast<std::int32_t>(mesh.Count()), nullptr) < 0) {
					logger::error("world mesh: {}", LastError());
				}
			}
			tris = static_cast<std::size_t>(std::max(0, dl.nm_world_triangle_count(a.world)));
			if (a_quiet) {
				const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				logger::info("  world re-gathered around the body ({:.1f} ms): {} triangles", ms, tris);
				return;
			}
			// what is under the pelvis, and which way it faces (a downward face = triangles wound inside out)
			const float from[3]{ 0, 0, 1.0f }, down[3]{ 0, 0, -1.0f };
			float       dist = 0, n[3]{};
			ground = tris > 0 && dl.nm_world_raycast(a.world, from, down, 2.5f, &dist, n) > 0;
			const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			logger::info("world: {} ({:.1f} ms): {} bodies, {} shapes, {} triangles kept ({} mesh tris, {} boxes, {} hulls, {} boxed, {} culled, {} over cap, {} other layers){}{}",
				ok ? "Skyrim collision" : "no Havok world", ms, st.bodies, st.shapes, tris, st.triangles, st.boxes, st.hulls, st.fallbacks, st.culled,
				st.capped, st.skippedLayer, ground ? fmt::format("; ground {:.2} m below the feet+1m, normal {:.2} {:.2} {:.2}", dist, n[0], n[1], n[2]) : "; no ground under the pelvis",
				st.unknown.empty() ? "" : "; boxed:" + st.unknown);
		}
		if (!ground) {
			dl.nm_world_add_floor(a.world, 0, 0, 0, 100.0f);
			logger::info("world: flat floor at the feet");
		}
	}

	float Engine::PartsFromBones(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot, std::string* detail) const
	{
		// parts from the bones as Skyrim posed them: rotations through the rig, positions down the chain from the pelvis.
		// Returns the worst gap between a physics joint and its Skyrim bone (calibration check).
		auto rel = [&](RE::NiAVObject* n) {
			const auto w = (n->world.translate - a.origin) * kUnit;
			return V3{ w.x, w.y, w.z };
		};
		float worst = 0;
		for (std::size_t i = 0; i < 21; ++i) {
			rot[i] = Mul(FromMat(a.ref[i]->world.rotate), Conj(rig_[i].offset));
			const V3 anchor = Scl(rig_[i].anchor, a.scale);  // the rig is for a scale-1 skeleton
			if (i == 0) {
				pos[0] = Sub(rel(a.ja[0]), Rot(rot[0], anchor));
			} else {
				const auto p = static_cast<std::size_t>(kParent[i]);
				pos[i] = Sub(Add(pos[p], Rot(rot[p], Scl(rig_[i].inParent, a.scale))), Rot(rot[i], anchor));
			}
			if (detail) {
				const V3    j = Add(pos[i], Rot(rot[i], anchor));
				const V3    ja = rel(a.ja[i]), jb = rel(a.jb[i]);
				const V3    d = Sub(j, { (ja.x + jb.x) / 2, (ja.y + jb.y) / 2, (ja.z + jb.z) / 2 });
				const float g = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
				worst = std::max(worst, g);
				*detail += fmt::format(" {}={:.2}", rig_[i].name, g);
			}
		}
		return worst;
	}

	void Engine::SetZeroPose(Active& a, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot)
	{
		std::array<float, 21 * 7> tf{};
		for (std::size_t i = 0; i < 21; ++i) {
			const float v[7]{ pos[i].x, pos[i].y, pos[i].z, rot[i].x, rot[i].y, rot[i].z, rot[i].w };
			std::copy_n(v, 7, tf.begin() + static_cast<std::ptrdiff_t>(i * 7));
		}
		if (dl.nm_agent_set_zero_pose(a.agent, tf.data(), 21) < 0 && !a.logged) {
			logger::error("zero pose: {}", LastError());
			a.logged = true;
		}
	}

	void Engine::Push(Active& a_act, RE::Actor* a_actor)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  d = a_actor->GetPosition() - player->GetPosition();
		d.z = 0;
		const float len = std::max(d.Length(), 1.0f);
		float chest[3]{}, q[4]{};
		dl.nm_agent_get_part_transform(a_act.agent, kChest, chest, q);
		const float imp[3]{ push_ * d.x / len, push_ * d.y / len, 0.0f };
		dl.nm_agent_apply_push(a_act.agent, chest, imp);
		a_act.calm = 0;
		a_act.sincePush = 0;  // not age: SmoothRoot and the logs time themselves by age
		a_act.blend = 1.0f;  // a new push mid fade-out takes the body back fully
		a_act.blendingOut = false;
		logger::info("{:08X}: push {} N s", a_actor->GetFormID(), push_);
	}

	void Engine::RestoreBodies(Active& a)
	{
		for (auto& l : a.bodies) {
			std::memset(&l.body->motion.linearVelocity, 0, 16);
			std::memset(&l.body->motion.angularVelocity, 0, 16);
			std::memcpy(&l.body->motion.inertiaAndMassInv, l.massInv.data(), sizeof(l.massInv));
			l.body->motion.gravityFactor = l.gravity;
		}
	}

	void Engine::Release(std::size_t a_i, const char* a_why)
	{
		auto& a = active_[a_i];
		logger::info("released ({}) after {:.1} s", a_why, a->age);
		if (auto act = a->actor.get(); act && act->IsInRagdollState()) {
			skip_.push_back(a->actor);  // still in Skyrim's ragdoll (its get-up, or F2): not taken over again
			g_skip = static_cast<int>(skip_.size());
		}
		// Skyrim's ragdoll gets its own mass and gravity back and carries on limp from our pose (then gets up)
		if (auto act = a->actor.get(); act && act->Get3D(false) && !a->handedBack) {
			RestoreBodies(*a);
		}
		dl.nm_agent_destroy(a->agent);
		dl.nm_world_destroy(a->world);
		active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(a_i));
		g_count = static_cast<int>(active_.size());
	}

	// Skyrim's Actor::UpdateActor3DPosition (vtable slot 0x11F) puts the 3D root on the actor's position, heading and
	// controller tilt. Going ragdoll it moves the actor from her feet to the ragdoll's root bone and tilts it (50+ units,
	// 90+ deg) after we wrote the bones: every bone would follow for a frame, and bone physics (CBPC, SMP) whips.
	// Patched in the actor's own vtable by the core and put back when it unloads.
	namespace
	{
		using Update3DFn = void (*)(RE::Actor*);
		std::uintptr_t* g_vtSlot{};
		Update3DFn      g_origUpdate3D{};

		void Update3DHook(RE::Actor* a_actor)
		{
			g_origUpdate3D(a_actor);
			if (Engine::Busy()) {
				Engine::Get().AfterRootMove(a_actor);
			}
		}
		void PatchSlot(std::uintptr_t* a_slot, std::uintptr_t a_value)
		{
			DWORD old{};
			VirtualProtect(a_slot, sizeof(*a_slot), PAGE_EXECUTE_READWRITE, &old);
			*a_slot = a_value;
			VirtualProtect(a_slot, sizeof(*a_slot), old, &old);
		}
		void HookUpdate3D(RE::Actor* a_actor)
		{
			if (g_vtSlot) {
				return;
			}
			auto* vt = *reinterpret_cast<std::uintptr_t**>(a_actor);
			g_vtSlot = vt + 0x11F;
			g_origUpdate3D = reinterpret_cast<Update3DFn>(*g_vtSlot);
			PatchSlot(g_vtSlot, reinterpret_cast<std::uintptr_t>(&Update3DHook));
			logger::info("hooked UpdateActor3DPosition (vtable {:X}, was {:X})", reinterpret_cast<std::uintptr_t>(vt),
				reinterpret_cast<std::uintptr_t>(g_origUpdate3D));
		}
		void UnhookUpdate3D()
		{
			if (g_vtSlot) {
				PatchSlot(g_vtSlot, reinterpret_cast<std::uintptr_t>(g_origUpdate3D));
				g_vtSlot = nullptr;
			}
		}
	}

	void Engine::AfterRootMove(RE::Actor* a_actor)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			auto& a = *p;
			if (!a.knockQueued || (a.native && a.calibrated) || a.actor.get().get() != a_actor) {
				continue;
			}
			auto* top = a_actor->Get3D(false);
			if (!top) {
				return;
			}
			// 1. the top node glides after Skyrim's instead of jumping (FSMP and SMP gear simulate relative to it)
			// (not for a handed-back corpse: its carried pose is relative to Skyrim's own top node)
			if (!a.handedBack) {
				SmoothRoot(a, a_actor);
			}
			if (!a.haveCom) {
				return;
			}
			// 2. the nodes between it and COM, refreshed from it (only those: the bones must not move even for a moment)
			std::vector<RE::NiAVObject*> chain;
			for (auto* n = a.com->parent; n && n != top; n = n->parent) {
				chain.push_back(n);
			}
			RE::NiUpdateData ud{};
			for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
				(*it)->UpdateWorldData(&ud);
			}
			// 3. COM keeps the world transform the last pose copy gave it, and with it every bone below
			const auto& pw = a.com->parent->world;
			const auto  before = pw.rotate * (a.com->local.translate * pw.scale) + pw.translate;
			const float off = (before - a.comWorld.translate).Length();
			const auto  inv = pw.rotate.Transpose();
			a.com->local.rotate = inv * a.comWorld.rotate;
			a.com->local.translate = (inv * (a.comWorld.translate - pw.translate)) / pw.scale;
			a.com->local.scale = a.comWorld.scale / pw.scale;
			a.com->world = a.comWorld;
			a.maxRootFix = std::max(a.maxRootFix, off);
			if (off > 1.0f && a.rootFixLog < 10) {
				++a.rootFixLog;
				logger::info("    root moved after the pose: COM would have moved {:.1f} units, held (age {:.3f}, top node lags {:.2f})", off, a.age,
					a.rootLag);
			}
			return;
		}
	}

	// Where Skyrim's knock "explosion" comes from: never the actor's own position, the direction from the source to
	// the actor would be zero and its normal NaN (a NaN ragdoll impulse: SMP cloth turns grey and duplicates).
	// Behind her along a_dir (flat), else behind her facing.
	static RE::NiPoint3 KnockSource(RE::Actor* a_actor, RE::NiPoint3 a_dir)
	{
		if (a_dir.Length() < 1e-4f) {
			const float z = a_actor->GetAngleZ();
			a_dir = { -std::sin(z), -std::cos(z), 0.0f };  // falling backwards
		}
		a_dir.z = 0.0f;
		a_dir.Unitize();
		return a_actor->GetPosition() - a_dir * 64.0f;
	}

	// Every NPC's animation update comes through here, so any way into Skyrim's ragdoll is seen the frame it starts:
	// Fus Ro Dah, PushActorAway, explosions, other mods. Delirium takes the body from the pose on screen, with the
	// velocities Skyrim's push gave the ragdoll bodies (InheritVelocity), and keeps it until Skyrim's get-up, like F7.
	void Engine::TryTakeOver(RE::Actor* a_actor, bool a_dead)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			if (p->actor.get().get() == a_actor) {
				return;
			}
		}
		for (auto& h : skip_) {
			if (h.get().get() == a_actor) {
				return;
			}
		}
		if (a_dead) {
			// Skyrim's death ragdoll: Delirium dies instead (the shot task at no health, InheritVelocity), the corpse is
			// held still once it lies there
			if (!a_actor->Get3D(false)) {
				return;
			}
			if (a_actor->AsActorState()->GetLifeState() != RE::ACTOR_LIFE_STATE::kDying) {
				skip_.push_back(a_actor->GetHandle());
				g_skip = static_cast<int>(skip_.size());
				logger::info("{:08X} '{}': a corpse already lying there, left to Skyrim", a_actor->GetFormID(), a_actor->GetName());
				return;
			}
			logger::info("{:08X} '{}': died into Skyrim's ragdoll (knock state {}): taking over", a_actor->GetFormID(), a_actor->GetName(),
				static_cast<int>(a_actor->AsActorState()->GetKnockState()));
			if (!Activate(a_actor, true, true)) {
				skip_.push_back(a_actor->GetHandle());
				g_skip = static_cast<int>(skip_.size());
				logger::info("{:08X}: dead, left to Skyrim: no Delirium body for this skeleton", a_actor->GetFormID());
			}
			return;
		}
		using K = RE::KNOCK_STATE_ENUM;
		const auto ks = a_actor->AsActorState()->GetKnockState();
		if (ks == K::kNormal || !a_actor->Get3D(false)) {
			return;  // not knocked (yet): looked at again next frame
		}
		auto skip = [&](std::string_view a_why) {
			// until it leaves the ragdoll state (getting up, or let go of by us)
			skip_.push_back(a_actor->GetHandle());
			g_skip = static_cast<int>(skip_.size());
			logger::info("{:08X} '{}': Skyrim ragdoll (knock state {}), left to Skyrim: {}", a_actor->GetFormID(), a_actor->GetName(), static_cast<int>(ks), a_why);
		};
		if (ks == K::kQueued || ks == K::kGetUp) {
			return skip("already getting up");
		}
		if (a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kParalysis) > 0.0f) {
			return skip("paralysed (Skyrim holds the body stiff)");
		}
		// the lead-in comes before Skyrim's push: taken over then, the push would hit puppet bodies (no mass) and do
		// nothing. Skyrim's ragdoll runs it; Delirium starts once the push is on the bodies.
		{
			float mean = 0, fastest = 0, mass = 0;
			RE::BSAnimationGraphManagerPtr mgr;
			if (a_actor->GetAnimationGraphManager(mgr) && mgr && !mgr->graphs.empty() && mgr->graphs[0]) {
				if (auto* d = mgr->graphs[0]->characterInstance.ragdollDriver.get(); d && d->ragdoll) {
					for (auto* b : d->ragdoll->rigidBodies) {
						if (!b) {
							continue;
						}
						const auto* lv = reinterpret_cast<const float*>(&b->motion.linearVelocity);
						const float im = reinterpret_cast<const float*>(&b->motion.inertiaAndMassInv)[3];
						const float m = im > 0.0f ? 1.0f / im : 0.0f, s = std::sqrt(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
						mean += m * s, mass += m, fastest = std::max(fastest, s);
					}
				}
			}
			mean = mass > 0.0f ? mean / mass : 0.0f;
			logger::info("{:08X}: Skyrim ragdoll, knock state {}: bodies {:.2f} m/s mass-weighted, fastest {:.2f} m/s", a_actor->GetFormID(), static_cast<int>(ks),
				mean, fastest);
			if (ks == K::kExplodeLeadIn || ks == K::kOutLeadIn) {
				return;  // looked at again next frame
			}
		}
		logger::info("{:08X} '{}': into Skyrim's ragdoll (knock state {}): taking over", a_actor->GetFormID(), a_actor->GetName(), static_cast<int>(ks));
		if (!Activate(a_actor, true)) {
			return skip("no Delirium body for this skeleton");
		}
	}

	void Engine::ForgetSkip(RE::Actor* a_actor)
	{
		std::scoped_lock l(lock_);
		std::erase_if(skip_, [&](const RE::ActorHandle& h) {
			auto a = h.get();
			return !a || a.get() == a_actor;
		});
		g_skip = static_cast<int>(skip_.size());
	}

	// Taken over: Skyrim's ragdoll bodies have run a physics step or two with the push Skyrim gave them. Delirium's
	// parts start with those velocities (a part without a body of its own takes its parent's).
	// Skyrim's ragdoll bodies' velocities, per Delirium part (parts without a body: see below and the caller); returns the
	// fastest body's speed
	float Engine::BodyVelocities(const Active& a, std::array<V3, 21>& v, std::array<V3, 21>& w, std::array<bool, 21>& have) const
	{
		float fastest = 0;
		for (const auto& b : a.bodies) {
			const int i = b.rotPart >= 0 ? b.rotPart : b.posPart;
			if (i < 0 || have[i]) {
				continue;
			}
			const auto* lv = reinterpret_cast<const float*>(&b.body->motion.linearVelocity);
			const auto* av = reinterpret_cast<const float*>(&b.body->motion.angularVelocity);
			v[i] = { lv[0], lv[1], lv[2] };
			w[i] = { av[0], av[1], av[2] };
			if (!std::isfinite(v[i].x + v[i].y + v[i].z + w[i].x + w[i].y + w[i].z)) {
				continue;
			}
			have[i] = true;
			fastest = std::max(fastest, std::sqrt(v[i].x * v[i].x + v[i].y * v[i].y + v[i].z * v[i].z));
		}
		// a part without a body of its own but carrying one (Skyrim's root body NPC COM rides on the pelvis): that body's
		// velocity. The pelvis is Delirium's root: left at rest, the joint solve would pull the whole body back to it.
		for (const auto& b : a.bodies) {
			if (b.follow < 0 || b.follow >= 21 || have[b.follow]) {
				continue;
			}
			const auto* lv = reinterpret_cast<const float*>(&b.body->motion.linearVelocity);
			const auto* av = reinterpret_cast<const float*>(&b.body->motion.angularVelocity);
			const V3 lin{ lv[0], lv[1], lv[2] }, ang{ av[0], av[1], av[2] };
			if (std::isfinite(lin.x + lin.y + lin.z + ang.x + ang.y + ang.z)) {
				v[b.follow] = lin;
				w[b.follow] = ang;
				have[b.follow] = true;
			}
		}
		if (!have[0]) {
			// else the mass-weighted mean of all bodies (no spin)
			V3    sum{};
			float mass = 0;
			for (const auto& b : a.bodies) {
				const auto* lv = reinterpret_cast<const float*>(&b.body->motion.linearVelocity);
				const float im = b.massInv[3];
				const float m = im > 0.0f ? 1.0f / im : 0.0f;
				if (m > 0.0f && std::isfinite(lv[0] + lv[1] + lv[2])) {
					sum = Add(sum, Scl({ lv[0], lv[1], lv[2] }, m));
					mass += m;
				}
			}
			if (mass > 0.0f) {
				v[0] = Scl(sum, 1.0f / mass);
				have[0] = true;
			}
		}
		return fastest;
	}

	// Native: Skyrim's ragdoll is the body. After each physics step Delirium's parts are put where Skyrim's ragdoll is
	// (through the bones Skyrim just posed from it, down Delirium's own joint chain) and move as its bodies move; Delirium
	// then steps from there, and that step is where DriveRagdoll steers Skyrim's bodies next
	// Delirium's parts straight from Skyrim's ragdoll bodies: the exact inverse of BodyTarget (and of the riding bodies'
	// formula in DriveRagdoll), so Skyrim -> Delirium -> Skyrim lands every body where it was. The bones lag the bodies by a
	// physics step, so they only fill in what no body gives (a part's rotation when its body gives only the position, and
	// the parts with no body at all, down the chain from their parent)
	void Engine::PartsFromBodies(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot) const
	{
		PartsFromBones(a, pos, rot, nullptr);
		const V3             o = Scl({ a.origin.x, a.origin.y, a.origin.z }, kUnit);
		std::array<bool, 21> gotR{}, gotP{};
		std::array<V3, 21>   joint{};
		for (const auto& b : a.bodies) {
			const auto& ms = b.body->motion.motionState;
			const auto* cl = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMassLocal);
			const auto* cw = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMass1);
			const auto* qc = reinterpret_cast<const float*>(&ms.sweptTransform.rotation1);
			const Q     qb{ qc[0], qc[1], qc[2], qc[3] };
			const V3    pb = Sub({ cw[0], cw[1], cw[2] }, Rot(qb, { cl[0], cl[1], cl[2] }));
			// body = bone * (mapR, mapT)  =>  bone = body * (mapR, mapT)^-1
			Q   nr = Mul(qb, Conj(b.mapR));
			V3  np = Sub(pb, Rot(nr, Scl(b.mapT, a.scale)));
			int rp = -1, pp = -1;
			if (b.mapped) {
				rp = b.rotPart;
				pp = b.posPart;
			} else if (b.follow >= 0) {
				// its own bone rides on the follow bone by (followR, followT)
				const Q br = Mul(nr, Conj(b.followR));
				np = Sub(np, Rot(br, b.followT));
				nr = br;
				rp = pp = b.follow;
			} else {
				continue;
			}
			if (!gotR[rp]) {
				rot[rp] = Mul(nr, Conj(rig_[rp].offset));
				gotR[rp] = true;
			}
			if (!gotP[pp]) {
				joint[pp] = Sub(np, o);
				gotP[pp] = true;
			}
		}
		for (std::size_t i = 0; i < 21; ++i) {
			const V3 anchor = Scl(rig_[i].anchor, a.scale);
			if (gotP[i]) {
				pos[i] = Sub(joint[i], Rot(rot[i], anchor));
			} else if (kParent[i] >= 0) {
				const auto p = static_cast<std::size_t>(kParent[i]);
				pos[i] = Sub(Add(pos[p], Rot(rot[p], Scl(rig_[i].inParent, a.scale))), Rot(rot[i], anchor));
			}
		}
	}

	void Engine::SyncFromSkyrim(Active& a)
	{
		std::array<V3, 21> pos;
		std::array<Q, 21>  rot;
		PartsFromBodies(a, pos, rot);
		std::array<V3, 21>   v{}, w{};
		std::array<bool, 21> have{};
		BodyVelocities(a, v, w, have);
		for (int i = 1; i < 21; ++i) {
			if (!have[i] && kParent[i] >= 0) {
				v[i] = v[kParent[i]];
				w[i] = w[kParent[i]];
			}
		}
		for (int i = 0; i < 21; ++i) {
			if (!std::isfinite(pos[i].x + pos[i].y + pos[i].z + rot[i].x + rot[i].y + rot[i].z + rot[i].w + v[i].x + v[i].y + v[i].z + w[i].x + w[i].y + w[i].z)) {
				if (a.nanLog < 30) {
					++a.nanLog;
					logger::error("    NaN sync age {:.3f}: part {}; Delirium left as it is", a.age, i);
				}
				return;
			}
		}
		for (int i = 0; i < 21; ++i) {
			float p[3]{}, q[4]{};
			dl.nm_agent_get_part_transform(a.agent, i, p, q);
			const V3 d = Sub(pos[i], { p[0], p[1], p[2] });
			a.maxSync = std::max(a.maxSync, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
		}
		for (int i = 0; i < 21; ++i) {
			const float p[3]{ pos[i].x, pos[i].y, pos[i].z }, q[4]{ rot[i].x, rot[i].y, rot[i].z, rot[i].w };
			const float lv[3]{ v[i].x, v[i].y, v[i].z }, av[3]{ w[i].x, w[i].y, w[i].z };
			dl.nm_agent_set_part_state(a.agent, i, p, q, lv, av);
		}
		for (auto& b : a.bodies) {
			if (!b.mapped) {
				continue;
			}
			V3 pt;
			Q  qt;
			BodyTarget(a, b, pt, qt);
			const auto& ms = b.body->motion.motionState;
			const auto* cl = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMassLocal);
			const auto* cw = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMass1);
			const auto* qc = reinterpret_cast<const float*>(&ms.sweptTransform.rotation1);
			const V3    com = Add(pt, Rot(qt, { cl[0], cl[1], cl[2] }));
			const V3    d = Sub(com, { cw[0], cw[1], cw[2] });
			Q           dq = Mul(qt, Conj({ qc[0], qc[1], qc[2], qc[3] }));
			const float ang = 2.0f * std::acos(std::clamp(std::abs(dq.w), 0.0f, 1.0f));
			const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
			if (dist > a.maxRound) {
				a.maxRound = dist;
				a.roundBody = b.node ? b.node->name.c_str() : "?";
			}
			a.maxRoundAng = std::max(a.maxRoundAng, ang);
		}
		if (!a.roundLog.empty()) {
			logger::info("    sync {} age {:.3f}:{}", a.syncs, a.age, a.roundLog);
			a.roundLog.clear();
		}
		++a.syncs;
	}

	void Engine::InheritVelocity(Active& a)
	{
		std::array<V3, 21> v{}, w{};
		std::array<bool, 21> have{};
		const float fastest = BodyVelocities(a, v, w, have);
		int set = 0;
		for (int i = 0; i < 21; ++i) {
			if (!have[i] && i > 0 && kParent[i] >= 0) {
				v[i] = v[kParent[i]];
				w[i] = w[kParent[i]];
				have[i] = have[kParent[i]];
			}
			if (!have[i]) {
				continue;
			}
			float p[3]{}, q[4]{};
			dl.nm_agent_get_part_transform(a.agent, i, p, q);
			const float lv[3]{ v[i].x, v[i].y, v[i].z }, av[3]{ w[i].x, w[i].y, w[i].z };
			dl.nm_agent_set_part_state(a.agent, i, p, q, lv, av);
			++set;
		}
		logger::info("  took Skyrim's ragdoll velocities: {} of 21 parts, fastest body {:.2f} m/s", set, fastest);
		float cp[3]{}, cv[3]{};
		dl.nm_agent_get_com(a.agent, cp, cv);
		logger::info("  Delirium's body now moves at {:.2f} {:.2f} {:.2f} m/s", cv[0], cv[1], cv[2]);
		if (a.death) {
			// the killing blow as a shot at no health: Delirium's own hit reaction (stagger, clutch, collapse). From the
			// killer to the chest, else along the way the body already moves, else from the front
			float cp[3]{}, cq[4]{};
			dl.nm_agent_get_part_transform(a.agent, kChest, cp, cq);
			V3          dir{ cv[0], cv[1], 0.0f };
			const char* from = "its own motion";
			if (auto self = a.actor.get()) {
				if (auto killer = self->GetActorRuntimeData().myKiller.get()) {
					const RE::NiPoint3 k = (killer->GetPosition() - a.origin) * kUnit;
					dir = { cp[0] - k.x, cp[1] - k.y, 0.0f };
					from = "the killer";
				}
			}
			if (dir.x * dir.x + dir.y * dir.y < 1e-4f) {
				dir = { 0.0f, 0.0f, 0.0f };
				from = "nowhere (no motion, no killer)";
			} else {
				// up to 25 deg either way, so every corpse doesn't fall the same way
				const float t = Rand(-0.44f, 0.44f), cs = std::cos(t), sn = std::sin(t);
				dir = { dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs, 0.0f };
			}
			// hit somewhere on the upper body: spine1-3, neck or a clavicle
			constexpr std::array<int, 6> kHitParts{ 8, 9, 10, 11, 13, 17 };
			const int part = kHitParts[std::uniform_int_distribution<int>(0, 5)(g_rng)];
			float hp[3]{}, hq[4]{};
			dl.nm_agent_get_part_transform(a.agent, part, hp, hq);
			const float d[3]{ dir.x, dir.y, dir.z };
			if (dl.nm_agent_task_shot(a.agent, a.world, nullptr, part, hp, d, 0.0f) < 0) {
				logger::error("  shot task: {}", LastError());
			} else {
				logger::info("  dying: shot task at no health on part {}, hit from {}", part, from);
			}
			// then: a short writhe on the ground now and then, and limp after it (the arms come down from the shot's pose)
			a.writhe = Rand(0.0f, 1.0f) < 0.25f;
			a.relaxAt = a.age + Rand(0.6f, 1.5f);
			logger::info("  dying plan: {}limp after {:.1f} s", a.writhe ? "writhe, then " : "", a.relaxAt - a.age);
		}
	}

	void Engine::AfterAnimation(RE::Actor* a_actor, float a_delta)
	{
		if (takeOver_ && ready_ && !a_actor->IsPlayerRef()) {
			if (a_actor->IsInRagdollState()) {
				TryTakeOver(a_actor, a_actor->IsDead());
			} else if (g_skip.load(std::memory_order_relaxed) != 0) {
				ForgetSkip(a_actor);
			}
		}
		if (g_count.load(std::memory_order_relaxed) == 0) {
			return;
		}
		std::scoped_lock l(lock_);
		for (std::size_t i = 0; i < active_.size(); ++i) {
			auto& a = *active_[i];
			if (a.actor.get().get() != a_actor) {
				continue;
			}
			if (!a_actor->Get3D(false) || (a_actor->IsDead() && !a.death) || a_actor->IsDisabled()) {
				Release(i, "actor gone");
				return;
			}
			if (a.handedBack) {
				if (!a_actor->IsDead()) {
					Release(i, "came back to life");
				} else if (!a_actor->IsInRagdollState()) {
					Release(i, "no longer in Skyrim's ragdoll state");
				}
				return;
			}
			const float dt = std::clamp(a_delta, 0.0f, 0.1f);
			++a.calls;
			a.dtSum += a_delta;
			a.age += dt;
			a.sincePush += dt;
			// the bones were already posed from the last step, inside the animation update (AfterPose)
			// every frame advances by exactly its own time, split into equal steps of at most kStep: a fixed step with
			// an accumulator runs 0 steps one frame and 2 the next, and that stutter shakes CBPC/SMP physics
			if (a.handover < 0.0f && dt > 0.0f && TorqueMode(a)) {
				if (!a.following) {
					dl.nm_agent_set_mode(a.agent, NM_MODE_FOLLOW);
					a.following = true;
					MapParts(a);
				}
				SyncFromSkyrim(a);
				a.haveDrv = false;
				if (dl.nm_agent_step(a.agent, a.world, dt) < 0) {
					if (!a.logged) {
						logger::error("step (follow): {}", LastError());
						a.logged = true;
					}
				} else {
					a.haveDrv = dl.nm_agent_get_part_drives(a.agent, a.drv.data(), 21) == 21 &&
					            dl.nm_agent_get_joint_accels(a.agent, a.acc.data(), 21) == 21;
				}
			} else if (a.handover < 0.0f && dt > 0.0f) {
				if (a.following) {
					dl.nm_agent_set_mode(a.agent, NM_MODE_LEAD);
					a.following = false;
				}
				const int   steps = std::clamp(int(std::ceil(dt / kStep - 0.05f)), 1, kMaxSteps);
				const float h = dt / float(steps);
				for (int n = 0; n < steps; ++n) {
					if (dl.nm_agent_step(a.agent, a.world, h) < 0 && !a.logged) {
						logger::error("step: {}", LastError());
						a.logged = true;
					}
				}
			}
			if (collision_ && a.handover < 0.0f) {
				// Skyrim's collision was gathered around one spot: thrown far (Fus Ro Dah) she would fly through walls
				// and out of the map past its edge. Past 40% of the patch, gather a new one around her.
				float cp[3]{}, cv[3]{};
				dl.nm_agent_get_com(a.agent, cp, cv);
				const RE::NiPoint3 at = a.origin + RE::NiPoint3(cp[0], cp[1], cp[2]) / kUnit;
				const float        dx = at.x - a.colCenter.x, dy = at.y - a.colCenter.y;
				if (std::isfinite(at.x + at.y + at.z) &&
					(dx * dx + dy * dy > 0.16f * colRadius_ * colRadius_ || std::abs(at.z - a.colCenter.z) > 0.4f * colHeight_)) {
					if (auto* w = dl.nm_world_create()) {
						dl.nm_world_destroy(a.world);
						a.world = w;
						AddWorld(a, a_actor, at, true);
					}
				}
			}
			const bool  fallen = dl.nm_agent_has_fallen(a.agent) != 0;
			const bool  balancing = dl.nm_agent_is_balancing(a.agent) != 0;
			const float ke = dl.nm_agent_get_kinetic_energy(a.agent);
			if (static_cast<int>(a.age) != static_cast<int>(a.age - dt)) {
				float head[3]{}, q[4]{};
				dl.nm_agent_get_part_transform(a.agent, kHead, head, q);
				logger::info("  t {:.0} s: head {:.2} m, KE {:.1}, balancing {}, fallen {}; {} animation updates ({:.0} per s of their time), {} pose copies", a.age, head[2], ke,
					balancing, fallen, a.calls, a.dtSum > 0 ? a.calls / a.dtSum : 0.0f, a.poses);
				a.calls = 0;
				a.poses = 0;
				a.dtSum = 0;
			}
			a.wasBalancing |= balancing;
			if (a.death && a.calibrated && !a.relaxed && a.handover < 0.0f) {
				if (a.writhe && !a.writhing && (fallen || a.age >= a.relaxAt)) {
					a.writhing = true;
					dl.nm_msg_begin(a.agent, "stopAllBehaviours");
					dl.nm_msg_send(a.agent, a.world);
					const float stiff = Rand(9.0f, 12.0f), period = Rand(2.0f, 4.0f), amp = Rand(0.7f, 1.3f);
					dl.nm_msg_begin(a.agent, "bodyWrithe");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					for (const char* n : { "armStiffness", "legStiffness", "backStiffness" }) {
						dl.nm_msg_set_float(a.agent, n, stiff);
					}
					for (const char* n : { "armPeriod", "legPeriod", "backPeriod" }) {
						dl.nm_msg_set_float(a.agent, n, period * Rand(0.8f, 1.2f));
					}
					for (const char* n : { "armAmplitude", "legAmplitude", "backAmplitude", "kneeAmplitude", "elbowAmplitude" }) {
						dl.nm_msg_set_float(a.agent, n, amp * Rand(0.8f, 1.2f));
					}
					for (const char* n : { "armDamping", "legDamping", "backDamping" }) {
						dl.nm_msg_set_float(a.agent, n, 1.0f);
					}
					dl.nm_msg_send(a.agent, a.world);
					a.relaxAt = a.age + Rand(1.0f, 2.0f);
					logger::info("{:08X}: dying: writhe (stiffness {:.1f}, period {:.1f} s) for {:.1f} s", a_actor->GetFormID(), stiff, period, a.relaxAt - a.age);
				} else if ((!a.writhe || a.writhing) && a.age >= a.relaxAt) {
					a.relaxed = true;
					dl.nm_msg_begin(a.agent, "stopAllBehaviours");
					dl.nm_msg_send(a.agent, a.world);
					const float relax = Rand(60.0f, 100.0f);
					dl.nm_msg_begin(a.agent, "bodyRelax");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					dl.nm_msg_set_float(a.agent, "relaxation", relax);
					dl.nm_msg_set_float(a.agent, "damping", 1.0f);
					dl.nm_msg_send(a.agent, a.world);
					logger::info("{:08X}: dying: limp (relaxation {:.0f})", a_actor->GetFormID(), relax);
				}
			}
			if (!a.knockQueued && (fallen || (a.wasBalancing && !balancing))) {
				// balance lost: hand the actor to Skyrim's own ragdoll state now, while our pose is still close to the
				// animation Skyrim builds its ragdoll from (no push of its own); DriveRagdoll makes it follow Delirium
				a.knockQueued = true;
				a.native = native_;
				// the way she is falling (Delirium's head over its pelvis, flat): the knock's source goes behind her
				float hp[3]{}, pp[3]{}, hq[4]{};
				dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
				dl.nm_agent_get_part_transform(a.agent, 0, pp, hq);
				const RE::NiPoint3 lean(hp[0] - pp[0], hp[1] - pp[1], 0.0f);
				g_tasks->AddTask([h = a.actor, lean]() {
					if (auto act = h.get()) {
						if (auto* proc = act->GetActorRuntimeData().currentProcess) {
							proc->KnockExplosion(act.get(), KnockSource(act.get(), lean), 0.0f);
							logger::info("{:08X}: into Skyrim's ragdoll state", act->GetFormID());
						}
					}
				});
			} else if (!a.knockQueued) {
				FollowActor(a, a_actor);
			}
			if (a.knockQueued && static_cast<int>(a.age) != static_cast<int>(a.age - dt)) {
				logger::info("  ragdoll: knock state {}, in state {}, {} bodies, {} drives, fastest steer {:.1f} m/s, top node lag up to {:.2f} rad",
					static_cast<int>(a_actor->AsActorState()->GetKnockState()), a_actor->IsInRagdollState(), a.bodies.size(), a.drives, a.maxSpeed,
					a.maxRootLag);
				if (a.following) {
					logger::info("  muscles: joints asked for up to {:.2f} rad/s per step ({} at age {:.2f}; {} capped), bodies turned up to {:.2f} rad/s per step, {} spins capped (gain {:.2f})",
						a.maxWant, rig_[static_cast<std::size_t>(a.wantPart)].name, a.wantAge, a.clamped, a.maxDw, a.spinCapped, gain_);
					a.maxWant = 0;
					a.clamped = 0;
					a.spinCapped = 0;
					a.maxDw = 0;
				}
				if (a.native) {
					logger::info("  native: Skyrim's bodies up to {:.2f} m from Delirium's; {} syncs, Delirium moved up to {:.2f} m by them", a.maxGap, a.syncs,
						a.maxSync);
					a.maxGap = 0;
					logger::info("  round trip Skyrim -> Delirium -> Skyrim: bodies up to {:.3f} m ({}) and {:.1f} deg off", a.maxRound, a.roundBody, a.maxRoundAng * 57.2958f);
					a.maxRound = 0;
					a.maxRoundAng = 0;
					a.maxSync = 0;
					a.syncs = 0;
				}
				a.drives = 0;
				a.maxSpeed = 0;
				a.maxRootLag = 0;
			}
			// back to Skyrim's animation once it has caught itself and stood still
			a.calm = (balancing && !fallen && ke < 2.0f && a.sincePush > 1.0f && !a.knockQueued) ? a.calm + dt : 0.0f;
			// lying still in Skyrim's ragdoll: let go, Skyrim gets up from this pose on its own
			// (a corpse is held for good: it keeps moving in Delirium until it is really still, so the hold doesn't freeze a slide)
			a.settled = (a.calibrated && ke < (a.death ? 0.05f : 1.0f) && (!a.death || a.relaxed)) ? a.settled + dt : 0.0f;
			if (autoRelease_ && a.calm > 1.5f && !a.blendingOut) {
				a.blendingOut = true;  // fade back into Skyrim's animation over kBlendOut, physics still running
			}
			if (a.blendingOut) {
				a.blend -= dt / kBlendOut;
				if (a.blend <= 0.0f) {
					Release(i, "recovered");
					return;
				}
			}
			// One ragdoll: Delirium's. Once it lies still, the body is held there (puppet bodies at rest) until Skyrim's own
			// get-up starts, so Skyrim's limp ragdoll never runs; Skyrim's get-up then blends from exactly our pose.
			if (a.native && a.calibrated && autoRelease_ && a.settled > (a.death ? 1.0f : 0.5f)) {
				Release(i, a.death ? "settled: the corpse is Skyrim's ragdoll already" : "settled: Skyrim's ragdoll gets up by itself");
				return;
			}
			if (autoRelease_ && a.settled > (a.death ? 2.0f : 1.0f) && a.handover < 0.0f) {
				a.handover = 0.0f;
				if (a.death) {
					// a corpse goes back to Skyrim's own ragdoll (mass back, no more steering), so it can be dragged and hit
					// like any other. No fade: Skyrim's pose of that ragdoll sits 0.1-0.2 m off ours, so the next pose copy
					// measures the difference per bone and every copy after it carries it (AfterPoseImpl)
					a.handedBack = true;
					RestoreBodies(a);
					logger::info("{:08X}: settled; corpse handed to Skyrim's ragdoll, our pose kept on it", a_actor->GetFormID());
					int corpses = 0;
					for (auto& p : active_) {
						corpses += p->handedBack;
					}
					if (corpses > kMaxCorpses) {
						for (std::size_t j = 0; j < active_.size(); ++j) {
							if (j != i && active_[j]->handedBack) {
								Release(j, "too many corpses, the oldest goes back to Skyrim's pose");
								return;
							}
						}
					}
				} else {
					logger::info("{:08X}: settled; holding still for Skyrim's get-up", a_actor->GetFormID());
				}
			}
			if (a.handover >= 0.0f) {
				a.handover += dt;
			}
			// the frame Skyrim switches to its ragdoll has no pose copy at all (and we only learn that a frame later):
			// while knocked, put our pose on the bones here too, so a frame without a copy still draws ours. Held still
			// too: with only Skyrim's one copy a frame, its own pose (0.1-0.2 m off ours) showed through as a twitch
			if (a.knockQueued && !a.handedBack && !(a.native && a.calibrated)) {
				if (a.copies == 0 && a.handover < 0.0f) {
					logger::info("  age {:.3f}: the last frame had no pose copy", a.age);
				}
				AfterPose(a_actor, false);
			}
			a.copies = 0;
			// knocked down in Delirium but Skyrim's ragdoll never came, or is over (a save loaded, a get-up we missed): let go
			a.noRagdoll = a.knockQueued && !a_actor->IsInRagdollState() ? a.noRagdoll + dt : 0.0f;
			if (a.noRagdoll > 3.0f) {
				Release(i, "not in Skyrim's ragdoll state for 3 s");
				return;
			}
			if (a.calibrated) {
				const auto ks = a_actor->AsActorState()->GetKnockState();
				if (ks == RE::KNOCK_STATE_ENUM::kQueued || ks == RE::KNOCK_STATE_ENUM::kGetUp) {
					Release(i, a.handover >= 0.0f ? "Skyrim gets up" : "Skyrim started its get-up early");
				} else if (a.handover > 6.0f && !a.death) {
					Release(i, "no get-up from Skyrim after 6 s");
				}
			}
			return;
		}
	}

	bool Engine::Busy() { return g_count.load(std::memory_order_relaxed) != 0; }

	bool Engine::KeepsDown(RE::Actor* a_actor)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			if (p->actor.get().get() == a_actor) {
				// fallen and not yet settled: Delirium owns the body, Skyrim must not start its get-up
				return p->knockQueued && p->handover < 0.0f;
			}
		}
		return false;
	}

	void Engine::AfterPose(RE::Actor* a_actor, bool a_postPhysics)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			if (p->actor.get().get() == a_actor) {
				++p->copies;
			}
		}
		AfterPoseImpl(a_actor, a_postPhysics);
		// the skeleton's top node as it gets drawn: SmoothRoot turns it from here on the next pose copy
		for (auto& p : active_) {
			if (auto* top = a_actor->Get3D(false); top && p->actor.get().get() == a_actor) {
				if (!p->haveDrawnRoot || !p->knockQueued) {
					// upright the top node is Skyrim's; ragdolled SmoothRoot keeps this up to date
					p->drawnRootQ = FromMat(top->world.rotate);
					p->drawnRootT = top->world.translate;
					p->drawnAge = p->age;
					p->haveDrawnRoot = true;
				}
				// COM as this pose copy left it (ours or Skyrim's ragdoll pose): AfterRootMove holds it there
				p->comWorld = p->com->world;
				p->haveCom = p->knockQueued && !(p->native && p->calibrated);
			}
		}
	}

	// Skyrim's fresh bone locals into world transforms, for the bones Delirium reads only (and the chains above them, from
	// NPC COM's parent down). Walking the whole skeleton also rewrote the attached models (sword, shield, hair) outside
	// Skyrim's update, and their textures flickered.
	void Engine::RefreshBones(Active& a)
	{
		auto* top = a.com->parent;
		std::vector<RE::NiAVObject*> order;
		auto add = [&](RE::NiAVObject* n) {
			std::vector<RE::NiAVObject*> chain;
			for (; n && n != top; n = n->parent) {
				chain.push_back(n);
			}
			if (n != top) {
				return;  // not below NPC COM's parent
			}
			for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
				if (std::find(order.begin(), order.end(), *it) == order.end()) {
					order.push_back(*it);  // parents always come before their children
				}
			}
		};
		add(a.com);
		for (std::size_t i = 0; i < a.ref.size(); ++i) {
			add(a.ref[i]);
			add(a.ja[i]);
			add(a.jb[i]);
		}
		for (const auto& b : a.bodies) {
			add(b.node);
		}
		if (top->parent) {
			top->world = top->parent->world * top->local;
		}
		for (auto* n : order) {
			n->world = n->parent->world * n->local;
		}
	}

	void Engine::AfterPoseImpl(RE::Actor* a_actor, bool a_postPhysics)
	{
		for (auto& p : active_) {
			auto& a = *p;
			if (a.actor.get().get() != a_actor || !a_actor->Get3D(false)) {
				continue;
			}
			++a.poses;
			if (a.handedBack) {
				CarryPose(a, a_actor);
				return;
			}
			// Skyrim's animated pose in world space (the bone locals are fresh from the graph), then Delirium's bones over it
			RefreshBones(a);
			if (a_postPhysics && !a.calibrated && a.driver && a_actor->IsInRagdollState()) {
				// the bones were just posed from Skyrim's ragdoll bodies: where each body sits on its bone
				Calibrate(a, a_actor);
				if (a.takeover) {
					InheritVelocity(a);
				}
			}
			if (a.native && a.calibrated) {
				if (sync_ && !TorqueMode(a) && a_postPhysics && (a.takeover || a.age - a.calibAge >= 0.15f)) {
					SyncFromSkyrim(a);
				}
				return;
			}
			std::array<V3, 21> pos;
			std::array<Q, 21>  rot;
			if (followAnimation_ && !a.knockQueued) {
				PartsFromBones(a, pos, rot, nullptr);
			}
			if (a.knockQueued) {
				// SmoothRoot refreshes the top node's world even when it leaves it where Skyrim put it: the skeleton below
				// has to follow before WriteBones reads it (a stale NPC Root left the pelvis ~5 cm off)
				// (only the chain from the 3D root down to NPC Root: walking the whole 3D tree reaches gear whose nif root is
				// a plain NiNode and made it flicker again; WriteBones updates the skeleton below NPC COM itself)
				SmoothRoot(a, a_actor);
				auto*                          root3d = a_actor->Get3D(false);
				std::array<RE::NiAVObject*, 8> chain{};
				std::size_t                    n = 0;
				for (auto* c = a.com->parent; c && c != root3d && n < chain.size(); c = c->parent) {
					chain[n++] = c;
				}
				while (n > 0) {
					auto* c = chain[--n];
					if (c->parent) {
						c->world = c->parent->world * c->local;
					}
				}
			}
			WriteBones(a);
			// the bones as we just posed them: the bodies' targets until DriveRagdoll's next step sets its own
			for (auto& b : a.bodies) {
				if (a_postPhysics || !b.hasTarget) {
					b.target = Scl({ b.node->world.translate.x, b.node->world.translate.y, b.node->world.translate.z }, kUnit);
					b.targetRot = FromMat(b.node->world.rotate);
					b.hasTarget = true;
				}
			}
			if (followAnimation_ && !a.knockQueued) {
				// the posture to hold follows the live animation: a guard stays up, a talking NPC keeps gesturing
				// (not once ragdolled: then the "animation" is Skyrim's ragdoll, which is us)
				SetZeroPose(a, pos, rot);
			}
			return;
		}
	}

	// A handed-back corpse: the bone locals are fresh from Skyrim's ragdoll. The first copy measures how far ours are from
	// them (COM as a whole transform, the written bones' rotations); every copy then puts that difference back on, so the
	// corpse looks like Delirium left it and still moves with Skyrim's ragdoll when dragged or hit.
	void Engine::CarryPose(Active& a, RE::Actor* a_actor)
	{
		auto* root3d = a_actor->Get3D(false);
		auto  refreshChain = [&]() {
			std::array<RE::NiAVObject*, 8> chain{};
			std::size_t                    n = 0;
			for (auto* c = a.com->parent; c && c != root3d && n < chain.size(); c = c->parent) {
				chain[n++] = c;
			}
			while (n > 0) {
				auto* c = chain[--n];
				if (c->parent) {
					c->world = c->parent->world * c->local;
				}
			}
		};
		if (!a.haveCarry) {
			RefreshBones(a);
			refreshChain();
			const RE::NiTransform        skyCom = a.com->local;
			std::array<RE::NiMatrix3, 21> sky;
			for (std::size_t i = 0; i < 21; ++i) {
				if (rig_[i].write && a.ref[i]) {
					sky[i] = a.ref[i]->local.rotate;
				}
			}
			const RE::NiPoint3 skyPelvis = a.ref[0]->world.translate;
			WriteBones(a);
			a.carryCom = skyCom.Invert() * a.com->local;
			float worst = 0;
			for (std::size_t i = 0; i < 21; ++i) {
				if (rig_[i].write && a.ref[i]) {
					a.carryBone[i] = sky[i].Transpose() * a.ref[i]->local.rotate;
					const float tr = a.carryBone[i].entry[0][0] + a.carryBone[i].entry[1][1] + a.carryBone[i].entry[2][2];
					worst = std::max(worst, std::acos(std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f)));
				}
			}
			a.haveCarry = true;
			logger::info("{:08X}: carried pose measured: pelvis {:.1f} units off Skyrim's, bones up to {:.1f} deg", a_actor->GetFormID(),
				(a.ref[0]->world.translate - skyPelvis).Length(), worst * 57.2958f);
			return;
		}
		a.com->local = a.com->local * a.carryCom;
		for (std::size_t i = 0; i < 21; ++i) {
			if (rig_[i].write && a.ref[i]) {
				a.ref[i]->local.rotate = a.ref[i]->local.rotate * a.carryBone[i];
			}
		}
		refreshChain();
		UpdateTransforms(a.com->parent);
	}

	void Engine::Calibrate(Active& a, RE::Actor* a_actor)
	{
		// Skyrim hangs each ragdoll body on a skeleton bone's collision object
		std::unordered_map<const void*, RE::NiAVObject*> owner;
		std::function<void(RE::NiAVObject*)>             walk = [&](RE::NiAVObject* n) {
			if (auto* co = n->collisionObject.get()) {
				if (auto* bco = skyrim_cast<RE::bhkNiCollisionObject*>(co); bco && bco->body) {
					owner[bco->body->referencedObject.get()] = n;
				}
			}
			if (auto* node = n->AsNode()) {
				for (auto& c : node->GetChildren()) {
					if (c) {
						walk(c.get());
					}
				}
			}
		};
		walk(a_actor->Get3D(false));
		a.bodies.clear();
		std::string list;
		for (auto* body : a.driver->ragdoll->rigidBodies) {
			auto it = body ? owner.find(body) : owner.end();
			if (it == owner.end()) {
				continue;
			}
			BodyLink l{ body, it->second };
			std::memcpy(l.massInv.data(), &body->motion.inertiaAndMassInv, sizeof(l.massInv));
			l.gravity = body->motion.gravityFactor;
			a.bodies.push_back(l);
		}
		// Skyrim hangs each body exactly on its bone (havok units = game units * kUnit). Check it on distances between
		// bodies vs between their bones: those don't care that the root hasn't caught up with the ragdoll yet.
		float worst = 0;
		for (std::size_t i = 1; i < a.bodies.size(); ++i) {
			auto pos = [](const BodyLink& l) {
				const float* t = reinterpret_cast<const float*>(&l.body->motion.motionState.transform.translation);
				return V3{ t[0], t[1], t[2] };
			};
			auto bone = [](const BodyLink& l) { return Scl({ l.node->world.translate.x, l.node->world.translate.y, l.node->world.translate.z }, kUnit); };
			const V3    db = Sub(pos(a.bodies[i]), pos(a.bodies[0])), dn = Sub(bone(a.bodies[i]), bone(a.bodies[0]));
			const float lb = std::sqrt(db.x * db.x + db.y * db.y + db.z * db.z), ln = std::sqrt(dn.x * dn.x + dn.y * dn.y + dn.z * dn.z);
			worst = std::max(worst, std::abs(lb - ln));
			list += fmt::format(" {}", a.bodies[i].node->name.c_str());
		}
		list = fmt::format(" body-vs-bone spacing differs by at most {:.3} m;", worst) + list;
		// Skyrim's own mapping between ragdoll bodies and animation bones (the one its ragdoll uses to pose the skeleton)
		std::string mapLog;
		int         mappedCount = 0;
		{
			auto* ch = a.driver->character;
			auto* setup = ch ? ch->setup.get() : nullptr;
			auto* m = setup ? reinterpret_cast<const SkeletonMapper*>(setup->animationToRagdollSkeletonMapper.get()) : nullptr;
			auto* rd = a.driver->ragdoll;
			auto* rs = rd->skeleton.get();
			if (!m || !rs) {
				mapLog = " no animation-to-ragdoll mapper";
			} else {
				const bool animIsA = m->skeletonB == rs;
				const auto* anim = animIsA ? m->skeletonA : m->skeletonB;
				mapLog = fmt::format(" mapper: ragdoll skeleton is {} ({} simple mappings)", animIsA ? "B" : (m->skeletonA == rs ? "A" : "neither"), m->simpleCount);
				for (auto& l : a.bodies) {
					int k = -1;
					for (int i = 0; i < static_cast<int>(rd->rigidBodies.size()); ++i) {
						k = rd->rigidBodies[i] == l.body ? i : k;
					}
					int rb = -1;
					for (int i = 0; i < static_cast<int>(rd->boneToRigidBodyMap.size()); ++i) {
						rb = rd->boneToRigidBodyMap[i] == k ? i : rb;
					}
					for (int i = 0; rb >= 0 && i < m->simpleCount; ++i) {
						const auto& sm = m->simple[i];
						if ((animIsA ? sm.boneB : sm.boneA) != rb) {
							continue;
						}
						const int   ab = animIsA ? sm.boneA : sm.boneB;
						const char* an = anim->bones[ab].name.c_str();
						for (int j = 0; j < 21; ++j) {
							if (rig_[j].ref == an && rig_[j].jointA == an && rig_[j].jointB == an) {
								l.posPart = j;
							}
							if (rig_[j].ref == an && (rig_[j].write || l.rotPart < 0)) {
								l.rotPart = j;
							}
						}
						Q r{ sm.r[0], sm.r[1], sm.r[2], sm.r[3] };
						// Skyrim's mapper stores offsets in game units (hkbRagdollDriver maps the poses before the havok scale)
						V3 t = Scl({ sm.t[0], sm.t[1], sm.t[2] }, kUnit);
						if (!animIsA) {  // the mapping goes the other way: invert it
							r = Conj(r);
							t = Rot(r, Scl(t, -1.0f));
						}
						l.mapR = r;
						l.mapT = t;
						l.mapped = l.posPart >= 0 && l.rotPart >= 0;
						mappedCount += l.mapped;
						mapLog += fmt::format(" {}{}<-{}({:.3f} m {:.0f} deg)", l.mapped ? "" : "[no part] ", l.node->name.c_str(), an,
							std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z), Angle(r, Q{}) * 57.2958f);
						break;
					}
				}
			}
		}
		logger::info("{:08X}: {} of {} bodies mapped to Delirium parts;{}", a_actor->GetFormID(), mappedCount, a.bodies.size(), mapLog);
		// a body without a part still has to move: as a puppet (below) nothing else can. Skyrim takes the model's
		// placement from the ragdoll's root body (NPC COM), so a frozen one freezes the whole skeleton.
		{
			auto pose = [](const BodyLink& l, V3& p, Q& r) {
				const float* t = reinterpret_cast<const float*>(&l.body->motion.motionState.transform.translation);
				const float* q = reinterpret_cast<const float*>(&l.body->motion.motionState.sweptTransform.rotation1);
				p = { t[0], t[1], t[2] };
				r = { q[0], q[1], q[2], q[3] };
			};
			std::string fl;
			for (auto& l : a.bodies) {
				if (l.mapped) {
					continue;
				}
				V3 p;
				Q  r;
				pose(l, p, r);
				// the nearest bone that Delirium poses with its own part (NPC COM -> NPC Pelvis: they share a spot)
				float best = 1e9f;
				for (int j = 0; j < 21; ++j) {
					if (a.ref[j] && rig_[j].ref == rig_[j].jointA && rig_[j].ref == rig_[j].jointB) {
						const float d = (a.ref[j]->world.translate - l.node->world.translate).Length();
						if (d < best) {
							best = d;
							l.follow = j;
						}
					}
				}
				if (l.follow >= 0) {
					// bone in bone (both skeleton nodes, so the root not having caught up yet doesn't matter); the body
					// then sits on its bone through Skyrim's own mapping (mapR/mapT), as in BodyTarget
					const auto& n = a.ref[l.follow]->world;
					const V3    lp = Scl({ n.translate.x, n.translate.y, n.translate.z }, kUnit);
					const Q     lr = FromMat(n.rotate);
					p = Scl({ l.node->world.translate.x, l.node->world.translate.y, l.node->world.translate.z }, kUnit);
					r = FromMat(l.node->world.rotate);
					l.followR = Mul(Conj(lr), r);
					l.followT = Rot(Conj(lr), Sub(p, lp));
					fl += fmt::format(" {} on {} ({:.3f} m)", l.node->name.c_str(), a.ref[l.follow]->name.c_str(), best * kUnit);
				} else {
					fl += fmt::format(" {} on nothing", l.node->name.c_str());
				}
			}
			if (!fl.empty()) {
				logger::info("{:08X}: bodies without a part ride on a neighbour:{}", a_actor->GetFormID(), fl);
			}
		}
		// puppet: infinite mass (joints, ground and other bodies can't push it) and no gravity; DriveRagdoll moves it.
		// Native: Skyrim's ragdoll is the body (its own mass, gravity, collision), DriveRagdoll only steers it
		if (!a.native) {
			for (auto& l : a.bodies) {
				std::memset(&l.body->motion.inertiaAndMassInv, 0, sizeof(l.massInv));
				l.body->motion.gravityFactor = 0.0f;
			}
		}
		a.calibrated = true;
		a.calibAge = a.age;
		logger::info("{:08X}: Skyrim ragdoll has {} bodies, {} on bones:{}", a_actor->GetFormID(),
			a.driver->ragdoll->rigidBodies.size(), a.bodies.size(), list);
	}

	bool Engine::DeliriumBone(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const
	{
		if (!l.mapped) {
			return false;
		}
		float p[3]{}, q[4]{};
		dl.nm_agent_get_part_transform(a.agent, l.rotPart, p, q);
		a_rot = Mul({ q[0], q[1], q[2], q[3] }, rig_[l.rotPart].offset);
		dl.nm_agent_get_part_transform(a.agent, l.posPart, p, q);
		const V3 j = Add({ p[0], p[1], p[2] }, Rot({ q[0], q[1], q[2], q[3] }, Scl(rig_[l.posPart].anchor, a.scale)));
		a_pos = Add(Scl({ a.origin.x, a.origin.y, a.origin.z }, kUnit), j);
		return true;
	}

	bool Engine::TorqueMode(const Active& a) const
	{
		// F7: Delirium leads the first 0.15 s (the push is Delirium's), then follows
		return torque_ && a.native && a.calibrated && !a.handedBack && (a.takeover || a.age - a.calibAge >= 0.15f);
	}

	// which of Skyrim's bodies each Delirium part's drive lands on: the body it gives its rotation to, else its position,
	// else the body riding on it; parts with no body (clavicles) go to the nearest body up the chain
	void Engine::MapParts(Active& a)
	{
		a.partBody.fill(-1);
		for (int pass = 0; pass < 3; ++pass) {
			for (std::size_t k = 0; k < a.bodies.size(); ++k) {
				const auto& b = a.bodies[k];
				const int   p = pass == 0 ? (b.mapped ? b.rotPart : -1) : pass == 1 ? (b.mapped ? b.posPart : -1) : (b.mapped ? -1 : b.follow);
				if (p >= 0 && p < 21 && a.partBody[p] < 0) {
					a.partBody[p] = static_cast<int>(k);
				}
			}
		}
		for (int p = 0; p < 21; ++p) {
			int up = p;
			while (a.partBody[up] < 0 && kParent[up] >= 0) {
				up = kParent[up];
			}
			if (a.partBody[p] < 0) {
				a.partBody[p] = a.partBody[up];
			}
		}
		std::vector<float> dm(a.bodies.size(), 0.0f);
		for (int p = 0; p < 21; ++p) {
			float m = 0;
			if (a.partBody[p] >= 0 && dl.nm_build_part_info(build_, p, &m, nullptr, nullptr, nullptr) >= 0) {
				dm[static_cast<std::size_t>(a.partBody[p])] += m * a.scale * a.scale * a.scale;
			}
		}
		std::string s;
		float       dt = 0, ht = 0;
		for (std::size_t k = 0; k < a.bodies.size(); ++k) {
			const auto& b = a.bodies[k];
			const float hm = b.massInv[3] > 0 ? 1.0f / b.massInv[3] : 0.0f;
			dt += dm[k];
			ht += hm;
			s += fmt::format(" {} {:.1f}/{:.1f}", b.node ? b.node->name.c_str() : "?", dm[k], hm);
		}
		logger::info("  torque drive: Delirium follows Skyrim's ragdoll, its muscles move it; mass Delirium/Skyrim kg {:.1f}/{:.1f}:{}", dt, ht, s);
	}

	// Delirium's muscles on Skyrim's bodies for one physics step. Its torques are sized for its own whole-body inertia, so
	// on one light Skyrim body (a hand) they spin it up ten times too fast; what carries over is what they do to each
	// joint: the change of the joint's relative spin. That change is split between the joint's two bodies by their own
	// inertia (equal and opposite torque), so the joint turns as Delirium's would and no spin comes from nowhere.
	// Behaviour forces (lift) go on through the body's mass. Gravity, joints, ground and contacts stay Skyrim's.
	void Engine::ApplyTorques(Active& a, float a_dt)
	{
		if (!a.haveDrv) {
			return;
		}
		using M3 = std::array<float, 9>;
		auto mul = [](const M3& m, const V3& v) {
			return V3{ m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z };
		};
		auto len = [](const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
		const std::size_t n = a.bodies.size();
		std::vector<M3>   inv(n);
		for (std::size_t k = 0; k < n; ++k) {
			// world inverse inertia: R diag(i) R^T (havok keeps it diagonal in the body's frame)
			const auto& mo = a.bodies[k].body->motion;
			const auto* d = reinterpret_cast<const float*>(&mo.inertiaAndMassInv);
			const auto* qc = reinterpret_cast<const float*>(&mo.motionState.sweptTransform.rotation1);
			const Q     q{ qc[0], qc[1], qc[2], qc[3] };
			const V3    ax[3]{ Rot(q, { 1, 0, 0 }), Rot(q, { 0, 1, 0 }), Rot(q, { 0, 0, 1 }) };
			M3          m{};
			for (int r = 0; r < 3; ++r) {
				for (int col = 0; col < 3; ++col) {
					float s = 0;
					for (int e = 0; e < 3; ++e) {
						const float* v = &ax[e].x;
						s += v[r] * d[e] * v[col];
					}
					m[r * 3 + col] = s;
				}
			}
			inv[k] = m;
		}
		const float     h = std::clamp(a_dt, 0.0f, 0.05f) * gain_;
		std::vector<V3> dw(n), dv(n);
		for (int p = 1; p < 21; ++p) {
			const int c = a.partBody[p], pb = a.partBody[kParent[p]];
			const float* ac = a.acc.data() + p * 3;
			if (c < 0 || pb < 0 || c == pb || !std::isfinite(ac[0] + ac[1] + ac[2])) {
				continue;
			}
			V3          want = Scl({ ac[0], ac[1], ac[2] }, h);
			const float lw = len(want);
			if (lw > a.maxWant) {
				a.maxWant = lw;
				a.wantPart = p;
				a.wantAge = a.age;
			}
			if (lw > 30.0f) {
				want = Scl(want, 30.0f / lw);
				++a.clamped;
			}
			// torque t with (inv_c + inv_p) t = want: the child turns by inv_c t, the parent back by inv_p t
			M3 s{};
			for (int e = 0; e < 9; ++e) {
				s[e] = inv[c][e] + inv[pb][e];
			}
			const float det = s[0] * (s[4] * s[8] - s[5] * s[7]) - s[1] * (s[3] * s[8] - s[5] * s[6]) + s[2] * (s[3] * s[7] - s[4] * s[6]);
			if (!(std::abs(det) > 1e-12f)) {
				continue;
			}
			const M3 si{ (s[4] * s[8] - s[5] * s[7]) / det, (s[2] * s[7] - s[1] * s[8]) / det, (s[1] * s[5] - s[2] * s[4]) / det,
				(s[5] * s[6] - s[3] * s[8]) / det, (s[0] * s[8] - s[2] * s[6]) / det, (s[2] * s[3] - s[0] * s[5]) / det,
				(s[3] * s[7] - s[4] * s[6]) / det, (s[1] * s[6] - s[0] * s[7]) / det, (s[0] * s[4] - s[1] * s[3]) / det };
			const V3 t = mul(si, want);
			dw[c] = Add(dw[c], mul(inv[c], t));
			dw[pb] = Sub(dw[pb], mul(inv[pb], t));
		}
		for (int p = 0; p < 21; ++p) {
			const int    k = a.partBody[p];
			const float* d = a.drv.data() + p * 6;
			if (k >= 0 && std::isfinite(d[0] + d[1] + d[2])) {
				const float im = reinterpret_cast<const float*>(&a.bodies[k].body->motion.inertiaAndMassInv)[3];
				dv[k] = Add(dv[k], Scl({ d[0], d[1], d[2] }, im * h));
			}
		}
		for (std::size_t k = 0; k < n; ++k) {
			auto& b = a.bodies[k];
			auto& mo = b.body->motion;
			V3    v = dv[k];
			if (const float l = len(v); l > 3.0f) {
				v = Scl(v, 3.0f / l);
			}
			a.maxDw = std::max(a.maxDw, len(dw[k]));
			auto*    lv = reinterpret_cast<float*>(&mo.linearVelocity);
			auto*    av = reinterpret_cast<float*>(&mo.angularVelocity);
			const V3 nl{ lv[0] + v.x, lv[1] + v.y, lv[2] + v.z };
			V3       na{ av[0] + dw[k].x, av[1] + dw[k].y, av[2] + dw[k].z };
			// Delirium's own cap on a body's spin (3 turns/s, MAX_SPIN_NM): its muscles never spin a body past it. Spin
			// Skyrim's contacts gave the body is kept
			const float was = len({ av[0], av[1], av[2] }), now = len(na);
			if (now > kMaxSpin && now > was) {
				na = Scl(na, std::max(was, kMaxSpin) / now);
				++a.spinCapped;
			}
			if (!std::isfinite(nl.x + nl.y + nl.z + na.x + na.y + na.z)) {
				continue;
			}
			lv[0] = nl.x, lv[1] = nl.y, lv[2] = nl.z, lv[3] = 0;
			av[0] = na.x, av[1] = na.y, av[2] = na.z, av[3] = 0;
			b.lin = nl;
			b.ang = na;
			b.hasTarget = true;  // re-applied after Skyrim's own driver, as the steering is
			a.maxSpeed = std::max(a.maxSpeed, len(nl));
		}
	}

	void Engine::BodyTarget(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const
	{
		V3 bp;
		Q  br;
		DeliriumBone(a, l, bp, br);
		// body = bone * (mapR, mapT): checked in game against Skyrim's own pose from the bodies, right within 0.5 deg
		a_rot = Mul(br, l.mapR);
		a_pos = Add(bp, Rot(br, Scl(l.mapT, a.scale)));
	}

	void Engine::DriveRagdoll(RE::Actor* a_actor, RE::hkbRagdollDriver* a_driver, float a_dt, bool a_after)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			auto& a = *p;
			if (a.actor.get().get() != a_actor) {
				continue;
			}
			a.driver = a_driver;
			if (!a.calibrated || !a_driver->ragdoll || a.handedBack) {
				return;
			}
			if (!a_after && a.following && TorqueMode(a)) {
				ApplyTorques(a, a_dt);
				++a.drives;
				return;
			}
			if (a_after) {
				// Skyrim's own driver has run: our velocities go back on
				for (auto& b : a.bodies) {
					if (!b.hasTarget) {
						continue;
					}
					auto* lv = reinterpret_cast<float*>(&b.body->motion.linearVelocity);
					auto* av = reinterpret_cast<float*>(&b.body->motion.angularVelocity);
					lv[0] = b.lin.x, lv[1] = b.lin.y, lv[2] = b.lin.z, lv[3] = 0;
					av[0] = b.ang.x, av[1] = b.ang.y, av[2] = b.ang.z, av[3] = 0;
				}
				return;
			}
			// the targets come straight from Delirium's body through Skyrim's own body<->bone mapping: no bone, no root involved
			for (auto& b : a.bodies) {
				if (b.mapped) {
					BodyTarget(a, b, b.target, b.targetRot);
					b.hasTarget = true;
				}
			}
			for (auto& b : a.bodies) {
				if (b.follow >= 0) {
					BodyLink bone;
					bone.mapped = true;
					bone.posPart = bone.rotPart = b.follow;
					V3 bp;
					Q  br;
					DeliriumBone(a, bone, bp, br);
					// this body's own bone, then the body on it (BodyTarget's formula)
					const Q nr = Mul(br, b.followR);
					const V3 np = Add(bp, Rot(br, b.followT));
					b.targetRot = Mul(nr, b.mapR);
					b.target = Add(np, Rot(nr, Scl(b.mapT, a.scale)));
					b.hasTarget = true;
				}
			}
			// hard keyframe: the velocities that carry each body onto Delirium's pose in one step
			const float inv = 1.0f / std::max(a_dt, 1.0f / 240.0f);
			for (auto& b : a.bodies) {
				if (!b.hasTarget) {
					continue;
				}
				auto&       ms = b.body->motion.motionState;
				const Q     qt = b.targetRot;
				const V3    pt = b.target;
				const auto* cl = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMassLocal);
				const auto* cw = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMass1);
				const auto* qc = reinterpret_cast<const float*>(&ms.sweptTransform.rotation1);
				const V3    com = Add(pt, Rot(qt, { cl[0], cl[1], cl[2] }));
				const V3    lin = Scl(Sub(com, { cw[0], cw[1], cw[2] }), inv);
				Q           d = Mul(qt, Conj({ qc[0], qc[1], qc[2], qc[3] }));
				if (d.w < 0) {
					d = { -d.x, -d.y, -d.z, -d.w };
				}
				const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
				const float ang = 2.0f * std::atan2(s, d.w);
				const V3    w = s > 1e-6f ? Scl({ d.x, d.y, d.z }, ang / s * inv) : V3{};
				// capped: one bad frame must never launch the body (it then catches up over a few frames)
				auto cap = [](V3 v, float m) {
					const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
					return l > m ? Scl(v, m / l) : v;
				};
				// puppet bodies have nothing to fight, so they track exactly; the cap only guards against garbage
				V3 linC = cap(lin, 200.0f), wC = cap(w, 400.0f);
				auto finite = [](const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
				if (!finite(linC) || !finite(wC) || !finite(pt) || !std::isfinite(qt.w)) {
					// never hand a NaN to havok: the whole ragdoll (and the cloth, hair and gear hanging on it) breaks
					if (a.nanLog < 30) {
						++a.nanLog;
						logger::error("    NaN drive age {:.3f} {}: target ({} {} {}) rot w {}, body now ({} {} {}), steer ({} {} {}) turn ({} {} {})", a.age,
							b.node ? b.node->name.c_str() : "?", pt.x, pt.y, pt.z, qt.w, cw[0], cw[1], cw[2], lin.x, lin.y, lin.z, w.x, w.y, w.z);
					}
					linC = {};
					wC = {};
				}
				a.maxSpeed = std::max(a.maxSpeed, std::sqrt(lin.x * lin.x + lin.y * lin.y + lin.z * lin.z));
				auto*       lv = reinterpret_cast<float*>(&b.body->motion.linearVelocity);
				auto*       av = reinterpret_cast<float*>(&b.body->motion.angularVelocity);
				if (a.native) {
					// part of the way from the body's own velocity to the one that lands it on Delirium's pose: joints,
					// ground, other bodies and gravity still act on it
					const V3 gap = Sub(com, { cw[0], cw[1], cw[2] });
					a.maxGap = std::max(a.maxGap, std::sqrt(gap.x * gap.x + gap.y * gap.y + gap.z * gap.z));
					const float s = (!a.takeover && a.age - a.calibAge < 0.15f) ? 1.0f : strength_;
					const V3    cv{ lv[0], lv[1], lv[2] }, ca{ av[0], av[1], av[2] };
					if (finite(cv) && finite(ca)) {
						linC = Add(cv, Scl(Sub(linC, cv), s));
						wC = Add(ca, Scl(Sub(wC, ca), s));
					}
				}
				lv[0] = linC.x, lv[1] = linC.y, lv[2] = linC.z, lv[3] = 0;
				av[0] = wC.x, av[1] = wC.y, av[2] = wC.z, av[3] = 0;
				b.lin = linC;
				b.ang = wC;
			}
			++a.drives;
			return;
		}
	}

	void Engine::FollowActor(Active& a, RE::Actor* a_actor)
	{
		// upright: the actor (capsule, crosshair, AI) moves under the pelvis; the bones are placed relative to it anyway
		float p[3]{}, q[4]{};
		dl.nm_agent_get_part_transform(a.agent, 0, p, q);
		RE::NiPoint3 want = a.origin + RE::NiPoint3(p[0], p[1], 0) / kUnit;
		const auto   cur = a_actor->GetPosition();
		want.z = cur.z;
		// every frame, however small: a threshold moves her every other frame in 7-unit snaps, and bone physics
		// (CBPC, SMP) that works relative to the actor sees that as a shake
		if ((want - cur).Length() < 0.01f) {
			return;
		}
		g_tasks->AddTask([h = a.actor, want]() {
			if (auto act = h.get(); act && !act->IsInRagdollState()) {
				act->SetPosition(want, true);
			}
		});
	}

	bool Engine::SmoothRoot(Active& a, RE::Actor* a_actor)
	{
		// Going ragdoll, Skyrim puts the skeleton's top node onto the tumbling body: it turns 30 deg a frame and jumps
		// from her feet to her hips (60+ units) in one frame. Our bones hide it (they are posed in world space), but the
		// meshes hang on the top node and cloth physics watches its turn: either can show the jump for a frame. Move the
		// top node at most kRootRate / kRootSpeed; the bones still land where Delirium wants them, so the pose is unchanged.
		constexpr float kRootRate = 8.0f;     // rad/s, under FSMP's default limit (10)
		constexpr float kRootSpeed = 600.0f;  // game units/s beyond which the top node trails Skyrim's
		auto* top = a_actor->Get3D(false);
		if (!top || !a.haveDrawnRoot) {
			return false;
		}
		RE::NiUpdateData ud{};
		top->UpdateWorldData(&ud);  // Skyrim may just have set its local
		const float dt = std::max(a.age - a.drawnAge, 0.0f);
		const Q     sky = FromMat(top->world.rotate);
		const float gap = Angle(sky, a.drawnRootQ);
		const float step = kRootRate * dt;
		const RE::NiPoint3 skyT = top->world.translate;
		const float        dist = (skyT - a.drawnRootT).Length();
		const float        reach = kRootSpeed * dt;
		if (gap < 1e-4f && dist < 0.01f) {
			return false;  // still where we left it: nothing new from Skyrim (the lag stays what it was)
		}
		auto remember = [&]() {
			a.drawnRootQ = FromMat(top->world.rotate);
			a.drawnRootT = top->world.translate;
			a.drawnAge = a.age;
		};
		if (gap <= step && dist <= reach) {
			a.rootLag = 0.0f;
			remember();
			return false;
		}
		const RE::NiMatrix3 want = gap <= step ? top->world.rotate : ToMat(Slerp(a.drawnRootQ, sky, step / gap));
		const RE::NiPoint3  wantT = dist <= reach ? skyT : a.drawnRootT + (skyT - a.drawnRootT) * (reach / dist);
		if (top->parent) {
			const auto& pw = top->parent->world;
			top->local.rotate = pw.rotate.Transpose() * want;
			top->local.translate = (pw.rotate.Transpose() * (wantT - pw.translate)) / pw.scale;
		} else {
			top->local.rotate = want;
			top->local.translate = wantT;
		}
		a.rootLag = std::max(gap - step, 0.0f) + std::max(dist - reach, 0.0f) * 0.01f;  // ~1 deg per 1.75 units
		a.maxRootLag = std::max(a.maxRootLag, a.rootLag);
		top->UpdateWorldData(&ud);  // the top node only: the caller refreshes what hangs below it
		remember();
		return true;
	}

	void Engine::WriteBones(Active& a)
	{
		// the caller has brought the skeleton's world transforms (NPC Root down) up to date
		RE::NiUpdateData ud{};
		auto*            top = a.com->parent;

		// the whole body follows the pelvis: move COM (the pelvis and the spine both hang off it) by the pelvis's error
		{
			float p[3]{}, q[4]{};
			dl.nm_agent_get_part_transform(a.agent, 0, p, q);
			if (!std::isfinite(p[0] + p[1] + p[2] + q[0] + q[1] + q[2] + q[3])) {
				if (a.nanLog < 30) {
					++a.nanLog;
					logger::error("    NaN pelvis age {:.3f}: Delirium's pelvis ({} {} {}) rot ({} {} {} {}); nothing written", a.age, p[0], p[1], p[2], q[0], q[1], q[2], q[3]);
				}
				return;
			}
			const V3           j = Add({ p[0], p[1], p[2] }, Rot({ q[0], q[1], q[2], q[3] }, Scl(rig_[0].anchor, a.scale)));
			const RE::NiPoint3 want = a.origin + RE::NiPoint3(j.x, j.y, j.z) / kUnit;
			if (a.knockQueued) {
				// ragdolled, Skyrim turns the skeleton root back and forth every frame: COM takes its rotation from our
				// pelvis instead, so nothing below it (spine, arms, fingers, hair, armour) depends on the root's heading
				const RE::NiMatrix3 pelvis = ToMat(Mul({ q[0], q[1], q[2], q[3] }, rig_[0].offset));
				a.com->local.rotate = top->world.rotate.Transpose() * (pelvis * a.comInPelvis);
				UpdateTransforms(a.com);
			}
			const RE::NiPoint3 d = (want - a.ref[0]->world.translate) * a.blend;
			a.com->local.translate += (top->world.rotate.Transpose() * d) / top->world.scale;
			UpdateTransforms(a.com);
		}

		std::array<bool, 21> done{};
		for (std::size_t i = 0; i < 21; ++i) {
			if (!rig_[i].write) {
				continue;
			}
			auto* node = a.ref[i];
			if (!node->parent) {
				continue;
			}
			// refresh the in-between nodes (CME helpers) from the nearest bone already written down to this one's parent
			std::vector<RE::NiAVObject*> chain;
			for (auto* n = node->parent; n && n != a.com; n = n->parent) {
				bool written = false;
				for (std::size_t k = 0; k < 21; ++k) {
					written |= done[k] && a.ref[k] == n;
				}
				if (written) {
					break;
				}
				chain.push_back(n);
			}
			for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
				(*it)->UpdateWorldData(&ud);
			}
			float p[3]{}, q[4]{};
			dl.nm_agent_get_part_transform(a.agent, static_cast<std::int32_t>(i), p, q);
			if (!std::isfinite(q[0] + q[1] + q[2] + q[3]) || !std::isfinite(node->parent->world.rotate.entry[0][0])) {
				if (a.nanLog < 30) {
					++a.nanLog;
					logger::error("    NaN bone age {:.3f} {}: Delirium part {} rot ({} {} {} {}), parent rot {}", a.age, node->name.c_str(), i, q[0], q[1], q[2], q[3],
						node->parent->world.rotate.entry[0][0]);
				}
				continue;
			}
			const RE::NiMatrix3 want = ToMat(Mul({ q[0], q[1], q[2], q[3] }, rig_[i].offset));
			const RE::NiMatrix3 ours = node->parent->world.rotate.Transpose() * want;
			node->local.rotate = a.blend >= 1.0f ? ours : ToMat(Slerp(FromMat(node->local.rotate), FromMat(ours), std::max(a.blend, 0.0f)));
			node->UpdateWorldData(&ud);
			done[i] = true;
		}
		// everything hanging off the driven bones (twist bones, weapons, ...) follows
		UpdateTransforms(top);
	}
}
