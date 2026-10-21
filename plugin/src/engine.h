#pragma once

#define DELIRIUM_PHYSICS_STATIC
#include "delirium_physics.h"

namespace delirium
{
	struct V3
	{
		float x{}, y{}, z{};
	};
	struct Q
	{
		float x{}, y{}, z{}, w{ 1.0f };
	};

	// One line of the rig file (fit-test's write_rig): how a physics part sits on its Skyrim bone.
	struct RigPart
	{
		std::string name, ref, jointA, jointB;
		bool        write{};
		Q           offset;    // ref bone rotation = part rotation * offset
		V3          anchor;    // the part's joint in its own frame (root: its hips' midpoint = the pelvis bone)
		V3          inParent;  // the same joint in the parent's frame
	};

	class Engine
	{
	public:
		static Engine& Get();

		void LoadSettings();
		bool Init();  // kDataLoaded: Delirium's DLL, the character file, our body and rig
		void OnKey(std::uint32_t a_key, RE::Actor* a_target = nullptr);  // a_target: else the crosshair's / console's NPC
		void Command(std::string_view a_line);  // main thread: a line of the command file (see Command in engine.cpp)
		void Shutdown();                        // main thread, before the core is unloaded: let go of every NPC
		void ReleaseAll(const char* a_why);     // let go of every NPC (loading screen)
		void AfterAnimation(RE::Actor* a_actor, float a_delta);  // after Skyrim's animation update: steps the physics
		void AfterPose(RE::Actor* a_actor, bool a_postPhysics);
		void AfterPoseImpl(RE::Actor* a_actor, bool a_postPhysics);  // right after Skyrim copied its pose into the bones: ours go on
		void DriveRagdoll(RE::Actor* a_actor, RE::hkbRagdollDriver* a_driver, float a_dt, bool a_after);  // around Skyrim's ragdoll drive
		static bool Busy();                                     // any body active (cheap, no lock)
		bool KeepsDown(RE::Actor* a_actor);                     // Delirium still moves this fallen body: no get-up yet
		void AfterRootMove(RE::Actor* a_actor);                 // Skyrim just put the 3D root on the actor: our bones stay put

	private:
		// one of Skyrim's ragdoll bodies and the bone it hangs on (the body sits on the bone, havok units = game units * kUnit).
		struct BodyLink
		{
			RE::hkpRigidBody* body{};
			RE::NiAVObject*   node{};
			V3                target;  // where we posed the bone (havok units), saved before Skyrim syncs bones to bodies
			Q                 targetRot;
			bool              hasTarget{};
			std::array<float, 4> massInv{};  // the body's own inertia/mass inverse
			V3                lin, ang;  // the velocities we set this step (re-applied after Skyrim's driver)
			// Skyrim's own ragdoll<->animation mapping for this body: body = animation bone * (mapR, mapT) (mapT unscaled)
			bool              mapped{};
			Q                 mapR;
			V3                mapT;
			int               posPart{ -1 }, rotPart{ -1 };  // the Delirium parts that give the bone's position / rotation
			// no Delirium part (NPC COM, the ragdoll's root): rides rigidly on the nearest Delirium bone (rig part index)
			int               follow{ -1 };
			Q                 followR;  // this body in that bone's frame, from calibration (havok units)
			V3                followT;
		};

		struct Active
		{
			RE::ActorHandle                    actor;
			NmAgent*                           agent{};
			NmWorld*                           world{};
			RE::NiPoint3                       origin;  // game units; physics works in metres from here
			RE::NiPoint3                       colCenter;  // game units: where the collision patch was gathered around
			std::array<RE::NiAVObject*, 21>    ref{};
			std::array<RE::NiAVObject*, 21>    ja{}, jb{};  // the bones whose midpoint is each part's joint
			RE::NiAVObject*                    com{};  // NPC COM: carries both the pelvis and the spine branches
			RE::NiMatrix3                      comInPelvis;  // COM's rotation in the pelvis's frame (from the standing pose)
			float                              age{}, calm{};
			float                              sincePush{};  // seconds since the last push (no calm-out before 1 s)
			int                                calls{}, poses{};  // animation updates / pose copies since the last log line
			float                              dtSum{};
			float                              scale{ 1.0f };  // the NPC's skeleton scale
			bool                               logged{};
			// Skyrim's side: upright we move the actor under the pelvis; fallen, Skyrim's own ragdoll state carries it
			bool                               knockQueued{}, calibrated{}, wasBalancing{};
			bool                               takeover{};  // started from Skyrim's own ragdoll (not F7)
			bool                               death{};     // a corpse: dies in Delirium, then fades over to Skyrim's ragdoll
			bool                               writhe{}, writhing{}, relaxed{};  // the dying plan: writhe first or not, gone limp
			float                              relaxAt{};   // age: when the next dying step (writhe end / limp) comes
			float                              calibAge{};    // age when Skyrim's ragdoll was calibrated
			bool                               following{};   // torque drive: Delirium in FOLLOW mode on Skyrim's ragdoll
			bool                               haveDrv{};
			std::array<float, 21 * 6>          drv{};         // Delirium's force and torque per part from its last step (world)
			std::array<int, 21>                partBody{};    // the body (index in bodies) each part's drive goes to
			std::array<float, 21 * 3>          acc{};         // Delirium's joint accelerations from its last step (world)
			float                              maxDw{}, maxWant{};
			int                                clamped{}, spinCapped{}, wantPart{};
			float                              wantAge{};     // age of the strongest joint ask since the last log line
			RE::hkbRagdollDriver*              driver{};
			std::vector<BodyLink>              bodies;
			float                              settled{}, noRagdoll{};  // seconds lying still in Skyrim's ragdoll state
			int                                drives{};
			float                              maxSpeed{};    // fastest body steering since the last log line (m/s)
			float                              blend{ 1.0f };  // 1 = Delirium's pose, 0 = Skyrim's animation
			bool                               blendingOut{};
			// Ragdolled, Delirium writes the skeleton until Skyrim's get-up (no blend over to Skyrim's own ragdoll pose:
			// that pose sits 0.1-0.2 m off ours and showed as a snap at landing)
			int                                copies{};  // Skyrim's pose copies since the last animation update
			// the skeleton's top node as last drawn, and how far SmoothRoot holds it behind Skyrim's (rad)
			Q                                  drawnRootQ;
			RE::NiPoint3                       drawnRootT;
			float                              drawnAge{}, rootLag{}, maxRootLag{};
			bool                               haveDrawnRoot{};
			// NPC COM in the world as WriteBones left it: when Skyrim moves the 3D root afterwards, COM's local is redone so
			// the bones under it stay where we put them
			RE::NiTransform                    comWorld;
			bool                               haveCom{};
			int                                rootFixLog{}, nanLog{};
			float                              maxRootFix{};
		};

		bool Activate(RE::Actor* a_actor, bool a_takeover = false, bool a_death = false);
		void TryTakeOver(RE::Actor* a_actor, bool a_dead);  // in Skyrim's ragdoll (or death ragdoll): Delirium takes the body over (or skips it)
		void ForgetSkip(RE::Actor* a_actor);   // out of Skyrim's ragdoll: may be taken over next time
		void InheritVelocity(Active& a);
		void Release(std::size_t a_i, const char* a_why);
		void Push(Active& a_act, RE::Actor* a_actor);
		float BodyVelocities(const Active& a, std::array<V3, 21>& v, std::array<V3, 21>& w, std::array<bool, 21>& have) const;
		void  SyncFromSkyrim(Active& a);
		bool  TorqueMode(const Active& a) const;
		void  MapParts(Active& a);
		void  ApplyTorques(Active& a, float a_dt);
		void  PartsFromBodies(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot) const;
		float PartsFromBones(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot, std::string* detail) const;
		void  SetZeroPose(Active& a, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot);
		void WriteBones(Active& a_act);
		bool SmoothRoot(Active& a, RE::Actor* a_actor);  // ragdolled: the top node turns no faster than cloth physics allows (true: moved)
		void AddWorld(Active& a, RE::Actor* a_actor, const RE::NiPoint3& a_center, bool a_quiet);  // Skyrim's collision around a_center
		void Calibrate(Active& a, RE::Actor* a_actor);
		void RefreshBones(Active& a);

		bool DeliriumBone(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const;  // havok world
		void BodyTarget(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const;
		void FollowActor(Active& a, RE::Actor* a_actor);

		std::recursive_mutex                  lock_;  // recursive: Skyrim may call a hooked function from inside one of ours
		std::vector<std::unique_ptr<Active>>  active_;
		std::vector<RE::ActorHandle>          skip_;  // in Skyrim's ragdoll but not ours (getting up, paralysed, ...)
		std::array<RigPart, 21>               rig_;
		float                                 rigYaw_{};
		NmBuild*                              build_{};
		bool                                  ready_{};

		// settings
		std::string   fredPath_, buildPath_, rigPath_;
		std::uint32_t pushKey_{ 0x41 }, releaseKey_{ 0x3C };  // F7, F2
		float         push_{ 80.0f };
		bool          autoRelease_{ true };
		bool          followAnimation_{ true };
		bool          collision_{ true };
		bool          takeOver_{ true };  // [Ragdoll] bTakeOver
		float         gain_{ 1.0f };      // [Ragdoll] fTorqueGain
		float         colRadius_{ 600.0f }, colHeight_{ 300.0f };  // game units around the NPC
		std::size_t   colMaxTris_{ 40000 };
	};
}
