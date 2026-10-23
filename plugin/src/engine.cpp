#include "engine.h"

#include "collision.h"

#include <RE/H/hkpLimitedHingeConstraintData.h>
#include <RE/H/hkpRagdollConstraintData.h>

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
		constexpr float kMaxExtTurn = 4.0f;              // the behaviours' own torques: most a body turns from them in a step (rad/s)
		// NM part order's parents (Buttocks is the root)
		constexpr std::array<int, 21> kParent{ -1, 0, 1, 2, 0, 4, 5, 0, 7, 8, 9, 10, 11, 10, 13, 14, 15, 10, 17, 18, 19 };
		constexpr int                 kChest = 10;
		constexpr int                 kHead = 12;
		constexpr float               kBlendOut = 0.4f;  // seconds from Delirium's pose back to the animation
		constexpr float               kCorpseHold = 30.0f;  // seconds a corpse is driven at most, still or not
		constexpr float               kRunawayKE = 50.0f;   // energy above which growth while down counts as running away
		constexpr int                 kRunawaySeconds = 5;  // seconds in a row of that before Skyrim's ragdoll takes over
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
	X(nm_agent_set_part_state) X(nm_agent_get_kinetic_energy) X(nm_agent_apply_push) X(nm_agent_add_velocity) X(nm_msg_begin)           \
	X(nm_msg_set_bool) X(nm_msg_set_int) X(nm_msg_set_float) X(nm_msg_set_string) X(nm_msg_set_vec3) X(nm_msg_send) X(nm_agent_is_balancing) X(nm_agent_has_fallen) X(nm_agent_set_zero_pose) \
	X(nm_build_scale) X(nm_build_destroy) X(nm_world_add_mesh) X(nm_world_raycast) X(nm_world_triangle_count) X(nm_agent_get_com) X(nm_agent_task_shot) X(nm_agent_task_balance) X(nm_agent_task_flinch) \
	X(nm_agent_get_part_drives) X(nm_build_part_info) X(nm_agent_get_joint_accels) X(nm_agent_get_part_ext_torques) X(nm_agent_get_joint) \
	X(nm_init_character_file) X(nm_build_from_character) X(nm_set_task_param)
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
		// Mu Joint Fix's height fix moves the body (its "MJF" nodes between COM and the pelvis) to keep standing feet out of
		// the ground, and turns itself off when it sees the pelvis touch it: on a falling body that throws the hips 20 cm
		// around and back. The ragdoll owns the hips while it falls, so the offset goes. True when one was there
		bool ClearJointFixOffset(RE::NiAVObject* a_pelvis, RE::NiAVObject* a_com)
		{
			bool had = false;
			for (RE::NiAVObject* n = a_pelvis ? a_pelvis->parent : nullptr; n && n != a_com; n = n->parent) {
				if (std::strncmp(n->name.c_str(), "MJF ", 4) == 0 && (n->local.translate.x != 0.0f || n->local.translate.y != 0.0f || n->local.translate.z != 0.0f)) {
					n->local.translate = {};
					had = true;
				}
			}
			return had;
		}
		void HookUpdate3D(RE::Actor* a_actor);  // below, with AfterRootMove
		void UnhookUpdate3D();
		void HookArrows();  // below, with OnArrow
		void UnhookArrows();
		// a_spell: 0 arrows only, 1 spells only, -1 any
		bool RecentImpact(RE::TESObjectREFR* a_ref, float a_maxAge, RE::NiPoint3& a_at, RE::NiPoint3& a_vel, int a_spell = -1);
		std::atomic<int> g_count{ 0 };  // active agents, read without the lock by every actor's animation update

		// Every actor's last hit, from wherever it is seen first (ProcessHitData for weapons, arrows, bashes and
		// explosions; the hit event for spells and shouts): who, from where, how hard. Reactions read it a frame later
		// (a stagger has no attacker for Fus or magic, an arrow's push scales by its damage).
		struct LastHit
		{
			std::chrono::steady_clock::time_point when;
			RE::ActorHandle                       from;
			RE::NiPoint3                          at, dir;  // game units; dir: the way it went (0 if not known)
			float                                 damage{};
			bool                                  exact{};  // at is where it hit
			std::uint32_t                         flags{};  // RE::HitData::Flag (0 for spells)
			float                                 strike{};  // a melee blow's push, N s (0: not one)
			RE::NiPoint3                          strikeDir;  // its way (unit)
			int                                   element{ -1 };  // a spell's (Element): its explosion's, which has none of its own
			bool                                  shout{};        // a shout's (a voice power's) hit
		};
		std::mutex                              g_hitLock;
		std::unordered_map<RE::FormID, LastHit> g_lastHit;

		void RecordHit(RE::FormID a_id, const LastHit& a_hit)
		{
			std::scoped_lock l(g_hitLock);
			auto&            h = g_lastHit[a_id];
			// the same hit seen twice (its HitData, then its event): the first one's point and direction stay
			if (a_hit.when - h.when < std::chrono::milliseconds(200) && h.exact && !a_hit.exact) {
				h.damage = std::max(h.damage, a_hit.damage);
				if (h.element < 0) {
					h.element = a_hit.element;
				}
				if (!h.from) {
					h.from = a_hit.from;
				}
				h.shout |= a_hit.shout;
				return;
			}
			h = a_hit;
			if (g_lastHit.size() > 256) {
				const auto now = std::chrono::steady_clock::now();
				std::erase_if(g_lastHit, [&](const auto& e) { return now - e.second.when > std::chrono::seconds(5); });
			}
		}
		bool RecentHit(RE::FormID a_id, float a_maxAge, LastHit& a_out)
		{
			std::scoped_lock l(g_hitLock);
			const auto       it = g_lastHit.find(a_id);
			if (it == g_lastHit.end() || std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second.when).count() > a_maxAge) {
				return false;
			}
			a_out = it->second;
			return true;
		}
		std::atomic<int> g_skip{ 0 };   // ragdolled NPCs left to Skyrim (skip_), same
		std::atomic<int> g_held{ 0 };   // deaths held back (held_), same
		constexpr std::array<int, 6> kUpperParts{ 8, 9, 10, 11, 13, 17 };  // spine1-3, neck, clavicles: where a killing blow lands
		constexpr float              kHeldHealth = 150.0f;  // the killing blow on her feet: the shot task's "stays up, fights it" health

		// The loading screen (a save loaded, a door): every NPC is let go. Their 3D is rebuilt after it, and an entry
		// left over kept a reloaded NPC on the ground (KeepsDown) with nothing moving her.
		struct LoadSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
					if (a_event->opening) {
						Engine::Get().ReleaseAll("loading screen");
					} else {
						Engine::Get().NoKillMoves();
					}
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
		// GTA IV's Fred.art from the player's own install (optional; sCharacterFile is the older name), else the mod's own
		fredPath_ = ini.GetValue("Data", "sGTAFile", ini.GetValue("Data", "sCharacterFile", ""));
		buildPath_ = ini.GetValue("Data", "sCharacter", "Data/SKSE/Plugins/SkyrimDelirium/skyrim_character.json");
		rigPath_ = ini.GetValue("Data", "sRig", "Data/SKSE/Plugins/SkyrimDelirium/skyrim_character_rig.txt");
		pushKey_ = static_cast<std::uint32_t>(ini.GetLongValue("Test", "iPushKey", 0x41));
		releaseKey_ = static_cast<std::uint32_t>(ini.GetLongValue("Test", "iReleaseKey", 0x3C));
		push_ = static_cast<float>(ini.GetDoubleValue("Test", "fPush", 80.0));
		autoRelease_ = ini.GetBoolValue("Test", "bAutoRelease", true);
		followAnimation_ = ini.GetBoolValue("Test", "bFollowAnimation", true);
		recordPoses_ = ini.GetBoolValue("Test", "bRecordPoses", false);
		standingLegs_ = ini.GetBoolValue("Test", "bStandingLegs", true);
		collision_ =ini.GetBoolValue("Collision", "bWorldCollision", true);
		colRadius_ = static_cast<float>(ini.GetDoubleValue("Collision", "fRadius", 600.0));
		colHeight_ = static_cast<float>(ini.GetDoubleValue("Collision", "fHeight", 300.0));
		colMaxTris_ = static_cast<std::size_t>(std::max(1000L, ini.GetLongValue("Collision", "iMaxTriangles", 40000)));
		takeOver_ = ini.GetBoolValue("Ragdoll", "bTakeOver", true);
		gain_ = static_cast<float>(ini.GetDoubleValue("Ragdoll", "fTorqueGain", 1.0));
		damp_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Ragdoll", "fJointDamping", 0.0)), 0.0f, 1.0f);
		friction_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Ragdoll", "fJointFriction", 0.0)), 0.0f, 1.0f);
		matchMass_ = ini.GetBoolValue("Ragdoll", "bMatchMass", true);
		matchContact_ = ini.GetBoolValue("Ragdoll", "bMatchContact", true);
		aboutJoint_ = ini.GetBoolValue("Ragdoll", "bTurnAboutJoint", true);
		noKillMoves_ = ini.GetBoolValue("Deaths", "bNoKillMoves", true);
		noKillCams_ = ini.GetBoolValue("Deaths", "bNoKillCams", true);
		dyingHealth_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fDyingHealth", 70.0)), 0.0f, 99.0f);
		limpMin_ = std::max(0.2f, static_cast<float>(ini.GetDoubleValue("Deaths", "fLimpAfterMin", 1.8)));
		limpMax_ = std::max(limpMin_, static_cast<float>(ini.GetDoubleValue("Deaths", "fLimpAfterMax", 3.0)));
		dyingWrithe_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fWritheChance", 0.5)), 0.0f, 1.0f);
		dyingCatch_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fCatchFallChance", 0.5)), 0.0f, 1.0f);
		deathPush_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fDeathPush", 1.5)), 0.0f, 6.0f);
		deathMaxSpeed_ = std::max(1.0f, static_cast<float>(ini.GetDoubleValue("Deaths", "fDeathMaxSpeed", 6.0)));
		dyingReact_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fShotTime", 1.2)), 0.0f, 5.0f);
		dyingTime_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fDyingTime", 3.0)), 0.0f, 10.0f);
		{
			// the reaction table: built in (today's reactions), each [Reactions] row replaces its own
			static constexpr std::pair<const char*, const char*> kTable[]{
			{ "stand", "step" },
			{ "stand.melee", "shot, spin, headsnap" },
			{ "stand.melee.light", "flinch" },
			{ "stand.arrow", "shot, spin, headsnap" },
			{ "stand.spell", "shot, spin, headsnap" },
			{ "stand.stagger.light", "flinch" },
			{ "stand.blast.light", "flinch" },
			{ "stand.lethal", "shot:150, spin, headsnap, collapse" },
			{ "fire", "burn" },
			{ "shock", "shock" },
			{ "frost", "" },
			{ "fly", "foetal:50, windmill:30, rollup:20" },
			{ "fly.far", "foetal:45, highfall:35, windmill:10, rollup:10" },
			{ "fly.fire", "foetal:75, windmill:25, thrash" },
			{ "downhit", "react, push:0.6" },
			{ "downhit.melee", "react, push:0.6, time:0.8" },
			{ "down.arrow", "writhe" },
			{ "down.magic", "writhe" },
			{ "down.fire", "roll, amp:2.2, period:0.4" },
			{ "down.shock", "writhe, amp:1.3, period:0.3, shock" },
			{ "down.frost", "foetal, shiver" },
			{ "fly.shout", "foetal:50, windmill:30, rollup:20" },
			{ "fly.shout.far", "highfall:60, foetal:25, windmill:15" }
			};
			rules_.clear();
			for (const auto& [k, v] : kTable) {
				rules_[k] = ParseRule(v);
			}
			heavyOomph_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Reactions", "fHeavyOomph", 0.5)), 0.0f, 1.01f);
			shoutThrowFrom_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Shouts", "fThrowFrom", 0.6)), 0.0f, 1.01f);
			shoutFly_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Shouts", "fFlySpeed", 9.0)));
			CSimpleIniA::TNamesDepend keys;
			ini.GetAllKeys("Reactions", keys);
			int own = 0;
			for (const auto& k : keys) {
				std::string key = k.pItem;
				std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (key.empty() || (k.pItem[0] == 'f' && std::isupper(static_cast<unsigned char>(k.pItem[1])))) {
					continue;  // a setting (fHeavyOomph), not a row
				}
				rules_[key] = ParseRule(ini.GetValue("Reactions", k.pItem, ""));
				++own;
			}
			static constexpr std::string_view kKnown[]{ "shot", "flinch", "step", "none", "spin", "headsnap", "collapse", "push", "burn", "shock", "chill", "catch",
				"foetal", "windmill", "rollup", "highfall", "thrash", "tumble", "react", "time", "writhe", "roll", "still", "amp", "period", "shiver" };
			for (const auto& [k, r] : rules_) {
				for (const auto& [n, v] : r.layers) {
					if (std::find(std::begin(kKnown), std::end(kKnown), n) == std::end(kKnown)) {
						logger::warn("[Reactions] {}: unknown behaviour '{}'", k, n);
					}
				}
			}
			logger::info("reaction table: {} rows ({} from the ini)", rules_.size(), own);
		}
		collapseMin_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Deaths", "fCollapseAfterMin", 0.8)), 0.0f, 10.0f);
		collapseMax_ = std::max(collapseMin_, static_cast<float>(ini.GetDoubleValue("Deaths", "fCollapseAfterMax", 1.6)));
		logger::info("muscle gain on Skyrim's ragdoll {:.2f}, joint damping {:.2f}, its joint friction x{:.2f} while driven, Delirium's masses {}", gain_, damp_,
			friction_, matchMass_);
		arrows_ = ini.GetBoolValue("Arrows", "bShotReaction", true);
		arrowPush_ = static_cast<float>(ini.GetDoubleValue("Arrows", "fPush", 55.0));
		spells_ = ini.GetBoolValue("Spells", "bShotReaction", true);
		burnTime_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Spells", "fBurnTime", 2.5)));
		shockTime_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Spells", "fShockTime", 2.0)));
		channelLinger_ = std::max(0.1f, static_cast<float>(ini.GetDoubleValue("Spells", "fChannelLinger", 0.6)));
		const auto chance = [&](const char* a_key, double a_or) { return std::clamp(static_cast<float>(ini.GetDoubleValue("Spells", a_key, a_or)), 0.0f, 1.0f); };
		spellChance_[0] = chance("fStartChance", 0.3);
		spellChance_[1] = chance("fStartChanceHurt", 0.5);
		channelChance_[0] = chance("fChannelChance", 0.1);
		channelChance_[1] = chance("fChannelChanceHurt", 0.2);
		logger::info("[Spells] start chance: missiles {:.0f}-{:.0f} %, channels {:.0f}-{:.0f} % a second (full health - near death)", spellChance_[0] * 100.0f,
			spellChance_[1] * 100.0f, channelChance_[0] * 100.0f, channelChance_[1] * 100.0f);
		staggers_ = ini.GetBoolValue("Stagger", "bReplace", true);
		staggerPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Stagger", "fPush", 90.0)));
		staggerMin_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Stagger", "fMinShare", 0.4)), 0.0f, 1.0f);
		damageRef_ = std::max(0.1f, static_cast<float>(ini.GetDoubleValue("Hits", "fDamageRef", 15.0)));
		damageExp_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Hits", "fExponent", 0.8)), 0.0f, 2.0f);
		damageMin_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fScaleMin", 0.4)));
		damageMax_ = std::max(damageMin_, static_cast<float>(ini.GetDoubleValue("Hits", "fScaleMax", 3.0)));
		staggerHitBoost_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Stagger", "fHitBoost", 0.5)));
		oomphFrom_ = static_cast<float>(ini.GetDoubleValue("Hits", "fOomphFrom", 1.0));
		oomphTo_ = std::max(oomphFrom_ + 0.01f, static_cast<float>(ini.GetDoubleValue("Hits", "fOomphTo", 2.5)));
		oomphSpin_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fOomphSpin", 2.0)));
		oomphLean_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fOomphLean", 0.9)));
		oomphPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fOomphPush", 1.0)));
		reelAt_ = static_cast<float>(ini.GetDoubleValue("Hits", "fReelAt", 0.3));
		fallAt_ = static_cast<float>(ini.GetDoubleValue("Hits", "fFallAt", 1.01));
		reelTime_ = std::max(0.1f, static_cast<float>(ini.GetDoubleValue("Hits", "fReelTime", 0.8)));
		reelTimeAdd_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fReelTimeAdd", 1.6)));
		logger::info("oomph: from x{:.2f} to x{:.2f} damage scale; twist x(1 + {:.1f}), lean 0.3 + {:.1f}, push x(1 + {:.1f}); reels from {:.2f} ({:.1f} + {:.1f} x oomph s), falls from {:.2f}",
			oomphFrom_, oomphTo_, oomphSpin_, oomphLean_, oomphPush_, reelAt_, reelTime_, reelTimeAdd_, fallAt_);
		melee_ = ini.GetBoolValue("Melee", "bReact", true);
		blasts_ = ini.GetBoolValue("Explosions", "bPush", true);
		blastPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fPush", 90.0)));
		blastFar_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Explosions", "fFarShare", 0.4)), 0.0f, 1.0f);
		blastUp_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Explosions", "fUp", 0.25)), 0.0f, 1.0f);
		flySpeed_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fFlySpeed", 3.0)));
		flyUp_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fFlyUp", 0.65)));
		flyMin_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fFlyMin", 2.0)));
		flyMax_ = std::max(flyMin_, static_cast<float>(ini.GetDoubleValue("Explosions", "fFlyMax", 9.0)));
		highFallAt_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fHighFallAt", 4.5)));
		tumble_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Explosions", "fTumble", 10.0)));
		writheTime_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Down", "fWritheTime", 1.0)));
		writheAdd_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Down", "fWritheAdd", 1.2)));
		foetalChance_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Down", "fFoetalChance", 0.3)), 0.0f, 1.0f);
		meleeOomphFrom_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fOomphFrom", 1.3)));
		meleeOomphTo_ = std::max(meleeOomphFrom_ + 0.1f, static_cast<float>(ini.GetDoubleValue("Melee", "fOomphTo", 4.0)));
		meleeKnockAt_ = static_cast<float>(ini.GetDoubleValue("Melee", "fKnockAt", 1.01));
		headWhip_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fHeadWhip", 0.2)));
		meleeMaxPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fMaxPush", 160.0)));
		hitMaxPush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Hits", "fMaxPush", 150.0)));
		pelvisShare_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Hits", "fPelvisShare", 0.0)), 0.0f, 1.0f);
		maxSteps_ = std::clamp(static_cast<int>(ini.GetLongValue("Hits", "iMaxSteps", 10)), 0, 100);
		logger::info("explosions: push {} {:.0f} N s at the centre, {:.0f}% at the edge, up {:.2f}; thrown at {:.1f} m/s (x damage scale x nearness), up {:.2f} per, from {:.1f} to {:.1f} m/s; high fall from {:.1f} m/s, tumble {:.0f} N s per m/s",
			blasts_, blastPush_, blastFar_ * 100.0f, blastUp_, flySpeed_, flyUp_, flyMin_, flyMax_, highFallAt_, tumble_);
		logger::info("down: writhes {:.1f} + {:.1f} x oomph s, curls up {:.0f}%; melee oomph from x{:.1f} to x{:.1f} a plain blow, knocked down from {:.2f}, head whip {:.2f}",
			writheTime_, writheAdd_, foetalChance_ * 100.0f, meleeOomphFrom_, meleeOomphTo_, meleeKnockAt_, headWhip_);
		meleePush_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fPush", 50.0)));
		meleePower_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fPower", 2.2)));
		meleeBash_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fBash", 1.6)));
		meleeSneak_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fSneak", 1.3)));
		meleeBlocked_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fBlocked", 0.5)));
		meleeWeightRef_ = std::max(0.1f, static_cast<float>(ini.GetDoubleValue("Melee", "fWeightRef", 9.0)));
		meleeHeftMin_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fHeftMin", 0.6)));
		meleeHeftMax_ = std::max(meleeHeftMin_, static_cast<float>(ini.GetDoubleValue("Melee", "fHeftMax", 1.7)));
		meleeSwing_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fSwing", 0.5)));
		meleeChop_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fChop", 0.35)));
		flinchBelow_ = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("Melee", "fFlinchBelow", 55.0)));
		logger::info("melee: {} push {:.0f} N s (power x{:.1f}, bash x{:.1f}, sneak x{:.1f}, blocked x{:.1f}), weight {:.0f} = x1 (x{:.1f}-{:.1f}), flinch under {:.0f}; stagger blow x(1 + {:.1f} x magnitude)",
			melee_, meleePush_, meleePower_, meleeBash_, meleeSneak_, meleeBlocked_, meleeWeightRef_, meleeHeftMin_, meleeHeftMax_, flinchBelow_, staggerHitBoost_);
		logger::info("staggers: played by Delirium {}, push {:.0f} N s at magnitude 1 ({:.0f}% at 0); hits push x{:.2f}-{:.2f}, x1 at {:.0f} damage, ^{:.2f}",
			staggers_, staggerPush_, staggerMin_ * 100.0f, damageMin_, damageMax_, damageRef_, damageExp_);
		logger::info("spells: shot reaction {}, burn {:.1f} s, shock {:.1f} s", spells_, burnTime_, shockTime_);
		wounded_ = std::clamp(static_cast<float>(ini.GetDoubleValue("Arrows", "fWoundedBelow", 0.2)), 0.0f, 1.0f);
		logger::info("arrows: shot reaction {}, push {:.0f} N s, wounded below {:.0f}% health", arrows_, arrowPush_, wounded_ * 100.0f);
		logger::info("settings: GTA IV file '{}', character '{}', push key {:#x}, release key {:#x}, push {} N s, follow animation {}, take over Skyrim's ragdolls {}",
			fredPath_, buildPath_, pushKey_, releaseKey_, push_, followAnimation_, takeOver_);
	}

	bool Engine::Init()
	{
		if (ready_) {
			return true;
		}
		if (!LoadApi()) {
			return false;
		}
		// the body (parts, masses, shapes) is always the mod's own character; its brain (joints, muscles, zero pose) is
		// GTA IV's Fred.art when the player points sGTAFile at their own copy and it exists, else the character's own
		std::ifstream cf(buildPath_, std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
		if (!cf) {
			logger::error("can't read character {}", buildPath_);
			return false;
		}
		std::error_code ec;
		const bool gta = !fredPath_.empty() && std::filesystem::is_regular_file(fredPath_, ec);
		if (!fredPath_.empty() && !gta) {
			logger::warn("GTA IV file {} not found: using the mod's own character", fredPath_);
		}
		if (gta ? dl.nm_init(fredPath_.c_str()) < 0 : dl.nm_init_character_file(buildPath_.c_str()) < 0) {
			logger::error("character {}: {}", gta ? fredPath_ : buildPath_, LastError());
			return false;
		}
		usingGta_ = gta;
		dl.nm_set_task_param("NMShot_Standing", "*", "shotMaxSteps", static_cast<float>(maxSteps_));
		build_ =dl.nm_build_from_character(text.c_str());
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
		HookArrows();
		NoKillMoves();
		logger::info("ready: {} on body {}, rig {} (build faces {:.2} rad)", usingGta_ ? fredPath_ : std::string("own character"), buildPath_, rigPath_, rigYaw_);
		return true;
	}

	void Engine::NoKillMoves()
	{
		// Skyrim.esm's KillMove global (the engine checks it before any paired kill), the kill cam's odds, and the
		// VATS-style kill camera. Saved games keep the global: `set killmove to 1` in the console brings them back
		std::string done;
		if (noKillMoves_) {
			if (auto* g = RE::TESForm::LookupByID<RE::TESGlobal>(0x100F19)) {
				done += fmt::format(" KillMove {:.0f} -> 0;", g->value);
				g->value = 0.0f;
			} else {
				done += " KillMove global not found;";
			}
		}
		if (noKillCams_) {
			if (auto* gs = RE::GameSettingCollection::GetSingleton(); gs) {
				if (auto* st = gs->GetSetting("fKillCamBaseOdds")) {
					done += fmt::format(" fKillCamBaseOdds {:.2f} -> 0;", st->data.f);
					st->data.f = 0.0f;
				} else {
					done += " fKillCamBaseOdds not found;";
				}
			}
			if (auto* is = RE::INISettingCollection::GetSingleton(); is) {
				if (auto* st = is->GetSetting("bVATSDisable:VATS")) {
					done += fmt::format(" bVATSDisable {} -> 1;", st->data.b);
					st->data.b = true;
				} else {
					done += " bVATSDisable not found;";
				}
			}
		}
		if (!done.empty()) {
			logger::info("deaths are Delirium's:{}", done);
		}
	}

	bool Engine::OnKill(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent, bool a_ragdollInstant)
	{
		if (!a_actor || a_actor == letKill_.load() || dyingTime_ <= 0.0f || !ready_ || a_actor->IsPlayerRef()) {
			return false;
		}
		std::scoped_lock l(lock_);
		const auto id = a_actor->GetFormID();
		const auto it = held_.find(id);
		if (it == held_.end()) {
			// people on their feet only: one already in a ragdoll (knocked down, paralysed) dies as she lies
			if (!Eligible(a_actor, "killing blow", false)) {
				return false;
			}
			if (a_actor->IsInRagdollState() || a_actor->IsInKillMove()) {
				logger::info("{:08X}: killing blow in a ragdoll or killmove: dies at once", id);
				return false;
			}
		}
		// she lives on at 1 health (blows during it hit the hold, not her life)
		auto*       av = a_actor->AsActorValueOwner();
		const float hp = av->GetActorValue(RE::ActorValue::kHealth);
		if (hp < 1.0f) {
			av->RestoreActorValue(RE::ActorValue::kHealth, 1.0f - hp);
		}
		if (it != held_.end()) {
			if (a_attacker) {
				it->second.attacker = a_attacker->GetHandle();
			}
			return true;
		}
		held_[id] = Held{ a_actor->GetHandle(), a_attacker ? a_attacker->GetHandle() : RE::ActorHandle{}, a_damage, a_sendEvent, a_ragdollInstant,
			std::chrono::steady_clock::now() };
		g_held = static_cast<int>(held_.size());
		logger::info("{:08X}: killing blow ({:.0f} damage, health {:.0f} of {:.0f}, by {:08X}{}): dying on her feet, up to {:.1f} s", id, a_damage, hp,
			av->GetPermanentActorValue(RE::ActorValue::kHealth), a_attacker ? a_attacker->GetFormID() : 0, a_ragdollInstant ? ", instant ragdoll" : "",
			dyingTime_);
		g_tasks->AddTask([h = a_actor->GetHandle()]() {
			if (auto t = h.get()) {
				Engine::Get().DyingOnFeet(t.get());
			}
		});
		return true;
	}

	void Engine::DyingOnFeet(RE::Actor* a_actor)
	{
		std::scoped_lock l(lock_);
		const auto it = held_.find(a_actor->GetFormID());
		if (it == held_.end() || a_actor->IsDead()) {
			return;
		}
		Active* act = nullptr;
		for (auto& p : active_) {
			if (p->actor.get().get() == a_actor) {
				act = p.get();
			}
		}
		if (!act) {
			if (!Activate(a_actor, false)) {
				logger::info("{:08X}: dying on her feet: Delirium can't take her, dies now", a_actor->GetFormID());
				it->second.at -= std::chrono::seconds(60);
				return;
			}
			act = active_.back().get();
		}
		// her hit's own reaction already plays it (React, wounded at the dying health), or she is down already
		if (act->heldDeath || act->knockQueued || act->death) {
			act->heldDeath = true;
			return;
		}
		if (act->flying > 0) {
			act->heldDeath = true;
			logger::info("{:08X}: dying on her feet: thrown, the throw plays on, she dies once down", a_actor->GetFormID());
			return;
		}
		act->heldDeath = true;
		// no reaction from a blow (a burn, a fall, the console): the killing blow as a wounded shot, from the killer
		float cp[3]{}, cq[4]{};
		dl.nm_agent_get_part_transform(act->agent, kChest, cp, cq);
		V3 dir{};
		if (auto k = it->second.attacker.get()) {
			const RE::NiPoint3 kp = (k->GetPosition() - act->origin) * kUnit;
			dir = { cp[0] - kp.x, cp[1] - kp.y, 0.0f };
		}
		if (dir.x * dir.x + dir.y * dir.y < 1e-4f) {
			const float h = a_actor->GetAngleZ();
			dir = { -std::sin(h), -std::cos(h), 0.0f };  // from the front
		}
		const float ln = std::sqrt(dir.x * dir.x + dir.y * dir.y);
		const float d[3]{ dir.x / ln, dir.y / ln, 0.0f };
		const int   part = kUpperParts[std::uniform_int_distribution<int>(0, 5)(g_rng)];
		float       hp[3]{}, hq[4]{};
		dl.nm_agent_get_part_transform(act->agent, part, hp, hq);
		std::string key;
		const Rule* row = FindRule({ "stand.lethal" }, key);
		const bool  collapse = !row || row->Has("collapse");
		if (dl.nm_agent_task_shot(act->agent, act->world, nullptr, part, hp, d, row ? row->Value("shot", kHeldHealth) : kHeldHealth) < 0) {
			logger::error("  shot task: {}", LastError());
		} else {
			act->collapseAt = collapse ? act->age + Rand(collapseMin_, collapseMax_) : -1.0f;
			logger::info("{:08X}: dying on her feet: shot task on part {}, collapsing in {:.1f} s", a_actor->GetFormID(), part, act->collapseAt - act->age);
		}
	}

	void Engine::CheckHeld(RE::Actor* a_actor)
	{
		Held h;
		{
			std::scoped_lock l(lock_);
			const auto it = held_.find(a_actor->GetFormID());
			if (it == held_.end()) {
				return;
			}
			const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second.at).count();
			Active*     act = nullptr;
			for (auto& p : active_) {
				if (p->actor.get().get() == a_actor) {
					act = p.get();
				}
			}
			const bool  down = act && act->knockQueued;
			const bool  reacting = down && (act->writheAt >= 0.0f || act->writheUntil > act->age);  // writhing, convulsing, burning on the ground
			const char* why = t >= dyingTime_ ? "her time is up" :
			                  down && !reacting && t >= 1.5f ? "she is down" :
			                  !act && t >= 0.5f      ? "Delirium let her go" :
			                                           nullptr;
			if (!why) {
				return;
			}
			h = it->second;
			held_.erase(it);
			g_held = static_cast<int>(held_.size());
			h.why = why;
		}
		g_tasks->AddTask([h]() { Engine::Get().KillNow(h, h.why); });
	}

	void Engine::KillHeld(const char* a_why)
	{
		std::vector<Held> all;
		{
			std::scoped_lock l(lock_);
			for (auto& [id, h] : held_) {
				all.push_back(h);
			}
			held_.clear();
			g_held = 0;
		}
		for (const auto& h : all) {
			KillNow(h, a_why);
		}
	}

	void Engine::KillNow(const Held& a_held, const char* a_why)
	{
		auto t = a_held.actor.get();
		if (!t || t->IsDead()) {
			return;
		}
		auto att = a_held.attacker.get();
		{
			std::scoped_lock l(lock_);
			heldDied_.insert(t->GetFormID());
		}
		const float s = std::chrono::duration<float>(std::chrono::steady_clock::now() - a_held.at).count();
		letKill_ = t.get();
		t->KillImpl(att.get(), a_held.damage, a_held.send, a_held.instant);
		letKill_ = nullptr;
		logger::info("{:08X}: dies now ({}), {:.1f} s after the killing blow{}", t->GetFormID(), a_why, s, t->IsDead() ? "" : " (still alive?)");
	}

	void Engine::ReleaseAll(const char* a_why)
	{
		KillHeld(a_why);
		std::scoped_lock l(lock_);
		heldDied_.clear();
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
		UnhookArrows();
		KillHeld("core unloading");
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
	//   shot <hex formid> [N s] [hurt] [fire|frost|shock]  an arrow (or spell) from the player into that NPC's chest (the reaction only, no damage)
	//   reset <hex formid> [dist] stand that NPC in front of the player, facing him
	//   spells                   give the player test spells: Flames, Frostbite, Sparks (channels), Firebolt, Fireball,
	//                            Ice Spike, Icy Spear, Ice Storm, Lightning Bolt, Chain Lightning
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
			} else if (w[0] == "spells") {
				auto* pc = RE::PlayerCharacter::GetSingleton();
				for (const RE::FormID id : { 0x012FCDu, 0x02B96Bu, 0x02DD2Au, 0x012FD0u, 0x01C789u, 0x02B96Cu, 0x10F7ECu, 0x045F9Cu, 0x02DD29u, 0x045F9Du }) {
					if (auto* s = RE::TESForm::LookupByID<RE::SpellItem>(id)) {
						pc->AddSpell(s);
						logger::info("  spell {:08X} '{}'", id, s->GetName());
					} else {
						logger::warn("  no spell {:08X}", id);
					}
				}
			} else if (w[0] == "shot" && w.size() >= 2) {
				if (auto* a = actor(1)) {
					const float keep = arrowPush_, keepWounded = wounded_;
					if (w.size() >= 3) {
						arrowPush_ = std::stof(w[2]);
					}
					int kind = kArrow;
					for (std::size_t k = 3; k < w.size(); ++k) {
						if (w[k] == "hurt") {
							wounded_ = 1.01f;  // as if under the wounded threshold
						} else if (w[k] == "fire") {
							kind = kFire;
						} else if (w[k] == "frost") {
							kind = kFrost;
						} else if (w[k] == "shock") {
							kind = kShock;
						}
					}
					RE::NiPoint3 at = a->GetPosition();
					at.z += 90.0f;  // chest height
					RE::NiPoint3 from = RE::PlayerCharacter::GetSingleton()->GetPosition();
					from.z += 110.0f;
					const RE::NiPoint3 v = at - from;
					const float        l = v.Length();
					OnHit(a, at, l > 1.0f ? v * (3000.0f / l) : RE::NiPoint3(0.0f, 3000.0f, 0.0f), true, kind, false, true);
					arrowPush_ = keep;
					wounded_ = keepWounded;
				}
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
						if (auto act = h.get(); act && player && act->Get3D(false) && !act->IsDisabled()) {
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
			} else if (w[0] == "gain" && w.size() >= 2) {
				gain_ = std::clamp(std::stof(std::string(w[1])), 0.0f, 10.0f);
				logger::info("torque gain {:.2f}", gain_);
			} else if (w[0] == "damp" && w.size() >= 2) {
				damp_ = std::clamp(std::stof(std::string(w[1])), 0.0f, 1.0f);
				logger::info("joint damping {:.2f}", damp_);
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
					logger::info("  {:08X}: age {:.1f} s, knocked {}, in ragdoll {}", act ? act->GetFormID() : 0, a->age, a->knockQueued,
						act && act->IsInRagdollState());
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
		{
			// the agent starts in its build's standing stance: kept, each part in its parent's frame, for StandingLegs
			std::array<V3, 21> rp;
			std::array<Q, 21>  rq;
			for (int i = 0; i < 21; ++i) {
				float p[3]{}, q[4]{ 0, 0, 0, 1 };
				dl.nm_agent_get_part_transform(act->agent, i, p, q);
				rp[i] = { p[0], p[1], p[2] };
				rq[i] = { q[0], q[1], q[2], q[3] };
			}
			for (std::size_t i = 1; i < 21; ++i) {
				const auto pa = static_cast<std::size_t>(kParent[i]);
				act->restRot[i] = Mul(Conj(rq[pa]), rq[i]);
				act->restPos[i] = Rot(Conj(rq[pa]), Sub(rp[i], rp[pa]));
			}
		}
		dl.nm_agent_set_mode(act->agent, NM_MODE_LEAD);
		for (int i = 0; i < 21; ++i) {
			const float p[3]{ pos[i].x, pos[i].y, pos[i].z }, q[4]{ rot[i].x, rot[i].y, rot[i].z, rot[i].w };
			dl.nm_agent_set_part_state(act->agent, i, p, q, zero, zero);
		}
		// the posture the muscles and balance hold: the NPC's own pose, not the GTA character's stance (standing: her
		// legs from the body's own stance, see StandingLegs)
		{
			auto zp = pos;
			auto zr = rot;
			if (!a_takeover) {
				StandingLegs(*act, zp, zr);
			}
			SetZeroPose(*act, zp, zr);
		}
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

	// [Test] bRecordPoses: the pose she holds each frame while standing (her live animation: stance, staggers, hit
	// reactions), one line per frame (age, then each part's position and rotation xyzw relative to where she was taken),
	// first 4 NPCs a session, 10 s each. fit-test sweep STANCE_TRACK= replays it as the zero pose
	void Engine::RecordPose(Active& a, RE::Actor* a_actor, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot)
	{
		if (!recordPoses_ || a.trackN >= 600) {
			return;
		}
		if (a.trackFile.empty()) {
			if (tracks_ >= 4) {
				return;
			}
			++tracks_;  // numbered: a summoned NPC's FormID is reused by the next one
			a.trackFile = (*SKSE::log::log_directory() / fmt::format("SkyrimDelirium_track_{}_{:08X}.txt", tracks_, a_actor->GetFormID())).string();
			std::ofstream(a.trackFile) << "heading " << a_actor->data.angle.z << " scale " << a.scale << "\n";
			logger::info("{:08X}: recording her held poses to {}", a_actor->GetFormID(), a.trackFile);
		}
		std::ostringstream f;
		f << a.age;
		for (std::size_t i = 0; i < 21; ++i) {
			f << " " << pos[i].x << " " << pos[i].y << " " << pos[i].z << " " << rot[i].x << " " << rot[i].y << " " << rot[i].z << " " << rot[i].w;
		}
		std::ofstream(a.trackFile, std::ios::app) << f.str() << "\n";
		++a.trackN;
	}

	// [Test] bStandingLegs: while she stands the hips, knees and ankles hold the body's own stance, soles level, under
	// the animation's pelvis; the spine, arms and head keep the animation. Skyrim's combat animations lunge, step and
	// tiptoe with planted feet, and holding that pose tipped her over with no hit at all (fit-test STANCE_TRACK on the
	// recorded tracks: 100% falls, 0% with these legs)
	void Engine::StandingLegs(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot) const
	{
		if (!standingLegs_) {
			return;
		}
		for (std::size_t i = 1; i <= 6; ++i) {
			const auto p = static_cast<std::size_t>(kParent[i]);
			rot[i] = Mul(rot[p], a.restRot[i]);
			pos[i] = Add(pos[p], Rot(rot[p], a.restPos[i]));
		}
		for (std::size_t i : { std::size_t{ 3 }, std::size_t{ 6 } }) {
			// a foot's local y is up when it stands flat: turn it the shortest way back to world up
			const V3    u = Rot(rot[i], { 0, 1, 0 });
			const Q     t{ u.y, -u.x, 0.0f, 1.0f + u.z };  // half-way quaternion from u to +z (u x z, 1 + u.z)
			const float n = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z + t.w * t.w);
			if (n > 1e-4f) {
				rot[i] = Mul({ t.x / n, t.y / n, t.z / n, t.w / n }, rot[i]);
			}
		}
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

	namespace
	{
		constexpr std::array<const char*, 5> kHitNames{ "arrow", "frost spell", "fire spell", "shock spell", "spell" };
	}

	bool Engine::Eligible(RE::Actor* a_target, const char* a_what, bool a_quiet) const
	{
		if (!ready_ || !a_target || a_target->IsPlayerRef() || a_target->IsDead() || a_target->IsDisabled() || !a_target->Get3D(false)) {
			if (!a_quiet) {
				logger::info("{}: skipped (off, dead, disabled or no 3D)", a_what);
			}
			return false;
		}
		// people only (the rig is a human's), not riding or sitting (the body would pull her off)
		auto* race = a_target->GetRace();
		if (!race || !race->HasKeywordString("ActorTypeNPC") || a_target->IsOnMount() ||
			a_target->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
			if (!a_quiet) {
				logger::info("{}: skipped {:08X} (not a person, riding or sitting)", a_what, a_target->GetFormID());
			}
			return false;
		}
		return true;
	}

	float Engine::StartChance(RE::Actor* a_target, const float (&a_chance)[2]) const
	{
		auto*       av = a_target->AsActorValueOwner();
		const float full = av->GetPermanentActorValue(RE::ActorValue::kHealth);
		const float share = full > 0.0f ? std::clamp(av->GetActorValue(RE::ActorValue::kHealth) / full, 0.0f, 1.0f) : 1.0f;
		return a_chance[0] + (a_chance[1] - a_chance[0]) * (1.0f - share);
	}

	void Engine::OnHit(RE::Actor* a_target, const RE::NiPoint3& a_at, const RE::NiPoint3& a_vel, bool a_exact, int a_kind, bool a_channel, bool a_sure)
	{
		const char* what = kHitNames[static_cast<std::size_t>(std::clamp(a_kind, 0, 4))];
		if (!(a_kind == kArrow ? arrows_ : spells_)) {
			return;
		}
		if (a_channel) {
			// a held spell: no blow, only its element while it keeps hitting
			if (a_kind == kMagic || !Eligible(a_target, what, true)) {
				return;
			}
			std::scoped_lock l(lock_);
			for (auto& a : active_) {
				if (a->actor.get().get() == a_target) {
					if (!a->death) {
						Channel(*a, a_target, a_vel, a_kind);
					}
					return;
				}
			}
			if (a_target->IsInRagdollState()) {
				return;
			}
			// not in Delirium yet: each second of channel brings her in by chance (then every tick keeps her in, above).
			// Ticks come ~0.15 s apart: the chance for the time since the last one (a fresh channel counts as one tick)
			if (!a_sure && !held_.contains(a_target->GetFormID())) {
				const auto  now = std::chrono::steady_clock::now();
				const auto  it = channelTick_.find(a_target->GetFormID());
				const float dt = it == channelTick_.end() ? 0.15f :
				                                            std::clamp(std::chrono::duration<float>(now - it->second).count(), 0.0f, 0.5f);
				if (channelTick_.size() > 64) {
					std::erase_if(channelTick_, [&](const auto& p) { return now - p.second > std::chrono::seconds(5); });
				}
				channelTick_[a_target->GetFormID()] = now;
				const float perSecond = StartChance(a_target, channelChance_);
				if (Rand(0.0f, 1.0f) >= 1.0f - std::pow(1.0f - perSecond, dt)) {
					return;  // Skyrim's own burn / shock / frost plays
				}
				logger::info("{:08X}: {} channel brings her in ({:.0f} % a second)", a_target->GetFormID(), what, perSecond * 100.0f);
			}
			channelTick_.erase(a_target->GetFormID());
			if (Activate(a_target)) {
				Channel(*active_.back(), a_target, a_vel, a_kind);
			}
			return;
		}
		const float speed = a_vel.Length();
		if (speed < 1.0f) {
			return;
		}
		// as hard as the hit (its damage), at the point it went in, along its flight
		Blow b;
		b.what = what;
		b.at = a_at;
		b.exact = a_exact;
		b.dir = { a_vel.x / speed, a_vel.y / speed, a_vel.z / speed };
		b.shot = true;
		b.element = a_kind;
		b.type = a_kind == kArrow ? kBlowArrow : kBlowSpell;
		LastHit     last;
		const bool  hit = a_target && RecentHit(a_target->GetFormID(), 0.5f, last);
		const float scale = hit ? DamageScale(a_target, last.damage) : 1.0f;
		b.oomph = std::clamp((scale - oomphFrom_) / (oomphTo_ - oomphFrom_), 0.0f, 1.0f);
		b.push = arrowPush_ * scale * (1.0f + oomphPush_ * b.oomph) * Rand(1.0f, 1.17f);
		if (hit) {
			b.by = last.from.get();
		}
		b.note = fmt::format("{:.0f} damage x{:.2f}, oomph {:.2f}", hit ? last.damage : 0.0f, scale, b.oomph);
		if (a_kind != kArrow && !a_sure) {
			b.startChance = StartChance(a_target, spellChance_);  // arrows always; a spell by chance unless she is in already
		}
		React(a_target, b);
	}

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

	void Engine::React(RE::Actor* a_target, const Blow& a_blow)
	{
		// a blow that may not bring her in: nothing to do when no one is in Delirium
		if (!a_blow.canStart && g_count.load(std::memory_order_relaxed) == 0) {
			return;
		}
		if (!Eligible(a_target, a_blow.what, !a_blow.shot)) {
			return;
		}
		std::scoped_lock l(lock_);
		Active*          act = nullptr;
		for (auto& p : active_) {
			if (p->actor.get().get() == a_target) {
				act = p.get();
			}
		}
		if (act && (act->knockQueued || act->death)) {
			DownHit(*act, a_target, a_blow);
			return;
		}
		if (!act) {
			if (!a_blow.canStart) {
				return;  // Skyrim's own hit reaction plays
			}
			if (a_target->IsInRagdollState()) {
				using K = RE::KNOCK_STATE_ENUM;
				const auto ks = a_target->AsActorState()->GetKnockState();
				const bool letGo = releasedAt_.contains(a_target->GetFormID());
				if (letGo && !a_target->IsDead() && ks != K::kNormal && ks != K::kQueued && ks != K::kGetUp &&
					a_target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kParalysis) <= 0.0f) {
					std::erase_if(skip_, [&](const RE::ActorHandle& h) {
						auto x = h.get();
						return !x || x.get() == a_target;
					});
					g_skip = static_cast<int>(skip_.size());
					logger::info("{:08X}: {} while lying in Skyrim's ragdoll (knock state {}): taken back", a_target->GetFormID(), a_blow.what, static_cast<int>(ks));
					if (Activate(a_target, true)) {
						DownHit(*active_.back(), a_target, a_blow);
					}
					return;
				}
				if (a_blow.shot) {
					logger::info("{}: skipped {:08X} (in Skyrim's own ragdoll, knock state {})", a_blow.what, a_target->GetFormID(), static_cast<int>(ks));
				}
				return;  // Skyrim's own ragdoll, not ours (its get-up, paralysis)
			}
			if (a_blow.startChance < 1.0f && !held_.contains(a_target->GetFormID())) {
				if (Rand(0.0f, 1.0f) >= a_blow.startChance) {
					logger::info("{:08X}: {} doesn't bring her in ({:.0f} % chance): Skyrim's own hit", a_target->GetFormID(), a_blow.what, a_blow.startChance * 100.0f);
					return;
				}
				logger::info("{:08X}: {} brings her in ({:.0f} % chance)", a_target->GetFormID(), a_blow.what, a_blow.startChance * 100.0f);
			}
			if (!Activate(a_target)) {
				return;
			}
			act = active_.back().get();
		}
		ReactOn(*act, a_target, a_blow);
	}

	void Engine::ReactOn(Active& a, RE::Actor* a_actor, const Blow& b)
	{
		a.lastType = b.type;
		a.lastElement = b.element;
		// where it landed if that was on her, else the chest
		float at[3]{}, q[4]{};
		int   part = kChest;
		if (b.exact) {
			const RE::NiPoint3 h = (b.at - a.origin) * kUnit;
			at[0] = h.x;
			at[1] = h.y;
			at[2] = h.z;
			part = NearestPart(a, at);
		} else {
			dl.nm_agent_get_part_transform(a.agent, kChest, at, q);
		}
		float      look[3]{};
		const bool sees = b.by && b.by.get() != a_actor;
		if (sees) {
			LookAt(a, b.by.get(), look);
		}
		if (b.fly.x != 0.0f || b.fly.y != 0.0f || b.fly.z != 0.0f) {
			Fly(a, a_actor, b);
			return;
		}
		if (a.flying > 0 && (!a.knockQueued || a.takeover)) {
			// in the air: the throw's behaviours play on, the blow only adds its push (a killing blow too: she dies once down)
			if (held_.contains(a_actor->GetFormID())) {
				a.heldDeath = true;
			}
			const float added = AddPush(a, at, b.dir, std::min(b.push, b.melee ? meleeMaxPush_ : hitMaxPush_));
			logger::info("{:08X}: {} in the air: push {:.0f} N s added, the throw plays on{} ({})", a_actor->GetFormID(), b.what, added,
				a.heldDeath ? ", dying" : "", b.note);
			return;
		}
		if (b.join && a.age - a.lastPushAt < 0.3f) {
			const float added = AddPush(a, at, b.dir, b.push);
			logger::info("{:08X}: {} with the reaction just started: push {:.0f} N s added (wanted {:.0f}; {})", a_actor->GetFormID(), b.what, added,
				b.push, b.note);
			return;
		}
		// the reaction, from what the blow is and how hard: a projectile went in (GTA's shot task: health on its scale,
		// 100 and up stays on its feet and catches a fall, under 100 is hurt and falls); else a light blow flinches (guard
		// up, turned to whoever did it) and a harder one makes her step to catch it, looking at them. Past what the steps
		// can catch she falls (the knock, as for any push). Each task stops every behaviour first.
		std::string how;
		float       health = 0.0f;
		int         r;
		// a projectile, or a blow past a flinch: Delirium's shot task, wounded where it landed and along it, fighting to
		// stay on her feet (a blow as GTA's melee set)
		const bool held = held_.contains(a_actor->GetFormID());  // the killing blow, held back: she dies on her feet
		if (held) {
			a.heldDeath = true;
			if (a.collapseAt < 0.0f) {
				a.collapseAt = a.age + Rand(collapseMin_, collapseMax_);
			}
		}
		// the reaction table: this blow's row (phase, type, strength), the element's layers on top
		std::string key;
		Rule        rule = PickRule("stand", b, held, key);
		{
			const Rule el = ElementRule(b.element);
			rule.layers.insert(rule.layers.end(), el.layers.begin(), el.layers.end());
		}
		if (held && !rule.Has("collapse")) {
			a.collapseAt = -1.0f;  // the row says she doesn't go down by herself: her time runs out on her feet
		}
		const bool asShot = rule.base == "shot";
		if (asShot) {
			auto*       av = a_actor->AsActorValueOwner();
			const float full = av->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float share = full > 0.0f ? std::clamp(av->GetActorValue(RE::ActorValue::kHealth) / full, 0.0f, 1.0f) : 1.0f;
			health = rule.Value("shot", share >= wounded_ ? 100.0f + 100.0f * share : 50.0f + 49.0f * share / std::max(wounded_, 1e-3f));
			const float d[3]{ b.dir.x, b.dir.y, b.dir.z };
			r = dl.nm_agent_task_shot(a.agent, a.world, b.melee ? "MELEE" : nullptr, part, at, d, health);
			how = b.melee ? "wounded (shot task, melee)" : "shot";
			if (held) {
				how += ", dying on her feet";
			}
			if (r >= 0 && b.oomph > 0.0f && rule.Has("spin")) {
				// a harder hit jolts harder: Delirium's shot twists the spine on impact (its shock spin, on any part now), read
				// on the shot's first tick. No lean on top: any leanInDirection with the shot takes her off her feet (fit-test
				// react: lean 0.15 falls at 2.5 s, 1.2 at once; without it she stays up to a 200 N s push)
				const float k = 1.0f + oomphSpin_ * b.oomph;
				dl.nm_msg_begin(a.agent, "shot");
				dl.nm_msg_set_bool(a.agent, "alwaysAddShockSpin", 1);
				dl.nm_msg_set_float(a.agent, "shockSpinMin", 40.0f * k);
				dl.nm_msg_set_float(a.agent, "shockSpinMax", 80.0f * k);
				if (dl.nm_msg_send(a.agent, a.world) < 0) {
					logger::error("  shot oomph: {}", LastError());
				}
			}
			float pp[3]{}, pq[4]{};
			dl.nm_agent_get_part_transform(a.agent, 0, pp, pq);
			const float fl = std::sqrt(b.dir.x * b.dir.x + b.dir.y * b.dir.y);
			a.shotPelvis = { pp[0], pp[1], pp[2] };
			a.shotDir = fl > 1e-3f ? V3{ b.dir.x / fl, b.dir.y / fl, 0.0f } : V3{};
			a.haveShot = fl > 1e-3f;
		} else if (rule.base == "flinch" && sees) {
			r = dl.nm_agent_task_flinch(a.agent, a.world, nullptr, look);
			how = "flinch";
		} else if (rule.base == "none") {
			r = 0;
			how = "no behaviour, the push only";
		} else {
			r = dl.nm_agent_task_balance(a.agent, a.world, nullptr, sees ? look : nullptr);
			how = "step";
		}
		// the push at most: past it one shove knocks her over limp before anything plays; the behaviours carry the rest
		const float want = rule.Value("push", 1.0f) *
			(b.melee ? std::min(b.push, meleeMaxPush_) : b.shot ? std::min(b.push, hitMaxPush_) : b.push);
		if (r >= 0 && asShot && b.oomph > 0.0f) {
			// a heavy hit: the head snaps with it, she stumbles back along it on the shot task's own steps
			if (rule.Has("headsnap")) {
				float hp[3]{}, hq[4]{};
				dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
				const float w = headWhip_ * want * (0.5f + b.oomph) * rule.Value("headsnap", 1.0f);
				const float imp[3]{ w * b.dir.x, w * b.dir.y, w * b.dir.z };
				dl.nm_agent_apply_push(a.agent, hp, imp);
				how += fmt::format(", head snaps {:.0f} N s", w);
			}
			// if she goes down anyway: on the ground for a while (a blow, half as long)
			a.writheFor = std::max(a.writheFor, (b.melee ? 0.5f : 1.0f) * (writheTime_ + writheAdd_ * b.oomph));
			a.writheOomph = std::max(a.writheOomph, b.oomph);
			if (b.element >= 0) {
				a.writheElement = b.element;
			}
		}
		if (r < 0) {
			logger::error("  {} task: {}", how, LastError());
		}
		const float push = AddPush(a, at, b.dir, want, pelvisShare_);
		{
			float hp[3]{}, hq[4]{};
			dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
			if (hp[2] < 1.0f) {
				a.writheFor = std::max(a.writheFor, (b.melee ? 0.8f : 1.0f) * (writheTime_ + writheAdd_ * b.oomph));
				a.writheOomph = std::max(a.writheOomph, b.oomph);
				if (b.element >= 0) {
					a.writheElement = b.element;
				}
				how += ", on the ground already";
			}
		}
		// the element on top, for its own time; one still running goes on over the blow, to its own end
		if (const float t = rule.Value("burn", burnTime_); rule.Has("burn") && t > 0.0f) {
			a.burnUntil = std::max(a.burnUntil, a.age + t);
		}
		if (const float t = rule.Value("shock", shockTime_); rule.Has("shock") && t > 0.0f) {
			a.shockUntil = std::max(a.shockUntil, a.age + t);
		}
		if (const float t = rule.Value("chill", channelLinger_); rule.Has("chill") && t > 0.0f) {
			a.chillUntil = std::max(a.chillUntil, a.age + t);
		}
		how = fmt::format("[{}: {}] ", key, rule.text) + how;
		Overlays(a, V3{ b.dir.x, b.dir.y, 0.0f });
		Reacting(a);
		logger::info("{:08X}: {} in the {}{}: {}{}, push {:.0f} N s (wanted {:.0f}; {}) along {:.2f} {:.2f} {:.2f}{}{}", a_actor->GetFormID(), b.what,
			rig_[static_cast<std::size_t>(part)].name, b.exact ? "" : " (no point: the chest)", how,
			asShot ? fmt::format(" (health {:.0f})", health) : "", push, b.push, b.note, b.dir.x, b.dir.y, b.dir.z,
			a.burnUntil >= 0.0f ? fmt::format(", burning for {:.1f} s", a.burnUntil - a.age) : "",
			a.shockUntil >= 0.0f ? fmt::format(", shocked for {:.1f} s", a.shockUntil - a.age) : "");
	}

	void Engine::DownHit(Active& a, RE::Actor* a_actor, const Blow& b)
	{
		if (!a.death) {
			a.lastType = b.type;
			a.lastElement = b.element;
		}
		// the blow moves the part it landed on: Skyrim's body once it is in its ragdoll (Delirium only follows it then),
		// else Delirium's own. A blast (thrown) moves all of her
		// a body on the ground takes less than one on its feet: no stance to knock out ([Reactions] downhit push)
		std::string     key = "(none)";
		static const Rule kDefault = ParseRule("react, push:0.6");
		const Rule*     found = FindRule({ std::string("downhit.") + TypeName(b.type), "downhit" }, key);
		const Rule&     rule = found ? *found : kDefault;
		const float     cap = b.melee ? meleeMaxPush_ : hitMaxPush_;
		const float     push = std::min(b.push, cap) * rule.Value("push", 1.0f);
		const bool      thrown = b.fly.x != 0.0f || b.fly.y != 0.0f || b.fly.z != 0.0f;
		float           at[3]{}, q[4]{};
		int             part = kChest;
		if (b.exact) {
			const RE::NiPoint3 h = (b.at - a.origin) * kUnit;
			at[0] = h.x, at[1] = h.y, at[2] = h.z;
			part = NearestPart(a, at);
		} else {
			dl.nm_agent_get_part_transform(a.agent, kChest, at, q);
		}
		const bool skyrims = a.calibrated || a.takeover;
		if (!skyrims) {
			if (thrown) {
				const float m = 60.0f * 0.5f;  // about half her weight's worth of the blast
				const float imp[3]{ b.fly.x * m, b.fly.y * m, b.fly.z * m };
				dl.nm_agent_apply_push(a.agent, at, imp);
			} else if (push > 0.0f) {
				AddPush(a, at, b.dir, push);
			}
		} else if (thrown) {
			a.kicks.push_back({ -1, Scl(b.fly, 0.5f), {} });
		} else if (push > 0.0f) {
			if (a.kicks.size() > 8) {
				a.kicks.erase(a.kicks.begin());  // no drive ran to take them (a corpse held still)
			}
			float pp[3]{}, pq[4]{};
			dl.nm_agent_get_part_transform(a.agent, part, pp, pq);
			a.kicks.push_back({ part, Scl(b.dir, push), { at[0] - pp[0], at[1] - pp[1], at[2] - pp[2] } });
		}
		std::string how = thrown ? "the blast moves her" : fmt::format("{:.0f} N s on the {}", push, rig_[static_cast<std::size_t>(part)].name);
		// alive: she squirms from it on the ground (or burns, convulses, curls up shivering), for longer the harder it was.
		// One already going goes on longer; another element starts its own
		how = fmt::format("[{}: {}] ", key, rule.text) + how;
		if (!a.death && rule.Has("react")) {
			float dur = rule.Value("time", 1.0f) * (writheTime_ + writheAdd_ * b.oomph);
			if (b.element == kFire) {
				dur = std::max(dur, burnTime_);
			}
			const bool other = b.element >= 0 && b.element != a.writheElement;
			if (b.element >= 0) {
				a.writheElement = b.element;
			}
			a.writheOomph = std::max(a.writheOomph, b.oomph);
			a.writheFor = std::max(a.writheFor, dur);
			if (a.writheUntil >= 0.0f && !other) {
				a.writheUntil = std::max(a.writheUntil, a.age + dur);
				how += fmt::format(", on the ground {:.1f} s more", a.writheUntil - a.age);
			} else if (a.writheAt < 0.0f || other) {
				a.writheFor = a.writheAt >= 0.0f ? std::max(a.writheFor, dur) : dur;  // one still to start keeps its time
				a.writheAt = a.age + 0.1f;  // a beat after the blow, so the blow itself shows
				how += ", reacting on the ground";
			}
			a.settled = 0;
		}
		a.blend = 1.0f;
		a.blendingOut = false;
		logger::info("{:08X}: {} while down{}: {} ({})", a_actor->GetFormID(), b.what, a.death ? " (dead)" : "", how, b.note);
	}

	void Engine::Fly(Active& a, RE::Actor* a_actor, const Blow& b)
	{
		a.lastType = b.type;
		a.lastElement = b.element;
		// thrown, alive the whole way, Delirium's own behaviours (their own gains) stacked by phase:
		// in the air: far throws Delirium's highFall (arms windmill, legs pedal, the body turns into the fall, its own
		// catch); shorter ones arms windmilling and legs pedalling, or tucked in a roll. A push at the head tumbles her.
		// on the way: arms out at whatever she is thrown at (braceForImpact, the frame update)
		// coming down: reaching to catch it; down: writhing or curled up a while, then Skyrim gets her up
		dl.nm_msg_begin(a.agent, "stopAllBehaviours");
		dl.nm_msg_send(a.agent, a.world);
		a.reelUntil = -1.0f;
		const float along = std::sqrt(b.fly.x * b.fly.x + b.fly.y * b.fly.y);
		// the reaction table: fly.element.far, fly.element, fly.far, fly; the element's layers on top
		std::string       key = "(none)";
		const std::string el = ElementName(b.element);
		const bool        isFar = along >= highFallAt_;
		std::vector<std::string> keys;
		if (!el.empty()) {
			if (isFar) {
				keys.push_back("fly." + el + ".far");
			}
			keys.push_back("fly." + el);
		}
		const std::string ty = TypeName(b.type);
		if (isFar) {
			keys.push_back("fly." + ty + ".far");
		}
		keys.push_back("fly." + ty);
		if (isFar) {
			keys.push_back("fly.far");
		}
		keys.push_back("fly");
		Rule rule;
		for (const auto& k : keys) {
			if (auto it = rules_.find(k); it != rules_.end()) {
				rule = it->second;
				key = k;
				break;
			}
		}
		{
			const Rule e = ElementRule(b.element);
			rule.layers.insert(rule.layers.end(), e.layers.begin(), e.layers.end());
		}
		a.flyMode = Choose(rule, { "windmill", "rollup", "highfall", "foetal" });
		if (a.flyMode < 0) {
			a.flyMode = 3;
		}
		if (a.flyMode == 2) {
			dl.nm_msg_begin(a.agent, "highFall");
			dl.nm_msg_set_bool(a.agent, "start", 1);
		} else if (a.flyMode == 3) {
			// curled up (the upper body) with the legs kicking hard: the busiest throw (fit-test layers: 250 deg/s vs
			// highFall's 210, foetal alone 95)
			dl.nm_msg_begin(a.agent, "bodyFoetal");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_string(a.agent, "mask", "fb");
			dl.nm_msg_set_float(a.agent, "stiffness", 13.0f);
			dl.nm_msg_set_float(a.agent, "dampingFactor", 1.0f);
			dl.nm_msg_set_float(a.agent, "asymmetry", Rand(0.0f, 0.4f));
			dl.nm_msg_set_float(a.agent, "backTwist", Rand(-0.3f, 0.3f));
			dl.nm_msg_set_int(a.agent, "randomSeed", static_cast<int>(Rand(1.0f, 999.0f)));
		} else {
			dl.nm_msg_begin(a.agent, "pedalLegs");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_bool(a.agent, "backPedal", Rand(0.0f, 1.0f) < 0.5f);
			dl.nm_msg_send(a.agent, a.world);
			if (a.flyMode == 1) {
				dl.nm_msg_begin(a.agent, "bodyRollUp");
				dl.nm_msg_set_bool(a.agent, "start", 1);
				dl.nm_msg_set_string(a.agent, "mask", "fb");
			} else {
				dl.nm_msg_begin(a.agent, "armsWindmillAdaptive");
				dl.nm_msg_set_bool(a.agent, "start", 1);
				dl.nm_msg_set_bool(a.agent, "disableOnImpact", 1);
			}
		}
		if (dl.nm_msg_send(a.agent, a.world) < 0) {
			logger::error("  thrown: {}", LastError());
		}
		std::string layer;
		if (a.flyMode == 0) {
			// the spine writhes over the throw (its mask only): burning, it thrashes; else it twists and arches
			const bool fire = rule.Has("thrash");
			dl.nm_msg_begin(a.agent, "bodyWrithe");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_string(a.agent, "mask", "us");
			dl.nm_msg_set_float(a.agent, "backAmplitude", fire ? 1.8f : 1.1f);
			dl.nm_msg_set_float(a.agent, "backPeriod", (fire ? 0.4f : 0.7f) * Rand(0.85f, 1.15f));
			dl.nm_msg_send(a.agent, a.world);
			layer = fire ? ", thrashing on fire" : ", spine writhing";
		}
		if (rule.Has("shock")) {
			dl.nm_msg_begin(a.agent, "electrocute");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_float(a.agent, "stunMag", 0.4f);
			dl.nm_msg_set_float(a.agent, "stunInterval", 0.07f);
			dl.nm_msg_set_float(a.agent, "largeMult", 2.5f);
			dl.nm_msg_send(a.agent, a.world);
			layer += ", electrocuted";
		}
		const float dv[3]{ b.fly.x, b.fly.y, b.fly.z };
		if (!b.inherited && dl.nm_agent_add_velocity(a.agent, dv) < 0) {
			logger::error("  thrown: {}", LastError());
		}
		// the head goes first, the legs swing under: a tumble (a little sideways, random)
		float hp[3]{}, hq[4]{};
		dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
		const float t = b.inherited ? 0.0f : tumble_ * rule.Value("tumble", 1.0f) * along * Rand(0.8f, 1.2f);
		const float side = Rand(-0.35f, 0.35f);
		const float fx = along > 1e-3f ? b.fly.x / along : 0.0f, fy = along > 1e-3f ? b.fly.y / along : 0.0f;
		const float imp[3]{ t * (fx - side * fy), t * (fy + side * fx), 0.0f };
		if (t > 0.0f) {
			dl.nm_agent_apply_push(a.agent, hp, imp);
		}
		a.flying = 1;
		a.flyAt = a.age;
		a.braced = false;
		a.braceCheckAt = a.age + 0.1f;
		const float hard = std::clamp(along / std::max(flyMax_, 1e-3f), 0.0f, 1.0f);
		a.writheFor = writheTime_ + writheAdd_ * std::max(hard, b.oomph);
		a.writheOomph = std::max(hard, b.oomph);
		a.writheElement = b.element;
		a.lastPushAt = a.age;  // the hit and blast that come with it add nothing more
		a.lastPush = 1e6f;
		Reacting(a);
		static constexpr const char* kModes[]{ "arms windmilling, legs pedalling", "rolled up, legs pedalling", "high fall", "curled up" };
		logger::info("{:08X}: {}: [{}: {}] thrown {:.1f} m/s along, {:.1f} up, {}{}, tumble {:.0f} N s; writhes {:.1f} s once down ({})", a_actor->GetFormID(),
			b.what, key, rule.text, along, b.fly.z, kModes[a.flyMode], layer, t, a.writheFor, b.note);
	}

	void Engine::Channel(Active& a, RE::Actor* a_actor, const RE::NiPoint3& a_vel, int a_kind)
	{
		const float fl = std::sqrt(a_vel.x * a_vel.x + a_vel.y * a_vel.y);
		const V3    away = fl > 1.0f ? V3{ a_vel.x / fl, a_vel.y / fl, 0.0f } : V3{};
		const float until = a.age + channelLinger_;
		const char* now = nullptr;
		if (a_kind != kFire && a_kind != kShock && a_kind != kFrost) {
			return;
		}
		// on the ground: rolling in the flames, convulsing, curled up shivering, for as long as it keeps hitting (and a
		// moment after); before, every tick let her go as lying still and she got up into the next one
		constexpr float kTail = 1.0f;
		a.writheElement = a_kind;
		a.writheOomph = std::max(a.writheOomph, 0.5f);
		a.writheFor = std::max(a.writheFor, channelLinger_ + kTail);
		if (a.knockQueued) {
			if (a.writheUntil >= 0.0f) {
				a.writheUntil = std::max(a.writheUntil, until + kTail);
			} else if (a.writheAt < 0.0f) {
				a.writheAt = a.age;
				now = "down, starting on the ground";
			}
			a.settled = 0;
			a.blend = 1.0f;
			a.blendingOut = false;
			if (now) {
				logger::info("{:08X}: {} channel: {}", a_actor->GetFormID(), kHitNames[static_cast<std::size_t>(std::clamp(a_kind, 0, 4))], now);
			}
			return;
		}
		if (a_kind == kFire) {
			if (a.burnUntil < 0.0f) {
				StartBurn(a, away);
				now = "burning";
			}
			a.burnUntil = std::max(a.burnUntil, until);
		} else if (a_kind == kShock) {
			if (a.shockUntil < 0.0f) {
				StartShock(a);
				now = "shocked";
			}
			a.shockUntil = std::max(a.shockUntil, until);
		} else if (a_kind == kFrost) {
			if (a.chillUntil < 0.0f) {
				StartChill(a);
				now = "freezing";
			}
			a.chillUntil = std::max(a.chillUntil, until);
		} else {
			return;
		}
		a.calm = 0;
		a.sincePush = 0;
		a.blend = 1.0f;
		a.blendingOut = false;
		if (now) {
			logger::info("{:08X}: {} channel: {}", a_actor->GetFormID(), kHitNames[static_cast<std::size_t>(std::clamp(a_kind, 0, 4))], now);
		}
	}

	void Engine::Reacting(Active& a)
	{
		a.calm = 0;
		a.sincePush = 0;
		a.blend = 1.0f;
		a.blendingOut = false;
	}

	float Engine::AddPush(Active& a, const float* a_at, const V3& a_dir, float a_push, float a_pelvis)
	{
		// a_pelvis: that share goes to the pelvis, so the whole body is driven back into catch steps instead of folding
		float push = a_push;
		if (a.age - a.lastPushAt < 0.3f) {
			push = std::max(0.0f, a_push - a.lastPush);  // the same blow seen twice (hit + stagger): the stronger one counts
			a.lastPush = std::max(a.lastPush, a_push);
		} else {
			a.lastPush = a_push;
			a.lastPushAt = a.age;
		}
		if (push > 0.0f) {
			const float k = 1.0f - a_pelvis;
			const float imp[3]{ k * push * a_dir.x, k * push * a_dir.y, k * push * a_dir.z };
			dl.nm_agent_apply_push(a.agent, a_at, imp);
			if (a_pelvis > 0.0f) {
				float pp[3]{}, pq[4]{};
				dl.nm_agent_get_part_transform(a.agent, 0, pp, pq);
				const float ip[3]{ a_pelvis * push * a_dir.x, a_pelvis * push * a_dir.y, a_pelvis * push * a_dir.z };
				dl.nm_agent_apply_push(a.agent, pp, ip);
			}
		}
		return push;
	}

	float Engine::DamageScale([[maybe_unused]] RE::Actor* a_target, float a_damage) const
	{
		// by the damage itself, the same for everyone: fDamageRef pushes at 1x, twice the damage 2^fExponent
		// as hard (1.74x at 0.8), within fScaleMin-fScaleMax. Her health does not flatten a stronger spell
		if (a_damage <= 0.0f) {
			return 1.0f;
		}
		return std::clamp(std::pow(a_damage / damageRef_, damageExp_), damageMin_, damageMax_);
	}

	void Engine::OnHitData(RE::Actor* a_target, const RE::HitData& a_hit)
	{
		if (!ready_ || a_target->IsPlayerRef()) {
			return;
		}
		LastHit h;
		h.when = std::chrono::steady_clock::now();
		h.from = a_hit.aggressor;
		h.at = a_hit.hitPosition;
		h.dir = a_hit.hitDirection;
		h.damage = a_hit.totalDamage;
		h.flags = a_hit.flags.underlying();
		h.exact = (h.at - a_target->GetPosition()).Length() < 200.0f;  // a point on her (some paths leave it empty)
		// an explosion's hit comes from the explosion itself (no flag, no damage, no point): never a melee blow
		const auto src = a_hit.sourceRef.get();
		auto*      srcBase = src ? src->GetBaseObject() : nullptr;
		const bool blast = srcBase && srcBase->GetFormType() == RE::FormType::Explosion;
		h.strike = blast ? 0.0f : StrikePush(a_target, a_hit, h.strikeDir);
		// the explosion's own hit has no damage: its spell's hit came just before it (Fireball's 40), kept
		float spellDamage = 0.0f;
		int   spellElement = -1;
		if (LastHit prev; blast && RecentHit(a_target->GetFormID(), 0.5f, prev)) {
			spellDamage = prev.damage;
			spellElement = prev.element;
		}
		if (!blast) {
			RecordHit(a_target->GetFormID(), h);
		}
		// a blow while Delirium has her (no stagger then: she is staggered already). Not the killing one: death plays that
		if (h.strike > 0.0f && a_target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) > a_hit.totalDamage) {
			Blow b;
			b.what = "blow";
			b.by = a_hit.aggressor.get();
			b.at = h.at;
			b.exact = h.exact;
			b.dir = { h.strikeDir.x, h.strikeDir.y, h.strikeDir.z };
			b.push = h.strike;
			b.canStart = false;
			b.melee = true;
			b.type = kBlowMelee;
			b.oomph = meleePush_ > 0.0f ? std::clamp((b.push / meleePush_ - meleeOomphFrom_) / (meleeOomphTo_ - meleeOomphFrom_), 0.0f, 1.0f) : 0.0f;
			b.note = fmt::format("{:.0f} damage, {}{}{}", a_hit.totalDamage, a_hit.weapon ? a_hit.weapon->GetName() : "no weapon",
				a_hit.flags.any(RE::HitData::Flag::kPowerAttack) ? ", power" : "", a_hit.flags.any(RE::HitData::Flag::kBash) ? ", bash" : "");
			React(a_target, b);
		}
		if (blast) {
			Blast(a_target, a_hit, static_cast<RE::Explosion*>(src.get()), srcBase->As<RE::BGSExplosion>(), spellDamage, spellElement);
		}
		// trace: what Skyrim's hits carry, and the object it came from (a projectile, an explosion)
		const auto by = a_hit.aggressor.get();
		const auto  srcPos = src ? src->GetPosition() : RE::NiPoint3{};
		logger::info("hitdata: {:08X} by {:08X} ('{}'), {:.0f} damage, stagger {:.2f}, flags {:X}, at {} ({:.0f} {:.0f} {:.0f}), dir {:.2f} {:.2f} {:.2f}; source {:08X} type {} '{}' at {:.0f} {:.0f} {:.0f}",
			a_target->GetFormID(), by ? by->GetFormID() : 0, a_hit.weapon ? a_hit.weapon->GetName() : "", a_hit.totalDamage, a_hit.stagger, h.flags,
			h.exact ? "on her" : "far", h.at.x, h.at.y, h.at.z, h.dir.x, h.dir.y, h.dir.z,
			src ? src->GetFormID() : 0, srcBase ? static_cast<int>(srcBase->GetFormType()) : -1, srcBase ? srcBase->GetName() : "",
			srcPos.x, srcPos.y, srcPos.z);
	}

	void Engine::Blast(RE::Actor* a_target, const RE::HitData& a_hit, RE::Explosion* a_ex, RE::BGSExplosion* a_base, float a_spellDamage, int a_spellElement)
	{
		if (!blasts_ || !a_ex || !a_base) {
			return;
		}
		// away from its centre (flat, a little up); at the centre itself (a direct hit) away from whoever cast it
		const RE::NiPoint3 c = a_ex->GetPosition();
		RE::NiPoint3       d = a_target->GetPosition() - c;
		d.z = 0.0f;
		const float dist = d.Length();
		auto        by = a_hit.aggressor.get();
		if (dist < 5.0f) {
			d = by && by.get() != a_target ? a_target->GetPosition() - by->GetPosition() : RE::NiPoint3{};
			d.z = 0.0f;
		}
		if (d.Length() < 1e-3f) {
			const float z = a_target->GetAngleZ();
			d = { -std::sin(z), -std::cos(z), 0.0f };  // backwards
		}
		d.Unitize();
		const float side = std::sqrt(1.0f - blastUp_ * blastUp_);
		const V3    dir{ d.x * side, d.y * side, blastUp_ };
		// by its damage (the one this explosion does, else its form's) and how near: full at the centre, fFarShare at the edge
		const auto& ex = a_ex->GetExplosionRuntimeData();  // its layout differs between SE and AE
		const float radius = std::max(1.0f, ex.radius > 0.0f ? ex.radius : a_base->data.radius);
		// its damage: the spell's that set it off, else the explosion's own, else its form's
		const float damage = std::clamp(a_spellDamage > 0.0f ? a_spellDamage : ex.damage > 0.0f ? ex.damage : a_base->data.damage, 0.0f, 1000.0f);
		const float closeness = std::clamp(1.0f - dist / radius, 0.0f, 1.0f);
		const float share = blastFar_ + (1.0f - blastFar_) * closeness;
		const float scale = DamageScale(a_target, damage) * share;
		Blow b;
		b.what = "blast";
		b.type = kBlowBlast;
		b.by = by;
		b.at = c;
		b.dir = dir;
		b.oomph = std::clamp((scale - oomphFrom_) / (oomphTo_ - oomphFrom_), 0.0f, 1.0f);
		b.push = blastPush_ * scale * (1.0f + oomphPush_ * b.oomph) * Rand(0.95f, 1.1f);
		b.join = true;
		// thrown: by the damage and how near, away from the centre and up
		const float v = std::min(flyMax_, flySpeed_ * scale * Rand(0.9f, 1.15f));
		if (v >= flyMin_) {
			b.fly = { d.x * v, d.y * v, v * flyUp_ };
		}
		// its element: the explosion's enchantment (Fireball's fire damage), by the resistance it goes against
		if (auto* ench = a_base->formEnchanting) {
			for (auto* e : ench->effects) {
				const auto* mgef = e ? e->baseEffect : nullptr;
				if (!mgef) {
					continue;
				}
				const auto rv = mgef->data.resistVariable;
				if (rv == RE::ActorValue::kResistFire) {
					b.element = kFire;
				} else if (rv == RE::ActorValue::kResistShock) {
					b.element = kShock;
				} else if (rv == RE::ActorValue::kResistFrost) {
					b.element = kFrost;
				}
				if (b.element >= 0) {
					break;
				}
			}
		}
		if (b.element < 0 && a_spellElement >= 0 && a_spellElement <= kShock) {
			b.element = a_spellElement;
		}
		b.note = fmt::format("'{}' {:.0f} damage x{:.2f}, {:.0f} of {:.0f} from its centre (x{:.2f}), force {:.0f}, oomph {:.2f}, thrown {:.1f} m/s",
			a_base->GetName(), damage, DamageScale(a_target, damage), dist, radius, share, a_base->data.force, b.oomph, v >= flyMin_ ? v : 0.0f);
		React(a_target, b);
	}

	float Engine::StrikePush(RE::Actor* a_target, const RE::HitData& a_hit, RE::NiPoint3& a_dir) const
	{
		using F = RE::HitData::Flag;
		const auto& fl = a_hit.flags;
		const auto* w = a_hit.weapon;
		auto        by = a_hit.aggressor.get();
		if (!melee_ || fl.any(F::kExplosion) || (w && (w->IsBow() || w->IsCrossbow() || w->IsStaff())) || !by || by.get() == a_target) {
			return 0.0f;
		}
		float kind = 1.0f;
		if (fl.any(F::kPowerAttack)) {
			kind *= meleePower_;
		}
		if (fl.any(F::kBash)) {
			kind *= meleeBash_;
		}
		if (fl.any(F::kSneakAttack)) {
			kind *= meleeSneak_;
		}
		if (fl.any(F::kBlocked)) {
			kind *= meleeBlocked_;
		}
		// a heavier weapon hits harder (fists and claws weigh nothing: half the reference)
		const float weight = w && w->weight > 0.0f ? w->weight : 0.5f * meleeWeightRef_;
		const float heft = std::clamp(std::sqrt(weight / meleeWeightRef_), meleeHeftMin_, meleeHeftMax_);
		// away from the attacker, bent sideways along the swing's sweep; an overhead chop also pushes down
		RE::NiPoint3 away = a_target->GetPosition() - by->GetPosition();
		away.z = 0.0f;
		if (away.Length() < 1.0f) {
			const float heading = a_target->data.angle.z;
			away = { -std::sin(heading), -std::cos(heading), 0.0f };
		}
		away.Unitize();
		RE::NiPoint3       d = away;
		const RE::NiPoint3 s = a_hit.hitDirection;
		const float        sl = s.Length();
		if (sl > 1.0f) {
			RE::NiPoint3 side(s.x, s.y, 0.0f);
			side -= away * side.Dot(away);  // only the sweep across her: the way in is always away
			d += side * (meleeSwing_ / sl);
			if (s.z < -0.7f * sl) {
				d.z = -meleeChop_;
			}
		}
		d.Unitize();
		a_dir = d;
		return meleePush_ * kind * heft * DamageScale(a_target, a_hit.totalDamage) * Rand(0.85f, 1.2f);
	}

	void Engine::LookAt(Active& a, RE::Actor* a_by, float* a_out) const
	{
		RE::NiPoint3 head = a_by->GetPosition();
		head.z += 115.0f * a_by->GetScale();
		const RE::NiPoint3 h = (head - a.origin) * kUnit;
		a_out[0] = h.x;
		a_out[1] = h.y;
		a_out[2] = h.z;
	}

	void Engine::Overlays(Active& a, const V3& a_away)
	{
		if (a.burnUntil >= 0.0f) {
			StartBurn(a, a_away);
		}
		if (a.shockUntil >= 0.0f) {
			StartShock(a);
		}
		if (a.chillUntil >= 0.0f) {
			StartChill(a);
		}
	}

	void Engine::OnStagger(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source, bool a_started)
	{
		if (!ready_ || !staggers_ || !a_started || a_target->IsPlayerRef()) {
			return;
		}
		// what Skyrim staggered with: after its perks and its cap of 1
		float mag = std::clamp(a_magnitude, 0.0f, 1.0f);
		a_target->GetGraphVariableFloat("staggerMagnitude", mag);
		// a frame later: a spell's or shout's hit event comes after its stagger, and with it who cast it
		g_tasks->AddTask([h = a_target->GetHandle(), s = a_source ? a_source->GetHandle() : RE::ActorHandle{}, mag]() {
			if (auto t = h.get()) {
				auto src = s.get();
				Engine::Get().Stagger(t.get(), mag, src.get());
			}
		});
	}

	void Engine::Stagger(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source)
	{
		if (!a_target) {
			return;
		}
		constexpr auto kExplosion = static_cast<std::uint32_t>(RE::HitData::Flag::kExplosion);
		// the way it pushes: away from where an explosion went off, from the attacker (the stagger's own, else her
		// last hit's), else straight back
		const auto   me = a_target->GetPosition();
		RE::NiPoint3 d;
		const char*  why = "from the attacker";
		LastHit      last;
		const bool   hit = RecentHit(a_target->GetFormID(), 0.5f, last);
		RE::ActorPtr by = a_source && a_source != a_target ? RE::ActorPtr(a_source) : RE::ActorPtr{};
		if (!by && hit) {
			if (auto from = last.from.get(); from && from.get() != a_target) {
				by = from;
			}
		}
		if (hit && last.exact && (last.flags & kExplosion)) {
			d = me - last.at;
			why = "from the explosion";
		} else if (by) {
			d = me - by->GetPosition();
			if (!a_source) {
				why = "from her last hit's attacker";
			}
		}
		d.z = 0.0f;
		if (d.Length() < 1.0f) {
			const float heading = a_target->data.angle.z;  // forward = (sin, cos)
			d = { -std::sin(heading), -std::cos(heading), 0.0f };
			why = "nothing to go by: back";
		}
		d.Unitize();
		Blow b;
		b.what = "stagger";
		b.type = kBlowStagger;
		b.by = by;
		b.dir = { d.x, d.y, 0.0f };
		const float scale = hit ? DamageScale(a_target, last.damage) : 1.0f;
		b.push = staggerPush_ * (staggerMin_ + (1.0f - staggerMin_) * a_magnitude) * scale * Rand(0.9f, 1.1f);
		// a melee blow that staggers: as hard as the blow (attack, weapon, damage), more for a bigger stagger, along its swing
		if (hit && last.strike > 0.0f) {
			b.push = std::max(b.push, last.strike * (1.0f + staggerHitBoost_ * a_magnitude));
			b.dir = { last.strikeDir.x, last.strikeDir.y, last.strikeDir.z };
			why = "the blow";
			b.melee = true;
			b.type = kBlowMelee;
			b.oomph = meleePush_ > 0.0f ? std::clamp((last.strike / meleePush_ - meleeOomphFrom_) / (meleeOomphTo_ - meleeOomphFrom_), 0.0f, 1.0f) : 0.0f;
		}
		// where the blow landed if it was on her (a sword's edge, a shield bash), else the chest
		b.exact = hit && last.exact && !(last.flags & kExplosion);
		b.at = last.at;
		if (hit && last.shout && !b.melee) {
			// a shout: its own row; strong, it throws her (Unrelenting Force), along the push, up as a blast throws
			b.what = "shout";
			b.type = kBlowShout;
			b.oomph = a_magnitude;
			if (a_magnitude >= shoutThrowFrom_ && shoutFly_ > 0.0f) {
				const float v = shoutFly_ * a_magnitude * Rand(0.9f, 1.1f);
				b.fly = { b.dir.x * v, b.dir.y * v, flyUp_ * v };
			}
		}
		b.note = fmt::format("magnitude {:.2f}, {}, {:.0f} damage x{:.2f}, oomph {:.2f}", a_magnitude, why, hit ? last.damage : 0.0f, scale, b.oomph);
		React(a_target, b);
	}

	void Engine::StartBurn(Active& a, const V3& a_away)
	{
		// GTA's onFire done by hand (its tuning is in GTA's task params, not loaded): she keeps her balance while the
		// arms and back writhe fast, beating at the flames, and stumbles away from the hit
		dl.nm_msg_begin(a.agent, "bodyWrithe");
		dl.nm_msg_set_bool(a.agent, "start", 1);
		dl.nm_msg_set_string(a.agent, "mask", "ub");
		dl.nm_msg_set_float(a.agent, "armStiffness", 12.0f);
		dl.nm_msg_set_float(a.agent, "armDamping", 0.5f);
		dl.nm_msg_set_float(a.agent, "armPeriod", Rand(0.4f, 0.6f));
		dl.nm_msg_set_float(a.agent, "armAmplitude", Rand(1.5f, 2.0f));
		dl.nm_msg_set_float(a.agent, "elbowAmplitude", 1.5f);
		dl.nm_msg_set_float(a.agent, "backStiffness", 11.0f);
		dl.nm_msg_set_float(a.agent, "backDamping", 0.6f);
		dl.nm_msg_set_float(a.agent, "backPeriod", Rand(0.8f, 1.2f));
		dl.nm_msg_set_float(a.agent, "backAmplitude", Rand(0.8f, 1.2f));
		dl.nm_msg_send(a.agent, a.world);
		if (a_away.x != 0.0f || a_away.y != 0.0f) {
			const float v[3]{ a_away.x, a_away.y, 0.0f };
			dl.nm_msg_begin(a.agent, "leanInDirection");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_set_bool(a.agent, "applyAsForce", 0);
			dl.nm_msg_set_vec3(a.agent, "dir", v);
			dl.nm_msg_set_float(a.agent, "leanAmount", Rand(0.2f, 0.35f));
			dl.nm_msg_send(a.agent, a.world);
		}
	}

	void Engine::StopBurn(Active& a)
	{
		a.burnUntil = -1.0f;
		for (const char* m : { "bodyWrithe", "leanInDirection" }) {
			dl.nm_msg_begin(a.agent, m);
			dl.nm_msg_set_bool(a.agent, "start", 0);
			dl.nm_msg_send(a.agent, a.world);
		}
	}

	void Engine::StartShock(Active& a)
	{
		dl.nm_msg_begin(a.agent, "electrocute");
		dl.nm_msg_set_bool(a.agent, "start", 1);
		dl.nm_msg_set_float(a.agent, "stunMag", Rand(0.45f, 0.55f));
		dl.nm_msg_set_float(a.agent, "stunInterval", 0.07f);
		dl.nm_msg_set_float(a.agent, "largeMult", 3.0f);
		dl.nm_msg_set_float(a.agent, "largeMinTime", 0.2f);
		dl.nm_msg_set_float(a.agent, "largeMaxTime", 0.45f);
		dl.nm_msg_set_float(a.agent, "directionRandomness", 0.6f);
		dl.nm_msg_send(a.agent, a.world);
	}

	void Engine::StopShock(Active& a)
	{
		a.shockUntil = -1.0f;
		dl.nm_msg_begin(a.agent, "electrocute");
		dl.nm_msg_set_bool(a.agent, "start", 0);
		dl.nm_msg_send(a.agent, a.world);
	}

	void Engine::StartChill(Active& a)
	{
		// all Delirium's own: the upper body curls in (arms clutched, back and neck bent; the legs stay the balancer's),
		// a fine fast shiver that also stiffens the muscles, and short low shuffling steps
		dl.nm_msg_begin(a.agent, "bodyFoetal");
		dl.nm_msg_set_bool(a.agent, "start", 1);
		dl.nm_msg_set_string(a.agent, "mask", "ub");
		dl.nm_msg_set_float(a.agent, "stiffness", 7.0f);
		dl.nm_msg_set_float(a.agent, "dampingFactor", 1.6f);
		dl.nm_msg_set_float(a.agent, "asymmetry", 0.3f);
		dl.nm_msg_set_int(a.agent, "randomSeed", static_cast<int>(Rand(1.0f, 10000.0f)));
		dl.nm_msg_send(a.agent, a.world);
		dl.nm_msg_begin(a.agent, "electrocute");
		dl.nm_msg_set_bool(a.agent, "start", 1);
		dl.nm_msg_set_bool(a.agent, "leftLeg", 0);
		dl.nm_msg_set_bool(a.agent, "rightLeg", 0);
		dl.nm_msg_set_float(a.agent, "stunMag", 0.06f);
		dl.nm_msg_set_float(a.agent, "stunInterval", 0.05f);
		dl.nm_msg_set_float(a.agent, "largeMult", 1.0f);
		dl.nm_msg_set_float(a.agent, "directionRandomness", 0.3f);
		dl.nm_msg_send(a.agent, a.world);
		dl.nm_msg_begin(a.agent, "configureBalance");
		dl.nm_msg_set_float(a.agent, "stepClampScale", 0.5f);
		dl.nm_msg_set_float(a.agent, "stepHeight", 0.05f);
		dl.nm_msg_send(a.agent, a.world);
	}

	void Engine::StopChill(Active& a)
	{
		a.chillUntil = -1.0f;
		dl.nm_msg_begin(a.agent, "bodyFoetal");
		dl.nm_msg_set_bool(a.agent, "start", 0);
		dl.nm_msg_send(a.agent, a.world);
		if (a.shockUntil < 0.0f) {  // the shiver is an electrocute: a shock still running keeps it
			dl.nm_msg_begin(a.agent, "electrocute");
			dl.nm_msg_set_bool(a.agent, "start", 0);
			dl.nm_msg_send(a.agent, a.world);
		}
		dl.nm_msg_begin(a.agent, "configureBalanceReset");
		dl.nm_msg_send(a.agent, a.world);
	}

	int Engine::NearestPart(const Active& a, const float* a_at) const
	{
		// distance to each part's bone: from the part to its children (a hand or the head: the part itself)
		std::array<V3, 21> p;
		for (int i = 0; i < 21; ++i) {
			float t[3]{}, q[4]{};
			dl.nm_agent_get_part_transform(a.agent, i, t, q);
			p[static_cast<std::size_t>(i)] = { t[0], t[1], t[2] };
		}
		const V3 x{ a_at[0], a_at[1], a_at[2] };
		auto     seg = [&](const V3& s, const V3& e) {
            const V3    d = Sub(e, s), w = Sub(x, s);
            const float dd = d.x * d.x + d.y * d.y + d.z * d.z;
            const float t = dd > 1e-8f ? std::clamp((w.x * d.x + w.y * d.y + w.z * d.z) / dd, 0.0f, 1.0f) : 0.0f;
            const V3    c = Sub(x, Add(s, Scl(d, t)));
            return c.x * c.x + c.y * c.y + c.z * c.z;
		};
		int   best = 0;
		float bestD = std::numeric_limits<float>::max();
		for (std::size_t i = 0; i < 21; ++i) {
			float di = seg(p[i], p[i]);
			for (std::size_t c = 1; c < 21; ++c) {
				if (kParent[c] == static_cast<int>(i)) {
					di = std::min(di, seg(p[i], p[c]));
				}
			}
			if (di < bestD) {
				bestD = di;
				best = static_cast<int>(i);
			}
		}
		return best;
	}

	void Engine::Release(std::size_t a_i, const char* a_why)
	{
		auto& a = active_[a_i];
		logger::info("released ({}) after {:.1} s", a_why, a->age);
		RestoreJoints(*a);
		for (std::size_t k = 0; k < a->weighed.size(); ++k) {
			if (auto* body = a->weighed[k]) {
				std::memcpy(&body->motion.inertiaAndMassInv, a->bodies[k].massInv.data(), sizeof(a->bodies[k].massInv));
				if (k < a->material.size()) {
					body->material.friction = a->material[k][0];
					body->material.restitution = a->material[k][1];
				}
				body->RemoveReference();
			}
		}
		a->weighed.clear();
		if (auto act = a->actor.get(); act && act->IsInRagdollState()) {
			releasedAt_[act->GetFormID()] = std::chrono::steady_clock::now();
			skip_.push_back(a->actor);  // still in Skyrim's ragdoll (its get-up, or F2): not taken over again
			g_skip = static_cast<int>(skip_.size());
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

	// Projectiles. AddImpact (vtable slot 0xBD) is where an arrow or a spell's missile or beam meets something: what,
	// where and how fast (the velocity comes in reversed for arrows; it is checked against the shooter). Skyrim's hit
	// (TESHitEvent) comes after it, without a point, and says what hit (bow, spell) and whether it landed (not blocked).
	// Flame projectiles (Flames, Frostbite, Sparks: a stream, hits every frame) are left out. Patched by the core like
	// UpdateActor3DPosition and put back when it unloads.
	namespace
	{
		using AddImpactFn = std::uintptr_t (*)(RE::Projectile*, RE::TESObjectREFR*, const RE::NiPoint3&, const RE::NiPoint3&, RE::hkpCollidable*, std::int32_t,
			std::uint32_t);
		struct Impact
		{
			RE::ObjectRefHandle                   target;
			RE::NiPoint3                          at, vel;  // game units, units/s
			bool                                  spell{};  // a missile or beam (else an arrow)
			std::chrono::steady_clock::time_point when;
		};
		constexpr std::array<const char*, 3> kProjNames{ "ArrowProjectile", "MissileProjectile", "BeamProjectile" };
		std::array<std::uintptr_t*, 3>       g_impactSlot{};
		std::array<AddImpactFn, 3>           g_origImpact{};
		std::mutex                           g_impactLock;
		std::vector<Impact>                  g_impacts;  // projectiles on actors, the last 2 s

		// what it returns passes through untouched (the game's may be a pointer)
		template <int K>
		std::uintptr_t ImpactHook(RE::Projectile* a_proj, RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_at, const RE::NiPoint3& a_vel, RE::hkpCollidable* a_col,
			std::int32_t a_6, std::uint32_t a_7)
		{
			if (a_ref && a_ref->Is(RE::FormType::ActorCharacter)) {
				// trace: impacts on actors
				logger::info("{} impact: ref {:08X} at {:.0f} {:.0f} {:.0f}, velocity {:.0f} {:.0f} {:.0f}", kProjNames[K], a_ref->GetFormID(), a_at.x, a_at.y, a_at.z,
					a_vel.x, a_vel.y, a_vel.z);
				const auto       now = std::chrono::steady_clock::now();
				std::scoped_lock l(g_impactLock);
				std::erase_if(g_impacts, [&](const Impact& i) { return now - i.when > std::chrono::seconds(2); });
				g_impacts.push_back({ a_ref->GetHandle(), a_at, a_vel, K != 0, now });
			}
			return g_origImpact[K](a_proj, a_ref, a_at, a_vel, a_col, a_6, a_7);
		}
		constexpr std::array<AddImpactFn, 3> kImpactHooks{ &ImpactHook<0>, &ImpactHook<1>, &ImpactHook<2> };

		bool RecentImpact(RE::TESObjectREFR* a_ref, float a_maxAge, RE::NiPoint3& a_at, RE::NiPoint3& a_vel, int a_spell)
		{
			const auto       now = std::chrono::steady_clock::now();
			std::scoped_lock l(g_impactLock);
			for (auto it = g_impacts.rbegin(); it != g_impacts.rend(); ++it) {
				if (std::chrono::duration<float>(now - it->when).count() <= a_maxAge && it->target.get().get() == a_ref &&
					(a_spell < 0 || it->spell == (a_spell == 1))) {
					a_at = it->at;
					a_vel = it->vel;
					return true;
				}
			}
			return false;
		}

		// a destruction spell's element, by its damage effect's resistance (-1: not a destruction spell)
		int Element(const RE::MagicItem* a_magic)
		{
			if (!a_magic) {
				return -1;
			}
			bool destruction = false;
			for (const auto* e : a_magic->effects) {
				const auto* m = e ? e->baseEffect : nullptr;
				if (!m) {
					continue;
				}
				switch (m->data.resistVariable) {
				case RE::ActorValue::kResistFire:
					return Engine::kFire;
				case RE::ActorValue::kResistFrost:
					return Engine::kFrost;
				case RE::ActorValue::kResistShock:
					return Engine::kShock;
				default:
					break;
				}
				destruction |= m->data.associatedSkill == RE::ActorValue::kDestruction;
			}
			return destruction ? Engine::kMagic : -1;
		}

		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastSpellHit;  // hit sink thread only

		bool Staggers(const RE::MagicItem* a_magic)
		{
			if (a_magic) {
				for (const auto* e : a_magic->effects) {
					if (e && e->baseEffect && e->baseEffect->HasArchetype(RE::EffectSetting::Archetype::kStagger)) {
						return true;
					}
				}
			}
			return false;
		}

		// a spell's health damage (its harmful health effects' magnitudes; perks and resistances left out)
		float SpellDamage(const RE::MagicItem* a_magic)
		{
			float d = 0.0f;
			if (a_magic) {
				for (const auto* e : a_magic->effects) {
					const auto* m = e ? e->baseEffect : nullptr;
					if (m && m->data.primaryAV == RE::ActorValue::kHealth && m->IsDetrimental()) {
						d += e->effectItem.magnitude;
					}
				}
			}
			return d;
		}

		struct HitSink : RE::BSTEventSink<RE::TESHitEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				// (its projectile is 0 in this setup: what hit is known by the source, a bow or a spell, and the impact)
				if (a_event && a_event->target) {
					// trace: every hit
					auto* sf = RE::TESForm::LookupByID(a_event->source);
					logger::info("hit: target {:08X} cause {:08X} source {:08X} ('{}', type {}) projectile {:08X} flags {:X}", a_event->target->GetFormID(),
						a_event->cause ? a_event->cause->GetFormID() : 0, a_event->source, sf ? sf->GetName() : "", sf ? static_cast<int>(sf->GetFormType()) : -1,
						a_event->projectile, static_cast<int>(a_event->flags.underlying()));
				}
				if (!a_event || !a_event->target || a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* target = a_event->target->As<RE::Actor>();
				if (!target || target->IsPlayerRef()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* src = RE::TESForm::LookupByID(a_event->source);
				// her last hit, for a spell or shout (a weapon's came already with its HitData); an explosion's own hit
				// (no damage) keeps the spell's that set it off
				if (auto* cause = a_event->cause ? a_event->cause->As<RE::Actor>() : nullptr; cause && !(src && src->GetFormType() == RE::FormType::Explosion)) {
					const RE::MagicItem* magic = src ? src->As<RE::MagicItem>() : nullptr;
					if (auto* w = src ? src->As<RE::TESObjectWEAP>() : nullptr; w && w->IsStaff()) {
						magic = w->formEnchanting;
					}
					LastHit h;
					h.when = std::chrono::steady_clock::now();
					h.from = cause->GetHandle();
					h.damage = SpellDamage(magic);
					h.element = Element(magic);
					h.shout = magic && magic->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower;
					RecordHit(target->GetFormID(), h);
				}
				int  kind = -1;
				bool channel = false;  // a concentration spell: hits as long as it is held
				bool staggers = false;  // a spell with a stagger effect: always brings her in, like any stagger
				if (auto* w = src ? src->As<RE::TESObjectWEAP>() : nullptr) {
					if (w->IsBow() || w->IsCrossbow()) {
						kind = Engine::kArrow;
					} else if (w->IsStaff()) {
						kind = Element(w->formEnchanting);  // a staff: its spell is its enchantment
						channel = w->formEnchanting && w->formEnchanting->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
						staggers = Staggers(w->formEnchanting);
					}
				} else if (auto* m = src ? src->As<RE::MagicItem>() : nullptr) {
					kind = Element(m);
					channel = m->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
					staggers = Staggers(m);
				}
				if (kind < 0) {
					return RE::BSEventNotifyControl::kContinue;  // a melee hit, a heal, a ward, ...
				}
				const bool spell = kind != Engine::kArrow;
				if (spell && !channel) {
					// a spell sends one hit per effect, all at once: one reaction
					const auto now = std::chrono::steady_clock::now();
					auto&      last = g_lastSpellHit[target->GetFormID()];
					if (now - last < std::chrono::milliseconds(300)) {
						return RE::BSEventNotifyControl::kContinue;
					}
					last = now;
				}
				// a spell's hit comes before its missile's impact is recorded: look for it a frame later
				g_tasks->AddTask([h = target->GetHandle(), c = a_event->cause ? a_event->cause->GetHandle() : RE::ObjectRefHandle{}, kind, spell, channel, staggers]() {
					auto t = h.get();
					if (!t) {
						return;
					}
					if (channel) {
						RE::NiPoint3 away;
						if (auto cause = c.get()) {
							away = t->GetPosition() - cause->GetPosition();
							away.z = 0.0f;
						}
						Engine::Get().OnHit(t.get(), t->GetPosition(), away, false, kind, true, staggers);
						return;
					}
					RE::NiPoint3 at, vel;
					const bool   exact = RecentImpact(t.get(), 0.5f, at, vel, spell ? 1 : 0);
					logger::info("  hit kind {}, impact {}", kind, exact);  // trace
					if (spell && !exact) {
						return;  // a spell without a missile or beam: a stream, a cloak, a rune
					}
					if (auto cause = c.get()) {
						// the way it flew must be away from the shooter (arrows: reversed); a missile's is only a unit vector, a beam's may be nothing
						const RE::NiPoint3 away = t->GetPosition() - cause->GetPosition();
						if (!exact || vel.Length() < 10.0f) {
							vel = { away.x, away.y, 0.0f };
							const float l = vel.Length();
							vel = l > 1.0f ? vel * (3000.0f / l) : RE::NiPoint3(0.0f, 3000.0f, 0.0f);
						} else if (vel.x * away.x + vel.y * away.y < 0.0f) {
							vel = -vel;
						}
					} else if (!exact) {
						return;
					}
					Engine::Get().OnHit(t.get(), at, vel, exact, kind, false, staggers);
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		} g_hitSink;
		bool g_hitSinkOn = false;

		void HookArrows()
		{
			const std::array<REL::Relocation<std::uintptr_t>, 3> vts{ REL::Relocation<std::uintptr_t>{ RE::VTABLE_ArrowProjectile[0] },
				REL::Relocation<std::uintptr_t>{ RE::VTABLE_MissileProjectile[0] }, REL::Relocation<std::uintptr_t>{ RE::VTABLE_BeamProjectile[0] } };
			for (std::size_t k = 0; k < 3; ++k) {
				if (g_impactSlot[k]) {
					continue;
				}
				g_impactSlot[k] = reinterpret_cast<std::uintptr_t*>(vts[k].address()) + 0xBD;
				g_origImpact[k] = reinterpret_cast<AddImpactFn>(*g_impactSlot[k]);
				PatchSlot(g_impactSlot[k], reinterpret_cast<std::uintptr_t>(kImpactHooks[k]));
				logger::info("hooked {}::AddImpact (vtable {:X}, was {:X})", kProjNames[k], vts[k].address(), reinterpret_cast<std::uintptr_t>(g_origImpact[k]));
			}
			if (auto* events = RE::ScriptEventSourceHolder::GetSingleton(); events && !g_hitSinkOn) {
				events->AddEventSink<RE::TESHitEvent>(&g_hitSink);
				g_hitSinkOn = true;
			}
		}
		void UnhookArrows()
		{
			if (g_hitSinkOn) {
				if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
					events->RemoveEventSink<RE::TESHitEvent>(&g_hitSink);
				}
				g_hitSinkOn = false;
			}
			for (std::size_t k = 0; k < 3; ++k) {
				if (g_impactSlot[k]) {
					PatchSlot(g_impactSlot[k], reinterpret_cast<std::uintptr_t>(g_origImpact[k]));
					g_impactSlot[k] = nullptr;
				}
			}
			std::scoped_lock l(g_impactLock);
			g_impacts.clear();
		}
	}

	void Engine::AfterRootMove(RE::Actor* a_actor)
	{
		std::scoped_lock l(lock_);
		for (auto& p : active_) {
			auto& a = *p;
			if (!a.knockQueued || a.actor.get().get() != a_actor) {
				continue;
			}
			auto* top = a_actor->Get3D(false);
			if (!top) {
				return;
			}
			struct TraceAfter
			{
				Engine* e;
				Active& a;
				RE::Actor* act;
				~TraceAfter() { e->Trace(a, act, "root move"); }
			} traceAfter{ this, a, a_actor };
			RE::NiUpdateData ud{};
			ClearJointFixOffset(a.ref[0], a.com);  // again: it can run between the pose copy and here
			if (!TorqueMode(a) && a.calibrated && a.haveCom) {
				// lead-in: ours moved toward Skyrim's COM, straight from its body (bone = body * (mapR, mapT)^-1)
				for (const auto& b : a.bodies) {
					if (b.node != a.com) {
						continue;
					}
					const auto& ms = b.body->motion.motionState;
					const auto* cl = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMassLocal);
					const auto* cw = reinterpret_cast<const float*>(&ms.sweptTransform.centerOfMass1);
					const auto* qc = reinterpret_cast<const float*>(&ms.sweptTransform.rotation1);
					const Q     qb{ qc[0], qc[1], qc[2], qc[3] };
					const V3    pb = Sub({ cw[0], cw[1], cw[2] }, Rot(qb, { cl[0], cl[1], cl[2] }));
					const Q     nr = Mul(qb, Conj(b.mapR));
					V3          np = Scl(Sub(pb, Rot(nr, Scl(b.mapT, a.scale))), 1.0f / kUnit);
					const float s = LeadShare(a);
					if (a.haveSkyCom) {
						// Skyrim's own pose copy draws COM a steady ~4 cm up from its body (seen in game, cause unknown), and
						// draws it so after the lead-in: the lead-in ends on its position, not the body's
						top->UpdateWorldData(&ud);
						std::vector<RE::NiAVObject*> up;
						for (auto* n = a.com->parent; n && n != top; n = n->parent) {
							up.push_back(n);
						}
						for (auto it = up.rbegin(); it != up.rend(); ++it) {
							(*it)->UpdateWorldData(&ud);
						}
						const RE::NiTransform sky = a.com->parent->world * a.skyComLocal;
						const RE::NiPoint3    d = (sky.translate - RE::NiPoint3(np.x, np.y, np.z)) * kUnit;
						const RE::NiPoint3    dl = top->world.rotate.Transpose() * d;
						logger::info("    lag {:.3f} lead: Skyrim's COM vs body t1 {:.3f} m (top frame {:.3f} {:.3f} {:.3f}), top scale {:.3f}", a.age, d.Length(), dl.x, dl.y, dl.z,
							top->world.scale);
						np = { sky.translate.x, sky.translate.y, sky.translate.z };
						logger::info("    blend {:.3f} s {:.2f}: ours {:.1f} {:.1f} {:.1f} sky {:.1f} {:.1f} {:.1f} top {:.1f} {:.1f} {:.1f}", a.age, s, a.comWorld.translate.x,
							a.comWorld.translate.y, a.comWorld.translate.z, sky.translate.x, sky.translate.y, sky.translate.z, top->world.translate.x, top->world.translate.y,
							top->world.translate.z);
					}
					if (std::isfinite(np.x + np.y + np.z + nr.w)) {
						a.comWorld.rotate = ToMat(Slerp(nr, FromMat(a.comWorld.rotate), s));
						a.comWorld.translate = RE::NiPoint3(np.x, np.y, np.z) + (a.comWorld.translate - RE::NiPoint3(np.x, np.y, np.z)) * s;
					}
				}
			}
			if (TorqueMode(a)) {
				// Skyrim's ragdoll poses the bones: its pose copy set them for the top node it has only just put on the
				// body, so COM's world is taken under that top node, then kept while ours glides after it
				top->UpdateWorldData(&ud);
				std::vector<RE::NiAVObject*> up;
				for (auto* n = a.com->parent; n && n != top; n = n->parent) {
					up.push_back(n);
				}
				for (auto it = up.rbegin(); it != up.rend(); ++it) {
					(*it)->UpdateWorldData(&ud);
				}
				a.comWorld = a.com->parent->world * a.com->local;
				if (a.age - a.calibAge < 0.45f) {
					// trace: which body state Skyrim's pose copy drew COM from (t1 = after the last step, t0 = before it)
					for (const auto& b : a.bodies) {
						if (b.node != a.com) {
							continue;
						}
						const auto& st = b.body->motion.motionState.sweptTransform;
						const auto* cl = reinterpret_cast<const float*>(&st.centerOfMassLocal);
						auto bonePos = [&](const float* cw, const float* qc) {
							const Q  qb{ qc[0], qc[1], qc[2], qc[3] };
							const V3 pb = Sub({ cw[0], cw[1], cw[2] }, Rot(qb, { cl[0], cl[1], cl[2] }));
							const Q  nr = Mul(qb, Conj(b.mapR));
							const V3 np = Scl(Sub(pb, Rot(nr, Scl(b.mapT, a.scale))), 1.0f / kUnit);
							return RE::NiPoint3(np.x, np.y, np.z);
						};
						const auto  p1 = bonePos(reinterpret_cast<const float*>(&st.centerOfMass1), reinterpret_cast<const float*>(&st.rotation1));
						const auto  p0 = bonePos(reinterpret_cast<const float*>(&st.centerOfMass0), reinterpret_cast<const float*>(&st.rotation0));
						const auto* tt = reinterpret_cast<const float*>(&st.centerOfMass0);
						const RE::NiPoint3 dl = top->world.rotate.Transpose() * ((a.comWorld.translate - p1) * kUnit);
						logger::info("    lag {:.3f}: drawn COM to body t1 {:.3f} m (top frame {:.3f} {:.3f} {:.3f}), to t0 {:.3f} m, t1-t0 {:.3f} m, top scale {:.3f}", a.age,
							(a.comWorld.translate - p1).Length() * kUnit, dl.x, dl.y, dl.z, (a.comWorld.translate - p0).Length() * kUnit, (p1 - p0).Length() * kUnit,
							top->world.scale);
						(void)tt;
						{
							// trace: every node from the top down to the pelvis (local translation, game units)
							std::vector<RE::NiAVObject*> ch;
							for (RE::NiAVObject* n = a.ref[0]; n; n = n->parent) {
								ch.push_back(n);
								if (n == top) {
									break;
								}
							}
							std::string cs;
							for (auto it = ch.rbegin(); it != ch.rend(); ++it) {
								cs += fmt::format(" {}({:.1f} {:.1f} {:.1f})", (*it)->name.c_str(), (*it)->local.translate.x, (*it)->local.translate.y, (*it)->local.translate.z);
							}
							logger::info("    lag {:.3f} chain:{}", a.age, cs);
						}
						// trace: the bones between COM and the first bodies (no body of their own): what poses them
						for (RE::NiAVObject* n : std::array<RE::NiAVObject*, 2>{ a.ref[0], a.ref[0] ? static_cast<RE::NiAVObject*>(a.ref[0]->parent) : nullptr }) {
							if (!n) {
								continue;
							}
							const auto& lr = n->local.rotate;
							logger::info("    lag {:.3f}: {} (parent {}) local t {:.1f} {:.1f} {:.1f} r {:.2f} {:.2f} {:.2f} / {:.2f} {:.2f} {:.2f}", a.age, n->name.c_str(),
								n->parent ? n->parent->name.c_str() : "-", n->local.translate.x, n->local.translate.y, n->local.translate.z, lr.entry[0][0], lr.entry[0][1],
								lr.entry[0][2], lr.entry[1][0], lr.entry[1][1], lr.entry[1][2]);
						}
					}
				}
			}
			// 1. the top node glides after Skyrim's instead of jumping (FSMP and SMP gear simulate relative to it)
			SmoothRoot(a, a_actor);
			if (!a.haveCom && !TorqueMode(a)) {
				return;
			}
			// 2. the nodes between it and COM, refreshed from it (only those: the bones must not move even for a moment)
			std::vector<RE::NiAVObject*> chain;
			for (auto* n = a.com->parent; n && n != top; n = n->parent) {
				chain.push_back(n);
			}
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
		if (auto it = releasedAt_.find(a_actor->GetFormID()); it != releasedAt_.end()) {
			logger::info("{:08X}: up from Skyrim's ragdoll {:.1f} s after we let go", a_actor->GetFormID(),
				std::chrono::duration<float>(std::chrono::steady_clock::now() - it->second).count());
			releasedAt_.erase(it);
		}
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
			const float p[3]{ pos[i].x, pos[i].y, pos[i].z }, q[4]{ rot[i].x, rot[i].y, rot[i].z, rot[i].w };
			const float lv[3]{ v[i].x, v[i].y, v[i].z }, av[3]{ w[i].x, w[i].y, w[i].z };
			dl.nm_agent_set_part_state(a.agent, i, p, q, lv, av);
		}
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
			bool held = false;
			if (auto self = a.actor.get()) {
				held = heldDied_.erase(self->GetFormID()) > 0;
			}
			StartDying(a, { cv[0], cv[1], 0.0f }, held);
		} else if (a.takeover) {
			Knocked(a, { cv[0], cv[1], cv[2] });
		}
	}

	void Engine::Knocked(Active& a, const V3& a_vel)
	{
		// Skyrim knocked her down by itself (a shout, a trap, a script's push, its own stagger fall): thrown fast, the
		// throw's behaviours on Skyrim's own motion (the fly rows, type knock); else she falls over, breaks the fall and
		// reacts on the ground (the down rows)
		auto self = a.actor.get();
		if (!self) {
			return;
		}
		const float along = std::sqrt(a_vel.x * a_vel.x + a_vel.y * a_vel.y);
		if (along >= flyMin_) {
			Blow b;
			b.what = "Skyrim's knock";
			b.note = "taken over";
			b.type = kBlowKnock;
			b.fly = a_vel;
			b.inherited = true;
			b.oomph = std::clamp(along / std::max(flyMax_, 1e-3f), 0.0f, 1.0f);
			Fly(a, self.get(), b);
			return;
		}
		a.lastType = kBlowKnock;
		a.writheFor = writheTime_;
		a.writheOomph = 0.0f;
		a.writheElement = -1;
		a.writheAt = a.age + 0.8f;  // once she is down
		logger::info("{:08X}: Skyrim's knock, {:.1f} m/s: falling over, breaking the fall, then reacting on the ground", self->GetFormID(), along);
	}

	void Engine::StartDying(Active& a, const V3& a_moving, bool a_held)
	{
		// the reaction table: die.held(.element) after dying on her feet, else die.element, die.type, die (her last blow);
		// by default from [Deaths]
		const std::string el = ElementName(a.lastElement);
		std::vector<std::string> keys;
		if (a_held) {
			if (!el.empty()) {
				keys.push_back("die.held." + el);
			}
			keys.push_back("die.held");
		} else {
			if (!el.empty()) {
				keys.push_back("die." + el);
			}
			if (a.lastType >= 0) {
				keys.push_back(std::string("die.") + TypeName(a.lastType));
			}
			keys.push_back("die");
		}
		std::string key = "([Deaths])";
		Rule        rule;
		bool        found = false;
		for (const auto& k : keys) {
			if (auto it = rules_.find(k); it != rules_.end()) {
				rule = it->second;
				key = k;
				found = true;
				break;
			}
		}
		if (!found) {
			rule = ParseRule(a_held ? fmt::format("writhe:{}", dyingWrithe_) :
			                          fmt::format("shot:{}, push, catch:{}, writhe:{}", dyingHealth_, dyingCatch_, dyingWrithe_));
		}
		const auto chance = [&](std::string_view n) { return rule.Has(n) ? rule.Value(n, 1.0f) : 0.0f; };
		a.dieAmp = rule.Value("amp", -1.0f);
		a.diePeriod = rule.Value("period", -1.0f);
		logger::info("  dying row [{}: {}]", key, rule.text);
		if (a_held) {
			// she already took the blow on her feet: no new shot or push, her reaction runs on into the fall, then limp
			a.dyingAt = a.age - dyingReact_;
			a.writhe = Rand(0.0f, 1.0f) < chance("writhe");
			a.relaxAt = a.age + Rand(0.8f, 1.6f);
			logger::info("  dying plan (after dying on her feet): {}limp after {:.1f} s", a.writhe ? "writhe, then " : "", a.relaxAt - a.age);
			return;
		}
		// the killing blow as a shot at no health: Delirium's own hit reaction (stagger, clutch, collapse). From the
		// killer to the chest, else along the way the body already moves, else from the front
		float cp[3]{}, cq[4]{};
		dl.nm_agent_get_part_transform(a.agent, kChest, cp, cq);
		V3          dir = a_moving;
		std::string from = "its own motion";
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
			const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
			dir = { dir.x / l, dir.y / l, 0.0f };
		}
		// hit somewhere on the upper body: spine1-3, neck or a clavicle
		constexpr std::array<int, 6> kHitParts{ 8, 9, 10, 11, 13, 17 };
		int part = kHitParts[std::uniform_int_distribution<int>(0, 5)(g_rng)];
		float hp[3]{}, hq[4]{};
		dl.nm_agent_get_part_transform(a.agent, part, hp, hq);
		// killed by an arrow: where it went in and the way it flew
		if (auto self = a.actor.get()) {
			RE::NiPoint3 at, vel;
			if (RecentImpact(self.get(), 1.5f, at, vel) && vel.Length() > 1.0f) {
				const RE::NiPoint3 h = (at - a.origin) * kUnit;
				hp[0] = h.x;
				hp[1] = h.y;
				hp[2] = h.z;
				part = NearestPart(a, hp);
				from = fmt::format("{}, where it went in", from);
			}
		}
		a.dyingAt = a.age;
		const float push = deathPush_ * (rule.Has("push") ? rule.Value("push", 1.0f) : 0.0f);
		if (push > 0.0f && (dir.x != 0.0f || dir.y != 0.0f)) {
			float       comp[3]{}, comv[3]{};
			dl.nm_agent_get_com(a.agent, comp, comv);
			const float along = comv[0] * dir.x + comv[1] * dir.y;
			const V3    perp{ comv[0] - dir.x * along, comv[1] - dir.y * along, 0.0f };
			const float sp = std::max(push * Rand(0.8f, 1.2f), along);  // running away from him: she keeps it
			const V3    shift{ dir.x * sp + perp.x * 0.3f - comv[0], dir.y * sp + perp.y * 0.3f - comv[1], 0.0f };
			if (a.calibrated || a.takeover) {
				a.kicks.push_back({ -1, shift, {} });
				a.speedCap = deathMaxSpeed_;
			} else {
				const float imp[3]{ shift.x * 60.0f, shift.y * 60.0f, 0.0f };  // about her weight
				dl.nm_agent_apply_push(a.agent, cp, imp);
			}
			logger::info("  dying: moving {:.2f} {:.2f} -> {:.2f} {:.2f} m/s (away from {}, {:.1f} m/s)", comv[0], comv[1], comv[0] + shift.x,
				comv[1] + shift.y, from, sp);
		}
		const float d[3]{ dir.x, dir.y, dir.z };
		// badly wounded, not dead on the spot: at no health the shot relaxes every muscle in 0.5 s (the flop). Wounded, she
		// reaches for the wound and her muscles fade over the fall
		if (rule.base == "shot") {
			const float health = rule.Value("shot", dyingHealth_);
			if (dl.nm_agent_task_shot(a.agent, a.world, nullptr, part, hp, d, health) < 0) {
				logger::error("  shot task: {}", LastError());
			} else {
				logger::info("  dying: shot task at health {:.0f} on part {}, hit from {}", health, part, from);
			}
		}
		// now and then the arms go out to break the fall (the shot only catches it at full health)
		const bool catches = Rand(0.0f, 1.0f) < chance("catch");
		if (catches) {
			dl.nm_msg_begin(a.agent, "catchFall");
			dl.nm_msg_set_bool(a.agent, "start", 1);
			dl.nm_msg_send(a.agent, a.world);
		}
		// then: a weak writhe on the ground now and then, and limp after it
		a.writhe = Rand(0.0f, 1.0f) < chance("writhe");
		a.relaxAt = a.age + Rand(limpMin_, limpMax_);
		logger::info("  dying plan: {}{}limp after {:.1f} s", catches ? "breaking the fall, " : "", a.writhe ? "writhe, then " : "", a.relaxAt - a.age);
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
		if (g_held.load(std::memory_order_relaxed) != 0) {
			CheckHeld(a_actor);
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
			if (!a_actor->Get3D(false) || a_actor->IsDisabled()) {
				Release(i, "actor gone");
				return;
			}
			if (a.death && !a_actor->IsDead()) {
				Release(i, "alive again");
				return;
			}
			if (a_actor->IsDead() && !a.death) {
				a.death = true;
				a.knockQueued = true;  // Skyrim's death ragdoll comes on by itself: no knock of ours
				a.writheAt = a.writheUntil = -1.0f;
				a.reelUntil = -1.0f;
				a.burnUntil = a.shockUntil = a.chillUntil = -1.0f;
				a.blend = 1.0f;
				a.blendingOut = false;
				a.settled = 0.0f;
				const bool held = a.heldDeath || heldDied_.contains(a_actor->GetFormID());
				heldDied_.erase(a_actor->GetFormID());
				if (!held) {
					dl.nm_msg_begin(a.agent, "stopAllBehaviours");
					dl.nm_msg_send(a.agent, a.world);
				}
				float cp[3]{}, cv[3]{};
				dl.nm_agent_get_com(a.agent, cp, cv);
				logger::info("{:08X}: died while Delirium has her ({}{}): dying in Delirium", a_actor->GetFormID(),
					a.calibrated ? "in Skyrim's ragdoll" : "on her feet", held ? ", after dying on her feet" : "");
				StartDying(a, { cv[0], cv[1], 0.0f }, held);
			}
			const float dt = std::clamp(a_delta, 0.0f, 0.1f);
			++a.calls;
			a.dtSum += a_delta;
			a.age += dt;
			if (a.death) {
				a.deadFor += dt;
			}
			a.sincePush += dt;
			// the bones were already posed from the last step, inside the animation update (AfterPose)
			// every frame advances by exactly its own time, split into equal steps of at most kStep: a fixed step with
			// an accumulator runs 0 steps one frame and 2 the next, and that stutter shakes CBPC/SMP physics
			if (dt > 0.0f && TorqueMode(a)) {
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
					            dl.nm_agent_get_joint_accels(a.agent, a.acc.data(), 21) == 21 &&
					            dl.nm_agent_get_part_ext_torques(a.agent, a.ext.data(), 21) == 21;
				}
			} else if (dt > 0.0f) {
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
			if (collision_) {
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
				// down and gaining energy second after second: nothing pushes her, so the simulation is running away
				a.keRising = (a.knockQueued && ke > kRunawayKE && a.keLast >= 0.0f && ke > a.keLast * 1.1f) ? a.keRising + 1 : 0;
				a.keLast = ke;
				if (a.filled) {
					logger::info("  {} frames without Skyrim's pose copy: ours put on after its update", a.filled);  // trace
				}
				a.filled = 0;
				a.calls = 0;
				a.poses = 0;
				a.dtSum = 0;
				if (a.haveShot) {
					float pp[3]{}, pq[4]{};
					dl.nm_agent_get_part_transform(a.agent, 0, pp, pq);
					const float mx = pp[0] - a.shotPelvis.x, my = pp[1] - a.shotPelvis.y;
					logger::info("  since the arrow: pelvis {:.2f} m along its flight, {:.2f} m across", mx * a.shotDir.x + my * a.shotDir.y,
						-mx * a.shotDir.y + my * a.shotDir.x);
				}
			}
			a.wasBalancing |= balancing;
			if (a.flying > 0 && (!a.knockQueued || a.takeover)) {
				float cp[3]{}, cv[3]{};
				dl.nm_agent_get_com(a.agent, cp, cv);
				// thrown at something (a wall, a rock, the ground coming up): arms out at it
				if (!a.braced && a.flyMode != 3 && a.age >= a.braceCheckAt) {
					a.braceCheckAt = a.age + 0.1f;
					const float sp = std::sqrt(cv[0] * cv[0] + cv[1] * cv[1] + cv[2] * cv[2]);
					if (sp > 1.0f) {
						const float dir[3]{ cv[0] / sp, cv[1] / sp, cv[2] / sp };
						float       dist = 0.0f, n[3]{};
						if (dl.nm_world_raycast(a.world, cp, dir, 0.5f + 0.35f * sp, &dist, n) > 0) {
							const float pos[3]{ cp[0] + dir[0] * dist, cp[1] + dir[1] * dist, cp[2] + dir[2] * dist };
							a.braced = true;
							if (a.flyMode != 2) {
								dl.nm_msg_begin(a.agent, "armsWindmillAdaptive");
								dl.nm_msg_set_bool(a.agent, "start", 0);
								dl.nm_msg_send(a.agent, a.world);
							}
							dl.nm_msg_begin(a.agent, "braceForImpact");
							dl.nm_msg_set_bool(a.agent, "start", 1);
							dl.nm_msg_set_vec3(a.agent, "pos", pos);
							dl.nm_msg_set_vec3(a.agent, "look", pos);
							if (dl.nm_msg_send(a.agent, a.world) < 0) {
								logger::error("  brace: {}", LastError());
							}
							logger::info("{:08X}: thrown at something {:.1f} m ahead (facing {:.1f} {:.1f} {:.1f}) at {:.1f} m/s: bracing", a_actor->GetFormID(), dist, n[0], n[1],
								n[2], sp);
						}
					}
				}
				// coming down: reach to catch it (highFall catches by itself)
				if (a.flying == 1 && a.age - a.flyAt > 0.25f && (cv[2] < -1.5f || a.age - a.flyAt > 2.5f)) {
					a.flying = 2;
					if (a.flyMode < 2) {
						dl.nm_msg_begin(a.agent, "pedalLegs");
						dl.nm_msg_set_bool(a.agent, "start", 0);
						dl.nm_msg_send(a.agent, a.world);
						if (a.flyMode == 1) {
							dl.nm_msg_begin(a.agent, "bodyRollUp");
							dl.nm_msg_set_bool(a.agent, "start", 0);
							dl.nm_msg_send(a.agent, a.world);
						}
						dl.nm_msg_begin(a.agent, "catchFall");
						dl.nm_msg_set_bool(a.agent, "start", 1);
						dl.nm_msg_send(a.agent, a.world);
					}
					logger::info("{:08X}: coming down {:.1f} m/s after {:.2f} s in the air: catching the fall", a_actor->GetFormID(), -cv[2], a.age - a.flyAt);
				}
			}
			// down: by what took her down, then still, and Skyrim gets her up. Burning: rolling, thrashing at the flames;
			// shocked: convulsing, the current still in her; frozen: curled up, shivering; else writhing or curled up
			if (a.writheAt >= 0.0f && a.age >= a.writheAt) {
				a.writheAt = -1.0f;
				const float o = a.writheOomph;
				const int   el = a.writheElement;
				if (el == kFire) {
					a.writheFor = std::max(a.writheFor, burnTime_ + 0.5f * o);
				}
				a.writheUntil = a.age + a.writheFor;
				// the reaction table: down.element, down (by default from [Down] fFoetalChance)
				std::string key = "(fFoetalChance)";
				const Rule* found = FindRule({ std::string("down.") + ElementName(el), "down" }, key);
				const Rule  rule = found ? *found :
				                           ParseRule(fmt::format("foetal:{}, writhe:{}, roll:{}", foetalChance_, (1.0f - foetalChance_) * 0.6f,
											   (1.0f - foetalChance_) * 0.4f));
				const int   mode = Choose(rule, { "writhe", "roll", "foetal", "still" });  // -1: writhe
				const bool  foetal = mode == 2;
				dl.nm_msg_begin(a.agent, "stopAllBehaviours");
				dl.nm_msg_send(a.agent, a.world);
				if (a.chillUntil >= 0.0f) {
					dl.nm_msg_begin(a.agent, "configureBalanceReset");
					dl.nm_msg_send(a.agent, a.world);
				}
				a.burnUntil = a.shockUntil = a.chillUntil = -1.0f;
				if (mode == 3) {
					dl.nm_msg_begin(a.agent, "bodyRelax");
					dl.nm_msg_set_bool(a.agent, "start", 1);
				} else if (foetal) {
					dl.nm_msg_begin(a.agent, "bodyFoetal");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					dl.nm_msg_set_float(a.agent, "asymmetry", Rand(0.0f, 0.6f));
					dl.nm_msg_set_float(a.agent, "backTwist", Rand(-0.5f, 0.5f));
					dl.nm_msg_set_int(a.agent, "randomSeed", static_cast<int>(Rand(1.0f, 999.0f)));
				} else {
					// amplitude, period: harder, bigger and faster (the row's amp / period fix them: fire 2.2 / 0.4, shock 1.3 / 0.3)
					const float k = rule.Value("amp", 0.8f + 0.6f * o), p = rule.Value("period", 1.1f - 0.4f * o);
					dl.nm_msg_begin(a.agent, "bodyWrithe");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					dl.nm_msg_set_string(a.agent, "mask", "fb");
					dl.nm_msg_set_float(a.agent, "armAmplitude", k);
					dl.nm_msg_set_float(a.agent, "legAmplitude", k);
					dl.nm_msg_set_float(a.agent, "backAmplitude", k);
					dl.nm_msg_set_float(a.agent, "armPeriod", p * Rand(0.85f, 1.15f));
					dl.nm_msg_set_float(a.agent, "legPeriod", p * Rand(0.85f, 1.15f));
					dl.nm_msg_set_float(a.agent, "backPeriod", p * Rand(0.85f, 1.15f));
					dl.nm_msg_set_bool(a.agent, "rollOverFlag", mode == 1);
				}
				if (dl.nm_msg_send(a.agent, a.world) < 0) {
					logger::error("  down: {}", LastError());
				}
				// the current, or the cold's shiver, on top
				const bool jolt = rule.Has("shock");
				if (jolt || rule.Has("shiver")) {
					dl.nm_msg_begin(a.agent, "electrocute");
					dl.nm_msg_set_bool(a.agent, "start", 1);
					dl.nm_msg_set_float(a.agent, "stunMag", jolt ? Rand(0.4f, 0.55f) : 0.12f);
					dl.nm_msg_set_float(a.agent, "stunInterval", jolt ? 0.07f : 0.05f);
					dl.nm_msg_set_float(a.agent, "largeMult", jolt ? 3.0f : 1.5f);
					dl.nm_msg_set_float(a.agent, "directionRandomness", 0.5f);
					dl.nm_msg_send(a.agent, a.world);
				}
				static constexpr const char* kHow[]{ "writhing", "rolling", "curled up", "lying still" };
				logger::info("{:08X}: down: [{}: {}] {}{} for {:.1f} s", a_actor->GetFormID(), key, rule.text, kHow[mode < 0 ? 0 : mode],
					jolt ? ", convulsing" : rule.Has("shiver") ? ", shivering" : "", a.writheFor);
			}
			if (a.writheUntil >= 0.0f && a.age >= a.writheUntil) {
				a.writheUntil = -1.0f;
				dl.nm_msg_begin(a.agent, "stopAllBehaviours");
				dl.nm_msg_send(a.agent, a.world);
				dl.nm_msg_begin(a.agent, "bodyRelax");
				dl.nm_msg_set_bool(a.agent, "start", 1);
				dl.nm_msg_send(a.agent, a.world);
				logger::info("{:08X}: down: still now", a_actor->GetFormID());
			}
			if (a.reelUntil >= 0.0f && a.age >= a.reelUntil) {
				// the reel is over: she catches herself (the balance task), the element still running goes on over it
				a.reelUntil = -1.0f;
				dl.nm_msg_begin(a.agent, "staggerFall");
				dl.nm_msg_set_bool(a.agent, "start", 0);
				dl.nm_msg_send(a.agent, a.world);
				if (!a.knockQueued && !fallen) {
					if (dl.nm_agent_task_balance(a.agent, a.world, nullptr, nullptr) < 0) {
						logger::error("  balance task: {}", LastError());
					}
					Overlays(a, a.shotDir);
				}
				logger::info("{:08X}: the reel is over{}", a_actor->GetFormID(), a.knockQueued || fallen ? " (she is down)" : ", catching herself");
			}
			if (a.burnUntil >= 0.0f && a.age >= a.burnUntil) {
				StopBurn(a);
				logger::info("{:08X}: the fire is out", a_actor->GetFormID());
			}
			if (a.shockUntil >= 0.0f && a.age >= a.shockUntil) {
				StopShock(a);
				logger::info("{:08X}: the shock is over", a_actor->GetFormID());
			}
			if (a.chillUntil >= 0.0f && a.age >= a.chillUntil) {
				StopChill(a);
				logger::info("{:08X}: warm again", a_actor->GetFormID());
			}
			if (a.heldDeath && !a.death && a.collapseAt >= 0.0f && a.age >= a.collapseAt) {
				a.collapseAt = -1.0f;
				if (!a.knockQueued && a.flying == 0) {
					// she stops fighting it: the strength leaves her over a second or two, she goes down (and dies there)
					dl.nm_msg_begin(a.agent, "stayUpright");
					dl.nm_msg_set_bool(a.agent, "start", 0);
					dl.nm_msg_send(a.agent, a.world);
					const float period = Rand(1.0f, 2.0f);
					dl.nm_msg_begin(a.agent, "shotRelax");
					dl.nm_msg_set_float(a.agent, "relaxPeriod", period);
					dl.nm_msg_send(a.agent, a.world);
					logger::info("{:08X}: dying on her feet: collapsing over {:.1f} s", a_actor->GetFormID(), period);
				}
			}
			if (a.death && a.calibrated && !a.relaxed) {
				if (a.writhe && !a.writhing && ((fallen && a.age - a.dyingAt >= dyingReact_) || a.age >= a.relaxAt)) {
					a.writhing = true;
					dl.nm_msg_begin(a.agent, "stopAllBehaviours");
					dl.nm_msg_send(a.agent, a.world);
					const float stiff = Rand(9.0f, 12.0f), period = a.diePeriod > 0.0f ? a.diePeriod : Rand(2.0f, 4.0f),
								amp = a.dieAmp >= 0.0f ? a.dieAmp : Rand(0.7f, 1.3f);
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
			// thrown: no balancer runs in the air, so Delirium never calls it a fall; down is her head near the ground
			bool landed = false;
			if (a.flying > 0 && (!a.knockQueued || a.takeover)) {
				float hp[3]{}, hq[4]{};
				dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
				landed = (a.flying == 2 && a.age - a.flyAt > 0.5f && hp[2] < 0.6f) || a.age - a.flyAt > 3.0f;
				if (landed) {
					logger::info("{:08X}: landed after {:.2f} s (head at {:.2f} m)", a_actor->GetFormID(), a.age - a.flyAt, hp[2]);
					if (a.knockQueued) {
						// Skyrim's own throw: she is in its ragdoll already, only her reaction on the ground is left
						a.flying = 0;
						if (a.writheFor > 0.0f && !a.death) {
							a.writheAt = a.age + 0.3f;
						}
					}
				}
			}
			// thrown: only the landing counts (her balance stopped in the air on purpose)
			a.offBalance = a.wasBalancing && !balancing ? a.offBalance + dt : 0.0f;
			// on her feet still (the flag can come on the moment a hard hit lands): only once her head is going down
			float hd[3]{}, hdq[4]{};
			dl.nm_agent_get_part_transform(a.agent, kHead, hd, hdq);
			if (!a.knockQueued && (a.flying > 0 ? landed : ((fallen || a.offBalance > 0.25f) && hd[2] < 1.0f))) {
				logger::info("{:08X}: down after {:.2f} s: {}", a_actor->GetFormID(), a.age,
					a.flying > 0 ? "landed" : fallen ? "Delirium says she fell" : "balance lost for 0.25 s");
				// balance lost: hand the actor to Skyrim's own ragdoll state now, while our pose is still close to the
				// animation Skyrim builds its ragdoll from (no push of its own); DriveRagdoll makes it follow Delirium
				a.knockQueued = true;
				if (a.writheFor > 0.0f && !a.death) {
					a.writheAt = a.age + (a.flying > 0 ? 0.3f : 0.7f);
				}
				// the way she is falling (Delirium's head over its pelvis, flat): the knock's source goes behind her
				float hp[3]{}, pp[3]{}, hq[4]{};
				dl.nm_agent_get_part_transform(a.agent, kHead, hp, hq);
				dl.nm_agent_get_part_transform(a.agent, 0, pp, hq);
				const RE::NiPoint3 lean(hp[0] - pp[0], hp[1] - pp[1], 0.0f);
				g_tasks->AddTask([h = a.actor, lean]() {
					// this runs a frame later: her 3D can be unloaded by then (Skyrim's knock reads it without a check),
					// and we may have let her go already
					if (auto act = h.get(); act && act->Get3D(false) && !act->IsDisabled() && Engine::Get().KeepsDown(act.get())) {
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
					logger::info("  muscles: joints asked for up to {:.2f} rad/s per step ({} at age {:.2f}; {} capped), bodies turned up to {:.2f} rad/s per step, {} spins capped (gain {:.2f}); limbs moving {:.0f} deg/s, behaviours' torques turned up to {:.2f} rad/s per step",
						a.maxWant, rig_[static_cast<std::size_t>(a.wantPart)].name, a.wantAge, a.clamped, a.maxDw, a.spinCapped, gain_,
						a.actN ? a.actSum / a.actN : 0.0f, a.extMax);
					{
						float sw = 0.0f, top = 0.0f;
						for (float s : a.actSwing) {
							sw += s;
							top = std::max(top, s);
						}
						logger::info("  limbs swung on average {:.0f} deg this second (most {:.0f}); Havok counted a body still up to {} frames",
							a.actSwing.empty() ? 0.0f : sw / a.actSwing.size(), top, a.sleepMax);
						a.sleepMax = 0;
						a.actRef.clear();
						a.actSwing.clear();
					}
					a.actSum = 0;
					a.actN = 0;
					a.extMax = 0;
					a.maxWant = 0;
					a.clamped = 0;
					a.spinCapped = 0;
					a.maxDw = 0;
				}
				a.drives = 0;
				a.maxSpeed = 0;
				a.maxRootLag = 0;
			}
			// back to Skyrim's animation once it has caught itself and stood still
			a.calm = (balancing && !fallen && ke < 2.0f && a.sincePush > 1.0f && !a.knockQueued && a.burnUntil < 0.0f && a.shockUntil < 0.0f && a.chillUntil < 0.0f && a.reelUntil < 0.0f) ? a.calm + dt : 0.0f;
			// lying still in Skyrim's ragdoll: let go, Skyrim gets up from this pose on its own
			// (a corpse is held for good: it keeps moving in Delirium until it is really still, so the hold doesn't freeze a slide)
			a.settled = (a.calibrated && ke < (a.death ? 0.05f : 1.0f) && (!a.death || a.relaxed) && a.writheAt < 0.0f && a.writheUntil < 0.0f) ? a.settled + dt : 0.0f;
			// not while Skyrim's stagger still runs underneath: its clip would show in the fade (Delirium plays the stagger,
			// Skyrim keeps only its gameplay: the interrupted attack and the staggered state)
			if (autoRelease_ && a.calm > 1.5f && !a.blendingOut && !a_actor->AsActorState()->IsStaggered()) {
				a.blendingOut = true;  // fade back into Skyrim's animation over kBlendOut, physics still running
			}
			if (a.blendingOut) {
				a.blend -= dt / kBlendOut;
				if (a.blend <= 0.0f) {
					Release(i, "recovered");
					return;
				}
			}
			// one body, Skyrim's ragdoll: once it lies still Skyrim takes over (corpse, or its own get-up)
			if (autoRelease_ && a.settled > (a.death ? 1.0f : 0.15f)) {
				Release(i, a.death ? "settled: the corpse is Skyrim's ragdoll already" : "settled: Skyrim's ragdoll gets up by itself");
				return;
			}
			// a corpse that never lies still (wedged, or under the ground) would be driven for good: Skyrim's ragdoll
			// is already running underneath, so hand it over as it is
			if (autoRelease_ && a.death && a.deadFor > kCorpseHold) {
				Release(i, "corpse held for 30 s: Skyrim's ragdoll takes it");
				return;
			}
			if (autoRelease_ && a.keRising >= kRunawaySeconds) {
				Release(i, "energy kept growing while down: Skyrim's ragdoll takes over");
				return;
			}
			// the frame Skyrim switches to its ragdoll has no pose copy at all (and we only learn that a frame later):
			// while knocked, put our pose on the bones here too, so a frame without a copy still draws ours. Held still
			// too: with only Skyrim's one copy a frame, its own pose (0.1-0.2 m off ours) showed through as a twitch
			if (a.knockQueued && !TorqueMode(a)) {
				if (a.copies == 0) {
					logger::info("  age {:.3f}: the last frame had no pose copy", a.age);
				}
				AfterPose(a_actor, false);
			} else if (!a.knockQueued && a.copies == 0) {
				// upright, Skyrim skipped its pose copy this frame (its animation update ran without one): the bones
				// would hold last frame's pose and the body stutters, so ours go on here
				++a.filled;
				a.filling = true;
				AfterPose(a_actor, false);
				a.filling = false;
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
					Release(i, "Skyrim started its get-up");
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
				return p->knockQueued;
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
				p->haveCom = p->knockQueued && !TorqueMode(*p);
				if (p->haveCom && p->calibrated && p->traceN < 100) {
					logger::info("    copy {:.3f} {}: COM {:.1f} {:.1f} {:.1f}, top {:.1f} {:.1f} {:.1f}", p->age, a_postPhysics ? "post" : "pre ", p->comWorld.translate.x,
						p->comWorld.translate.y, p->comWorld.translate.z, top->world.translate.x, top->world.translate.y, top->world.translate.z);
				}
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
			// Skyrim's animated pose in world space (the bone locals are fresh from the graph), then Delirium's bones over it
			RefreshBones(a);
			if (a_postPhysics && !a.calibrated && a.driver && a_actor->IsInRagdollState()) {
				// the bones were just posed from Skyrim's ragdoll bodies: where each body sits on its bone
				Calibrate(a, a_actor);
				if (a.takeover) {
					InheritVelocity(a);
				}
			}
			if (TorqueMode(a)) {
				// Skyrim's ragdoll is the body: Skyrim poses the bones from it. Not yet through F7's lead-in: Skyrim
				// builds its ragdoll from the animation, not from our pose, and the lead-in first pulls it onto ours
				return;
			}
			std::array<V3, 21> pos;
			std::array<Q, 21>  rot;
			// the live animation only from a real pose copy: on a filler frame the bones still hold our own pose from the last
			// frame, and taking that as the pose to hold let her own fall become the target (she kept leaning into it)
			const bool follow = followAnimation_ && !a.knockQueued && !a.filling;
			if (follow) {
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
				// WriteBones puts COM where the pelvis lands on ours: an offset left below COM would move COM instead
				if (ClearJointFixOffset(a.ref[0], a.com)) {
					UpdateTransforms(a.com);
				}
			}
			// trace, through F7's lead-in: Skyrim's pose (from its bodies) vs ours, relative to the pelvis = the jump the
			// switch to Skyrim's bodies will show
			std::array<RE::NiTransform, 21> sky{}, skyC{};
			const bool                      gap = a_postPhysics && a.calibrated && a.traceN < 100;
			const bool                      dump = gap && a.age - a.calibAge > 0.12f && !a.dumped;
			if (dump) {
				for (std::size_t i = 0; i < 21; ++i) {
					if (a.ref[i]) {
						skyC[i] = a.com->world.Invert() * a.ref[i]->world;
					}
				}
			}
			if (gap) {
				for (std::size_t i = 0; i < 21; ++i) {
					if (a.ref[i]) {
						sky[i] = a.ref[0]->world.Invert() * a.ref[i]->world;
					}
				}
			}
			a.haveSkyCom = a.calibrated && a_postPhysics;
			if (a.haveSkyCom) {
				a.skyComLocal = a.com->local;
			}
			a.limbBlend = a.calibrated ? LeadShare(a) : 1.0f;
			WriteBones(a);
			a.limbBlend = 1.0f;
			for (std::size_t i = 0; i < 21; ++i) {
				if (a.ref[i]) {
					a.written[i] = a.ref[i]->world;
				}
			}
			a.haveWritten = true;
			if (dump) {
				// every bone relative to NPC COM (a body in Skyrim's ragdoll): Skyrim's pose of it vs ours
				a.dumped = true;
				std::string s;
				for (std::size_t i = 0; i < 21; ++i) {
					if (!a.ref[i]) {
						continue;
					}
					const auto  ours = a.com->world.Invert() * a.ref[i]->world;
					const float d = (ours.translate - skyC[i].translate).Length() * a.com->world.scale * kUnit;
					const auto  m = skyC[i].rotate.Transpose() * ours.rotate;
					const float r = std::acos(std::clamp((m.entry[0][0] + m.entry[1][1] + m.entry[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2958f;
					s += fmt::format(" {}={:.3f}m/{:.0f}", rig_[i].name, d, r);
				}
				logger::info("    gap dump {:.3f} (from NPC COM, Skyrim's vs ours):{}", a.age, s);
			}
			if (gap) {
				float       wp = 0, wr = 0;
				std::size_t ip = 0, ir = 0;
				std::string cl;
				for (std::size_t i = 1; i < 21; ++i) {
					if (!a.ref[i]) {
						continue;
					}
					const auto  ours = a.ref[0]->world.Invert() * a.ref[i]->world;
					const float d = (ours.translate - sky[i].translate).Length() * a.ref[0]->world.scale * kUnit;
					const auto  m = sky[i].rotate.Transpose() * ours.rotate;
					const float r = std::acos(std::clamp((m.entry[0][0] + m.entry[1][1] + m.entry[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2958f;
					if (d > wp) {
						wp = d, ip = i;
					}
					if (r > wr) {
						wr = r, ir = i;
					}
					if (i == 13 || i == 17 || i == 14 || i == 18) {
						cl += fmt::format(" {} {:.0f}", rig_[i].name, r);
					}
				}
				logger::info("    gap {:.3f}: Skyrim's pose vs ours (from the pelvis): {:.3f} m ({}), {:.0f} deg ({});{}", a.age, wp, rig_[ip].name, wr, rig_[ir].name, cl);
			}
			// the bones as we just posed them: the bodies' targets until DriveRagdoll's next step sets its own
			for (auto& b : a.bodies) {
				if (a_postPhysics || !b.hasTarget) {
					b.target = Scl({ b.node->world.translate.x, b.node->world.translate.y, b.node->world.translate.z }, kUnit);
					b.targetRot = FromMat(b.node->world.rotate);
					b.hasTarget = true;
				}
			}
			if (follow) {
				// the posture to hold follows the live animation: a guard stays up, a talking NPC keeps gesturing
				// (not once ragdolled: then the "animation" is Skyrim's ragdoll, which is us)
				RecordPose(a, a_actor, pos, rot);
				StandingLegs(a, pos, rot);
				SetZeroPose(a, pos, rot);
			}
			return;
		}
	}

	// The pose as it will be drawn (each bone's world from its locals up to the 3D root, nothing written), once a frame
	// after the root move, compared with the last frame: top node and pelvis moves, the worst limb move and turn relative
	// to the pelvis, and how many limbs turned back against their last frame's turn (shaking)
	void Engine::Trace(Active& a, RE::Actor* a_actor, const char*)
	{
		auto* root3d = a_actor->Get3D(false);
		if (!root3d || !a.knockQueued || a.traceN >= 100 || (a.calibrated && a.age - a.calibAge > 1.0f)) {
			return;
		}
		std::function<RE::NiTransform(RE::NiAVObject*)> world = [&](RE::NiAVObject* n) -> RE::NiTransform {
			if (n == root3d || !n->parent) {
				return n->parent ? n->parent->world * n->local : n->local;
			}
			return world(n->parent) * n->local;
		};
		const RE::NiTransform           top = world(root3d);
		std::array<RE::NiTransform, 21> bone{};
		std::array<RE::NiMatrix3, 21>   rel{};
		for (std::size_t i = 0; i < 21; ++i) {
			if (a.ref[i]) {
				bone[i] = world(a.ref[i]);
			}
		}
		for (std::size_t i = 0; i < 21; ++i) {
			rel[i] = bone[0].rotate.Transpose() * bone[i].rotate;
		}
		{
			// trace: every drawn bone's world position per frame (m), for plotting
			std::ofstream f(*SKSE::log::log_directory() / "SkyrimDelirium_trace.csv", a.haveTrace ? std::ios::app : std::ios::trunc);
			if (!a.haveTrace) {
				f << "age,torque,steps";
				for (std::size_t i = 0; i < 21; ++i) {
					f << ',' << rig_[i].name << "_x," << rig_[i].name << "_y," << rig_[i].name << "_z";
				}
				f << ",COM_x,COM_y,COM_z\n";
			}
			f << fmt::format("{:.4f},{},{}", a.age, TorqueMode(a) ? 1 : 0, a.physSteps - a.trSteps);
			for (std::size_t i = 0; i < 21; ++i) {
				const auto& p = bone[i].translate;
				f << fmt::format(",{:.4f},{:.4f},{:.4f}", p.x * kUnit, p.y * kUnit, p.z * kUnit);
			}
			const auto c = world(a.com).translate;
			f << fmt::format(",{:.4f},{:.4f},{:.4f}\n", c.x * kUnit, c.y * kUnit, c.z * kUnit);
		}
		auto angle = [](const RE::NiMatrix3& x, const RE::NiMatrix3& y) {
			const auto  d = x.Transpose() * y;
			const float tr = d.entry[0][0] + d.entry[1][1] + d.entry[2][2];
			return std::acos(std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2958f;
		};
		std::array<RE::NiPoint3, 21> spin{};
		if (a.haveTrace) {
			const RE::NiPoint3 pelvisMove = bone[0].translate - a.trBone[0].translate;
			float              worst = 0, worstRot = 0;
			std::size_t        wi = 0, wr = 0;
			int                back = 0;
			std::string        backs;
			for (std::size_t i = 1; i < 21; ++i) {
				if (!a.ref[i]) {
					continue;
				}
				const float d = (bone[i].translate - a.trBone[i].translate - pelvisMove).Length();
				if (d > worst) {
					worst = d, wi = i;
				}
				const auto  m = a.trRel[i].Transpose() * rel[i];
				const float r = angle(a.trRel[i], rel[i]);
				if (r > worstRot) {
					worstRot = r, wr = i;
				}
				// turn axis in the pelvis frame (rel_prev * m's axis), so frames compare
				const RE::NiPoint3 ax = a.trRel[i] * RE::NiPoint3(m.entry[2][1] - m.entry[1][2], m.entry[0][2] - m.entry[2][0], m.entry[1][0] - m.entry[0][1]);
				spin[i] = ax;
				const auto& pv = a.trSpin[i];
				if (r > 4.0f && pv.Length() > 0.07f && ax.Dot(pv) < 0.0f) {
					++back;
					backs += ' ';
					backs += rig_[i].name;
				}
			}
			const char* phase = !a.calibrated ? "ours" : TorqueMode(a) ? (a.following ? "torque" : "torque-wait") : "lead";
			float       comSpeed = -1.0f;
			for (auto& b : a.bodies) {
				if (b.node == a.com) {
					const auto* lv = reinterpret_cast<const float*>(&b.body->motion.linearVelocity);
					comSpeed = std::sqrt(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
				}
			}
			const bool pelvisBack = pelvisMove.Dot(a.trPelvisMove) < 0.0f;
			float      off = 0;
			if (a.haveWritten && !TorqueMode(a)) {
				for (std::size_t i = 0; i < 21; ++i) {
					if (a.ref[i]) {
						off = std::max(off, (bone[i].translate - a.written[i].translate).Length() * kUnit);
					}
				}
			}
			logger::info("    trace {:.3f} {:<11} drawn off ours {:.3f} m, {} steps, COM body {:.1f} m/s; top {:.3f} m {:.1f} deg (lag {:.2f}), pelvis {:.3f} m{}, limb {:.3f} m ({}), turn {:.1f} deg ({}), {} turned back{}",
				a.age, phase, off, a.physSteps - a.trSteps, comSpeed, (top.translate - a.trTop.translate).Length() * kUnit, angle(a.trTop.rotate, top.rotate),
				a.rootLag, pelvisMove.Length() * kUnit, pelvisBack ? " BACK" : "", worst * kUnit, rig_[wi].name, worstRot, rig_[wr].name, back, backs);
			a.trPelvisMove = pelvisMove;
		}
		a.trSteps = a.physSteps;
		a.trTop = top;
		a.trBone = bone;
		a.trRel = rel;
		a.trSpin = spin;
		a.haveTrace = true;
		++a.traceN;
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
		{
			// the ragdoll-to-animation mapper: what Skyrim's pose copy draws from each body
			auto*       ch = a.driver->character;
			auto*       setup = ch ? ch->setup.get() : nullptr;
			const auto* m = setup ? reinterpret_cast<const SkeletonMapper*>(setup->ragdollToAnimationSkeletonMapper.get()) : nullptr;
			auto*       rd = a.driver->ragdoll;
			const auto* rs = rd ? rd->skeleton.get() : nullptr;
			std::string drawLog;
			if (m && rs && (m->skeletonA == rs || m->skeletonB == rs)) {
				const bool ragIsA = m->skeletonA == rs;
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
						if ((ragIsA ? sm.boneA : sm.boneB) != rb) {
							continue;
						}
						Q  r{ sm.r[0], sm.r[1], sm.r[2], sm.r[3] };
						V3 t = Scl({ sm.t[0], sm.t[1], sm.t[2] }, kUnit);
						if (!ragIsA) {  // body = bone * (r, t): invert it
							r = Conj(r);
							t = Rot(r, Scl(t, -1.0f));
						}
						l.drawR = r;
						l.drawT = t;
						l.haveDraw = true;
						// against the inverse of the other mapper
						const Q  ir = Conj(l.mapR);
						const V3 it = Rot(ir, Scl(l.mapT, -1.0f));
						drawLog += fmt::format(" {} {:.3f} m {:.1f} deg", l.node->name.c_str(), [](const V3& d) { return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); }(Sub(t, it)), Angle(r, ir) * 57.2958f);
						break;
					}
				}
				logger::info("{:08X}: ragdoll-to-animation mapper ({} is the ragdoll), off the other's inverse by:{}", a_actor->GetFormID(), ragIsA ? "A" : "B", drawLog);
			} else {
				logger::info("{:08X}: no ragdoll-to-animation mapper", a_actor->GetFormID());
			}
		}
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
		// Skyrim's ragdoll is the body (its own mass, gravity, collision); DriveRagdoll only moves it
		a.calibrated = true;
		a.calibAge = a.age;
		logger::info("{:08X}: Skyrim ragdoll has {} bodies, {} on bones:{}", a_actor->GetFormID(),
			a.driver->ragdoll->rigidBodies.size(), a.bodies.size(), list);
		// trace: the joints' own friction and motors (they may eat the behaviours' small steady push)
		{
			std::string js;
			int         friction = 0, motors = 0;
			for (auto* ci : a.driver->ragdoll->constraints) {
				auto* d = ci ? ci->data : nullptr;
				if (!d) {
					continue;
				}
				const auto ty = d->GetType();
				if (ty == 7) {
					auto& at = static_cast<RE::hkpRagdollConstraintData*>(d)->atoms;
					const bool m = at.ragdollMotors.enabled;
					js += fmt::format(" ragdoll(friction {}{:.1f}, motors {}{})", at.angFriction.enabled ? "" : "off ", at.angFriction.maxFrictionTorque,
						m ? "on" : "off", at.ragdollMotors.motors[0] ? fmt::format(" type {}", static_cast<int>(at.ragdollMotors.motors[0]->type.underlying())) : "");
					friction += at.angFriction.enabled && at.angFriction.maxFrictionTorque > 0.0f;
					motors += m;
				} else if (ty == 2) {
					auto& at = static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms;
					const bool m = at.angMotor.enabled;
					js += fmt::format(" hinge(friction {}{:.1f}, motor {}{})", at.angFriction.enabled ? "" : "off ", at.angFriction.maxFrictionTorque,
						m ? "on" : "off", at.angMotor.motor ? fmt::format(" type {}", static_cast<int>(at.angMotor.motor->type.underlying())) : "");
					friction += at.angFriction.enabled && at.angFriction.maxFrictionTorque > 0.0f;
					motors += m;
				} else {
					js += fmt::format(" type{}", ty);
				}
			}
			logger::info("{:08X}: Skyrim ragdoll joints: {}, {} with friction, {} with motors on:{}", a_actor->GetFormID(),
				a.driver->ragdoll->constraints.size(), friction, motors, js);
		}
		EaseJoints(a);
		{
			std::string bs;
			for (auto* body : a.driver->ragdoll->rigidBodies) {
				if (body) {
					const auto& ms = body->motion.motionState;
					bs += fmt::format(" {:.2f}/{:.2f}/{}", static_cast<float>(ms.linearDamping), static_cast<float>(ms.angularDamping), ms.maxAngularVelocity.value);
				}
			}
			logger::info("{:08X}: Skyrim ragdoll bodies' damping (linear/angular/max spin code):{}", a_actor->GetFormID(), bs);
			std::string ms;
			for (auto* body : a.driver->ragdoll->rigidBodies) {
				if (body) {
					ms += fmt::format(" {:.2f}/{:.2f}/{}", body->material.friction, body->material.restitution,
						body->motion.motionState.deactivationClass);
				}
			}
			logger::info("{:08X}: Skyrim ragdoll bodies' friction/bounce/sleep class:{}", a_actor->GetFormID(), ms);
			std::string ls;
			for (auto* ci : a.driver->ragdoll->constraints) {
				auto* d = ci ? ci->data : nullptr;
				if (!d) {
					continue;
				}
				std::string who = "?", dlim;
				for (const auto& b : a.bodies) {
					if (b.body && reinterpret_cast<RE::hkpEntity*>(b.body) == ci->entities[0]) {
						who = b.node ? b.node->name.c_str() : "?";
						if (b.mapped && b.rotPart > 0) {
							float lim[3]{};
							const int kind = dl.nm_agent_get_joint(a.agent, b.rotPart, nullptr, nullptr, nullptr, lim, nullptr);
							dlim = kind == 2 ? fmt::format(" | Delirium {} hinge {:.0f}..{:.0f}", rig_[static_cast<std::size_t>(b.rotPart)].name, lim[0] * 57.3f, lim[1] * 57.3f) :
							       kind == 1 ? fmt::format(" | Delirium {} lean +-{:.0f}/+-{:.0f} twist +-{:.0f}", rig_[static_cast<std::size_t>(b.rotPart)].name, lim[0] * 57.3f,
							                       lim[1] * 57.3f, lim[2] * 57.3f) : "";
						}
						break;
					}
				}
				ls += fmt::format("\n    {}:", who);
				if (d->GetType() == 7) {
					auto& at = static_cast<RE::hkpRagdollConstraintData*>(d)->atoms;
					ls += fmt::format(" ragdoll(twist {:.0f}..{:.0f}, cone {:.0f}, planes {:.0f}..{:.0f}, stab {})", at.twistLimit.minAngle * 57.3f,
						at.twistLimit.maxAngle * 57.3f, at.coneLimit.maxAngle * 57.3f, at.planesLimit.minAngle * 57.3f, at.planesLimit.maxAngle * 57.3f,
						at.setupStabilization.enabled);
					ls += dlim;
				} else if (d->GetType() == 2) {
					auto& at = static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms;
					ls += fmt::format(" hinge({:.0f}..{:.0f}, stab {})", at.angLimit.minAngle * 57.3f, at.angLimit.maxAngle * 57.3f,
						at.setupStabilization.enabled);
					ls += dlim;
				}
			}
			logger::info("{:08X}: Skyrim ragdoll joint limits (deg):{}", a_actor->GetFormID(), ls);
		}
		DumpRagdoll(a, a_actor);
	}

	// Skyrim's ragdoll joints for the offline test body (fit-test): each constraint's Havok frames (in its bodies' frames),
	// each body's rotation in its Delirium part (rig offset * Skyrim's mapping) and the limits. One file per new set of limits.
	void Engine::DumpRagdoll(const Active& a, RE::Actor* a_actor)
	{
		if (!a.driver || !a.driver->ragdoll || ragdollDumps_.size() >= 12) {
			return;
		}
		const auto body = [&](RE::hkpEntity* e) -> const BodyLink* {
			for (const auto& b : a.bodies) {
				if (b.body && static_cast<RE::hkpEntity*>(b.body) == e) {
					return &b;
				}
			}
			return nullptr;
		};
		const auto q = [](const Q& r) { return fmt::format("[{:.6f},{:.6f},{:.6f},{:.6f}]", r.x, r.y, r.z, r.w); };
		const auto cols = [](const RE::hkTransform& t) {
			const auto* f = reinterpret_cast<const float*>(&t);
			return fmt::format("[[{:.6f},{:.6f},{:.6f}],[{:.6f},{:.6f},{:.6f}],[{:.6f},{:.6f},{:.6f}]]", f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9], f[10]);
		};
		const auto side = [&](const BodyLink* b) {
			if (!b) {
				return std::string("null");
			}
			const bool ok = b->mapped && b->rotPart >= 0;
			return fmt::format("{{\"bone\":\"{}\",\"part\":{},\"part_ref\":\"{}\",\"inv_inertia_mass\":[{:.6g},{:.6g},{:.6g},{:.6g}],\"body_in_part\":{}}}", b->node ? b->node->name.c_str() : "?", ok ? b->rotPart : -1,
				ok ? rig_[static_cast<std::size_t>(b->rotPart)].ref : std::string("?"), b->massInv[0], b->massInv[1], b->massInv[2], b->massInv[3],
				ok ? q(Mul(rig_[static_cast<std::size_t>(b->rotPart)].offset, b->mapR)) : "null");
		};
		std::string js, sig;
		for (auto* ci : a.driver->ragdoll->constraints) {
			auto* d = ci ? ci->data : nullptr;
			if (!d || (d->GetType() != 7 && d->GetType() != 2)) {
				continue;
			}
			std::string lim, fa, fb;
			if (d->GetType() == 7) {
				auto& at = static_cast<RE::hkpRagdollConstraintData*>(d)->atoms;
				lim = fmt::format("\"type\":\"ragdoll\",\"twist\":[{:.5f},{:.5f}],\"cone\":{:.5f},\"planes\":[{:.5f},{:.5f}]", at.twistLimit.minAngle,
					at.twistLimit.maxAngle, at.coneLimit.maxAngle, at.planesLimit.minAngle, at.planesLimit.maxAngle);
				fa = cols(at.transforms.transformA);
				fb = cols(at.transforms.transformB);
			} else {
				auto& at = static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms;
				lim = fmt::format("\"type\":\"hinge\",\"angle\":[{:.5f},{:.5f}]", at.angLimit.minAngle, at.angLimit.maxAngle);
				fa = cols(at.transforms.transformA);
				fb = cols(at.transforms.transformB);
			}
			sig += lim;
			js += fmt::format("{}\n  {{{},\"child\":{},\"parent\":{},\"frame_child\":{},\"frame_parent\":{}}}", js.empty() ? "" : ",", lim,
				side(body(ci->entities[0])), side(body(ci->entities[1])), fa, fb);
		}
		if (!ragdollDumps_.insert(sig).second) {
			return;
		}
		const auto* base = a_actor->GetActorBase();
		const auto  name = fmt::format("SkyrimDelirium_ragdoll_{:08X}.json", a_actor->GetFormID());
		std::ofstream(*SKSE::log::log_directory() / name) << fmt::format(
			"{{\"format\":\"skyrim-ragdoll-1\",\"actor\":\"{:08X}\",\"female\":{},\"race\":\"{:08X}\",\"scale\":{:.4f},\n \"note\":\"frame columns: ragdoll twist/planes/cross, hinge axle/perp/perp2; body_in_part: the body's rotation in the Delirium part (xyzw)\",\n \"joints\":[{}\n]}}\n",
			a_actor->GetFormID(), base && base->IsFemale(), a_actor->GetRace() ? a_actor->GetRace()->GetFormID() : 0u, a.scale, js);
		logger::info("{:08X}: wrote {} (Skyrim's ragdoll joints for the test body)", a_actor->GetFormID(), name);
	}

	namespace
	{
		RE::hkpAngFrictionConstraintAtom* FrictionOf(RE::hkpConstraintData* d)
		{
			switch (d->GetType()) {
			case 7:
				return &static_cast<RE::hkpRagdollConstraintData*>(d)->atoms.angFriction;
			case 2:
				return &static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms.angFriction;
			default:
				return nullptr;
			}
		}
	}

	// Skyrim's ragdoll joints carry friction (10 N m on most): it takes ~3 rad/s a step out of a forearm, more than the
	// behaviours' steady push, so a writhe or a shock died after the landing. Off while Delirium drives, back on release.
	void Engine::EaseJoints(Active& a)
	{
		if (friction_ >= 1.0f || !a.eased.empty() || !a.driver || !a.driver->ragdoll) {
			return;
		}
		int n = 0;
		for (auto* ci : a.driver->ragdoll->constraints) {
			auto* d = ci ? ci->data : nullptr;
			auto* f = d ? FrictionOf(d) : nullptr;
			if (!f) {
				continue;
			}
			auto it = eased_.find(d);
			if (it == eased_.end()) {
				if (f->maxFrictionTorque <= 0.0f) {
					continue;
				}
				it = eased_.emplace(d, std::pair{ f->maxFrictionTorque, 0 }).first;
				f->maxFrictionTorque = it->second.first * friction_;
			}
			++it->second.second;
			d->AddReference();
			a.eased.push_back(d);
			++n;
		}
		logger::info("  joint friction x{:.2f} on {} joints while Delirium drives", friction_, n);
	}

	void Engine::RestoreJoints(Active& a)
	{
		for (auto* d : a.eased) {
			if (auto it = eased_.find(d); it != eased_.end() && --it->second.second <= 0) {
				if (auto* f = FrictionOf(d)) {
					f->maxFrictionTorque = it->second.first;
				}
				eased_.erase(it);
			}
			d->RemoveReference();
		}
		a.eased.clear();
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

	float Engine::LeadShare(const Active& a) const
	{
		if (!a.calibrated || a.takeover) {
			return a.calibrated ? 0.0f : 1.0f;
		}
		return std::clamp(1.0f - (a.age - a.calibAge) / 0.15f, 0.0f, 1.0f);
	}

	bool Engine::TorqueMode(const Active& a) const
	{
		// F7: Delirium leads the first 0.15 s (the push is Delirium's), then follows
		return a.calibrated && (a.takeover || a.age - a.calibAge >= 0.15f);
	}

	// which of Skyrim's bodies each Delirium part's drive lands on: the body it gives its rotation to, else its position,
	// else the body riding on it; parts with no body (clavicles) go to the nearest body up the chain
	void Engine::MapParts(Active& a)
	{
		a.partBody.fill(-1);
		int pivots = 0;
		for (auto& b : a.bodies) {
			b.havePivot = false;
		}
		if (a.driver && a.driver->ragdoll) {
			for (auto* ci : a.driver->ragdoll->constraints) {
				auto* d = ci ? ci->data : nullptr;
				if (!d || (d->GetType() != 7 && d->GetType() != 2)) {
					continue;
				}
				const auto& tA = d->GetType() == 7 ? static_cast<RE::hkpRagdollConstraintData*>(d)->atoms.transforms.transformA :
				                                     static_cast<RE::hkpLimitedHingeConstraintData*>(d)->atoms.transforms.transformA;
				const auto* t = reinterpret_cast<const float*>(&tA.translation);
				for (auto& b : a.bodies) {
					if (b.body && reinterpret_cast<RE::hkpEntity*>(b.body) == ci->entities[0]) {
						b.pivot = { t[0], t[1], t[2] };
						b.havePivot = true;
						++pivots;
						break;
					}
				}
			}
		}
		logger::info("  {} of Skyrim's bodies have their joint's pivot: muscles turn them about it{}", pivots, aboutJoint_ ? "" : " (off)");
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
		if (matchMass_ && a.weighed.empty()) {
			a.weighed.assign(a.bodies.size(), nullptr);
			int n = 0;
			for (std::size_t k = 0; k < a.bodies.size(); ++k) {
				auto&       b = a.bodies[k];
				const float hm = b.massInv[3] > 0 ? 1.0f / b.massInv[3] : 0.0f;
				if (!b.body || dm[k] < 0.05f || hm <= 0.0f) {
					continue;
				}
				// inverse mass and inertia scale together: same shape, Delirium's weight
				const float r = hm / dm[k];
				float       mi[4];
				for (int e = 0; e < 4; ++e) {
					mi[e] = b.massInv[e] * r;
				}
				b.body->AddReference();
				std::memcpy(&b.body->motion.inertiaAndMassInv, mi, sizeof(mi));
				a.weighed[k] = b.body;
				++n;
				if (matchContact_) {
					// Delirium's grip (feet 1, hands 2.25, else 0.75) and bounce (0.4): Skyrim's 2.5 grip held her limbs to the
					// ground, its 0.8-1.2 bounce kicked them off it
					if (a.material.size() != a.bodies.size()) {
						a.material.assign(a.bodies.size(), { 0.0f, 0.0f });
					}
					a.material[k] = { b.body->material.friction, b.body->material.restitution };
					const std::string nm = b.node ? b.node->name.c_str() : "";
					b.body->material.friction = nm.find("Foot") != std::string::npos ? 1.0f : nm.find("Hand") != std::string::npos ? 2.25f : 0.75f;
					b.body->material.restitution = 0.4f;
				}
			}
			logger::info("  {} of Skyrim's bodies weigh{} as Delirium's parts while it drives", n, matchContact_ ? ", grip and bounce" : "");
		}
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
			V3 want = Scl({ ac[0], ac[1], ac[2] }, h);
			if (damp_ > 0.0f) {
				// tensed muscles: the joint's spin (child vs parent) is cut by damp_ this step. Skyrim's joints have almost
				// no damping of their own, so limbs bounce on landing
				const auto* wc = reinterpret_cast<const float*>(&a.bodies[c].body->motion.angularVelocity);
				const auto* wp = reinterpret_cast<const float*>(&a.bodies[pb].body->motion.angularVelocity);
				want = Sub(want, Scl({ wc[0] - wp[0], wc[1] - wp[1], wc[2] - wp[2] }, damp_));
			}
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
			const V3 wc = mul(inv[c], t), wp = Scl(mul(inv[pb], t), -1.0f);
			dw[c] = Add(dw[c], wc);
			dw[pb] = Add(dw[pb], wp);
			if (aboutJoint_ && a.bodies[c].havePivot) {
				auto cross = [](const V3& x, const V3& y) { return V3{ x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x }; };
				const auto& mc = a.bodies[c].body->motion.motionState;
				const auto* oc = reinterpret_cast<const float*>(&mc.transform.translation);
				const auto* qc = reinterpret_cast<const float*>(&mc.sweptTransform.rotation1);
				const V3    piv = Add({ oc[0], oc[1], oc[2] }, Rot(Q{ qc[0], qc[1], qc[2], qc[3] }, a.bodies[c].pivot));
				const auto* cc = reinterpret_cast<const float*>(&mc.sweptTransform.centerOfMass1);
				const auto* cp = reinterpret_cast<const float*>(&a.bodies[pb].body->motion.motionState.sweptTransform.centerOfMass1);
				dv[c] = Add(dv[c], cross(wc, Sub({ cc[0], cc[1], cc[2] }, piv)));
				dv[pb] = Add(dv[pb], cross(wp, Sub({ cp[0], cp[1], cp[2] }, piv)));
			}
		}
		for (int p = 0; p < 21; ++p) {
			const int    k = a.partBody[p];
			const float* e = a.ext.data() + p * 3;
			if (k >= 0 && std::isfinite(e[0] + e[1] + e[2])) {
				V3          turn = Scl(mul(inv[k], { e[0], e[1], e[2] }), h);
				const float tl = len(turn);
				a.extMax = std::max(a.extMax, tl);
				if (tl > kMaxExtTurn) {
					turn = Scl(turn, kMaxExtTurn / tl);
				}
				dw[k] = Add(dw[k], turn);
			}
		}
		for (int p = 0; p < 21; ++p) {
			const int    k = a.partBody[p];
			const float* d = a.drv.data() + p * 6;
			if (k >= 0 && std::isfinite(d[0] + d[1] + d[2])) {
				const float im = reinterpret_cast<const float*>(&a.bodies[k].body->motion.inertiaAndMassInv)[3];
				dv[k] = Add(dv[k], Scl({ d[0], d[1], d[2] }, im * h));
			}
		}
		// blows on her while down: the push on the body it hit, and the spin about its centre from where it hit
		std::vector<V3> kv(n);
		for (const auto& kk : a.kicks) {
			if (kk.part < 0) {
				for (std::size_t k = 0; k < n; ++k) {
					kv[k] = Add(kv[k], kk.j);
				}
				continue;
			}
			const int bi = kk.part < 21 ? a.partBody[kk.part] : -1;  // mapped by now (a body taken back is mapped after the blow)
			if (bi < 0 || static_cast<std::size_t>(bi) >= n) {
				continue;
			}
			const auto  k = static_cast<std::size_t>(bi);
			const float im = reinterpret_cast<const float*>(&a.bodies[k].body->motion.inertiaAndMassInv)[3];
			kv[k] = Add(kv[k], Scl(kk.j, im));
			const V3 tq{ kk.arm.y * kk.j.z - kk.arm.z * kk.j.y, kk.arm.z * kk.j.x - kk.arm.x * kk.j.z, kk.arm.x * kk.j.y - kk.arm.y * kk.j.x };
			dw[k] = Add(dw[k], mul(inv[k], tq));
		}
		a.kicks.clear();
		const float cap = a.speedCap;  // once: her death's spikes
		a.speedCap = -1.0f;
		for (std::size_t k = 0; k < n; ++k) {
			auto& b = a.bodies[k];
			auto& mo = b.body->motion;
			V3    v = dv[k];
			if (const float l = len(v); l > 3.0f) {
				v = Scl(v, 3.0f / l);
			}
			v = Add(v, kv[k]);
			a.maxDw = std::max(a.maxDw, len(dw[k]));
			if (k == 0) {
				// trace: how much the limbs visibly move (mean spin of every body against the pelvis body), as fit-test layers measures it
				const int pk = a.partBody[0];
				if (pk >= 0) {
					const auto* w0 = reinterpret_cast<const float*>(&a.bodies[pk].body->motion.angularVelocity);
					float       sum = 0.0f;
					int         cnt = 0;
					for (std::size_t j = 0; j < n; ++j) {
						if (static_cast<int>(j) == pk) {
							continue;
						}
						const auto* wj = reinterpret_cast<const float*>(&a.bodies[j].body->motion.angularVelocity);
						sum += len({ wj[0] - w0[0], wj[1] - w0[1], wj[2] - w0[2] });
						++cnt;
					}
					if (cnt) {
						a.actSum += sum / cnt * 57.3f;
						++a.actN;
					}
					// trace: how far each limb actually swings (a jitter spins fast and goes nowhere)
					const auto* q0c = reinterpret_cast<const float*>(&a.bodies[pk].body->motion.motionState.sweptTransform.rotation1);
					const Q     q0i = Conj(Q{ q0c[0], q0c[1], q0c[2], q0c[3] });
					const bool  fresh = a.actRef.size() != n;
					if (fresh) {
						a.actRef.assign(n, Q{});
						a.actSwing.assign(n, 0.0f);
					}
					for (std::size_t j = 0; j < n; ++j) {
						const auto* qc = reinterpret_cast<const float*>(&a.bodies[j].body->motion.motionState.sweptTransform.rotation1);
						const Q     r = Mul(q0i, Q{ qc[0], qc[1], qc[2], qc[3] });
						if (fresh) {
							a.actRef[j] = r;
							continue;
						}
						a.sleepMax = std::max<int>(a.sleepMax, a.bodies[j].body->motion.deactivationNumInactiveFrames[0]);
						const Q&    f = a.actRef[j];
						const float d = std::min(1.0f, std::abs(f.x * r.x + f.y * r.y + f.z * r.z + f.w * r.w));
						a.actSwing[j] = std::max(a.actSwing[j], 2.0f * std::acos(d) * 57.3f);
					}
				}
			}
			auto*    lv = reinterpret_cast<float*>(&mo.linearVelocity);
			auto*    av = reinterpret_cast<float*>(&mo.angularVelocity);
			V3 nl{ lv[0] + v.x, lv[1] + v.y, lv[2] + v.z };
			if (cap > 0.0f) {
				if (const float s = len(nl); s > cap) {
					nl = Scl(nl, cap / s);
				}
			}
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
			if (!a_after) {
				++a.physSteps;
			}
			if (!a.calibrated || !a_driver->ragdoll) {
				return;
			}
			if (!a_after && TorqueMode(a)) {
				if (a.following) {
					ApplyTorques(a, a_dt);
					++a.drives;
				} else {
					// Delirium starts following on the next frame: until then Skyrim's ragdoll runs on its own
					for (auto& b : a.bodies) {
						b.hasTarget = false;
					}
				}
				return;
			}
			if (a_after) {
				// Skyrim's own driver has run: our velocities go back on
				float       over = 0;
				std::string overBody;
				for (auto& b : a.bodies) {
					if (!b.hasTarget) {
						continue;
					}
					auto* lv = reinterpret_cast<float*>(&b.body->motion.linearVelocity);
					auto* av = reinterpret_cast<float*>(&b.body->motion.angularVelocity);
					const float o = std::sqrt((lv[0] - b.lin.x) * (lv[0] - b.lin.x) + (lv[1] - b.lin.y) * (lv[1] - b.lin.y) + (lv[2] - b.lin.z) * (lv[2] - b.lin.z));
					if (o > over) {
						over = o;
						overBody = b.node ? b.node->name.c_str() : "?";
					}
					lv[0] = b.lin.x, lv[1] = b.lin.y, lv[2] = b.lin.z, lv[3] = 0;
					av[0] = b.ang.x, av[1] = b.ang.y, av[2] = b.ang.z, av[3] = 0;
				}
				if (!TorqueMode(a) && a.traceN < 100) {
					logger::info("    kf {:.3f}: Skyrim's driver changed a body's velocity by up to {:.2f} m/s ({})", a.age, over, overBody);
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
			// F7's first 0.15 s (the push is Delirium's): the velocities that carry each body onto Delirium's pose in one step
			const float inv = 1.0f / std::max(a_dt, 1.0f / 240.0f);
			{
				// trace: how far each body ended from where the last step sent it
				float       miss = 0;
				std::string missBody;
				for (auto& b : a.bodies) {
					if (!b.haveAim) {
						continue;
					}
					const auto* cw = reinterpret_cast<const float*>(&b.body->motion.motionState.sweptTransform.centerOfMass1);
					const V3    d = Sub({ cw[0], cw[1], cw[2] }, b.aimCom);
					const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
					if (l > miss) {
						miss = l;
						missBody = b.node ? b.node->name.c_str() : "?";
					}
				}
				if (a.traceN < 100) {
					logger::info("    kf {:.3f}: dt {:.4f}, bodies ended up to {:.3f} m from where the last step sent them ({})", a.age, a_dt, miss, missBody);
				}
			}
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
				b.aimCom = com;
				b.haveAim = true;
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
				// the cap only guards against garbage
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
			const float w = a.blend * a.limbBlend;
			node->local.rotate = w >= 1.0f ? ours : ToMat(Slerp(FromMat(node->local.rotate), FromMat(ours), std::max(w, 0.0f)));
			node->UpdateWorldData(&ud);
			done[i] = true;
		}
		// everything hanging off the driven bones (twist bones, weapons, ...) follows
		UpdateTransforms(top);
	}
}
