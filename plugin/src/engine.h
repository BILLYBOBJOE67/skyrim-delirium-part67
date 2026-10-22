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
		void NoKillMoves();  // [Deaths]: Skyrim's killmoves and kill cams off (Delirium plays the death)
		void AfterAnimation(RE::Actor* a_actor, float a_delta);  // after Skyrim's animation update: steps the physics
		void AfterPose(RE::Actor* a_actor, bool a_postPhysics);
		void AfterPoseImpl(RE::Actor* a_actor, bool a_postPhysics);  // right after Skyrim copied its pose into the bones: ours go on
		void DriveRagdoll(RE::Actor* a_actor, RE::hkbRagdollDriver* a_driver, float a_dt, bool a_after);  // around Skyrim's ragdoll drive
		static bool Busy();                                     // any body active (cheap, no lock)
		bool KeepsDown(RE::Actor* a_actor);                     // Delirium still moves this fallen body: no get-up yet
		void AfterRootMove(RE::Actor* a_actor);                 // Skyrim just put the 3D root on the actor: our bones stay put
		// what hit her: an arrow or bolt, or a destruction spell's projectile by its element
		enum HitKind : int { kArrow, kFrost, kFire, kShock, kMagic };
		// main thread: a projectile hit a living NPC (game units, units/s; a_exact: a_at is where it went in)
		void OnHit(RE::Actor* a_target, const RE::NiPoint3& a_at, const RE::NiPoint3& a_vel, bool a_exact, int a_kind, bool a_channel = false);
		// any weapon hit on an actor (Actor::ProcessHitData, before Skyrim's): kept as her last hit
		void OnHitData(RE::Actor* a_target, const RE::HitData& a_hit);
		// Skyrim staggered her (a_started: it really did now): Delirium's push takes the stagger's place on the body
		void OnStagger(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source, bool a_started);
		void Stagger(RE::Actor* a_target, float a_magnitude, RE::Actor* a_source);  // main thread, a frame later
		// her killing blow (Character::KillImpl, before Skyrim's): true holds the death back, at 1 health, for up to
		// [Deaths] fDyingTime s: she dies on her feet in Delirium (wounded shot, stagger, collapse), then really dies
		bool OnKill(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent, bool a_ragdollInstant);

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
			bool              havePivot{};     // its joint to its parent body: the pivot in the body's own frame (havok units)
			V3                pivot;
			V3                lin, ang;  // the velocities we set this step (re-applied after Skyrim's driver)
			V3                aimCom;    // trace: where the last keyframe sent the body's centre of mass
			bool              haveAim{};
			// Skyrim's own ragdoll<->animation mapping for this body: body = animation bone * (mapR, mapT) (mapT unscaled)
			bool              mapped{};
			Q                 mapR;
			V3                mapT;
			// the way back, as Skyrim's pose copy draws it (its ragdoll-to-animation mapper): bone = body * (drawR, drawT)
			bool              haveDraw{};
			Q                 drawR;
			V3                drawT;
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
			std::vector<RE::hkpConstraintData*> eased;   // joints whose friction we took off (held, put back on release)
			std::vector<RE::hkpRigidBody*>      weighed; // bodies given Delirium's mass (held, Skyrim's put back on release)
			std::vector<std::array<float, 2>>   material; // Skyrim's own friction and bounce of each weighed body (put back on release)
			NmWorld*                           world{};
			RE::NiPoint3                       origin;  // game units; physics works in metres from here
			RE::NiPoint3                       colCenter;  // game units: where the collision patch was gathered around
			std::array<RE::NiAVObject*, 21>    ref{};
			std::array<RE::NiAVObject*, 21>    ja{}, jb{};  // the bones whose midpoint is each part's joint
			RE::NiAVObject*                    com{};  // NPC COM: carries both the pelvis and the spine branches
			// snap trace around the fall (knock until 0.6 s after calibration): the drawn pose at each pose copy / root move
			bool                               haveTrace{};
			std::array<RE::NiTransform, 21>    trBone{};
			std::array<RE::NiMatrix3, 21>      trRel{};   // each bone's rotation relative to the pelvis
			std::array<RE::NiPoint3, 21>       trSpin{};  // and its last change (axis * sin angle)
			RE::NiTransform                    trTop;
			RE::NiPoint3                       trPelvisMove;
			std::array<RE::NiTransform, 21>    written{};  // bone worlds as WriteBones left them (trace: drawn vs ours)
			bool                               haveWritten{}, dumped{};
			// F7's lead-in: what is drawn goes from our pose to Skyrim's (its ragdoll can't reach every pose of ours)
			float                              limbBlend{ 1.0f };  // share of ours on the limbs in WriteBones
			RE::NiTransform                    skyComLocal;        // NPC COM from Skyrim's last pose copy
			bool                               haveSkyCom{};
			int                                traceN{}, physSteps{}, trSteps{};
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
			bool                               heldDeath{};  // dying on her feet: her death is held back (OnKill)
			float                              collapseAt{ -1.0f };  // age: dying on her feet, she stops fighting it and goes down
			float                              relaxAt{};   // age: when the next dying step (writhe end / limp) comes
			float                              dyingAt{ -1.0f };  // age: when she died (the shot plays before any writhe)
			int                                lastType{ -1 }, lastElement{ -1 };  // her last blow's type and element ([Reactions] die rows)
			float                              dieAmp{ -1.0f }, diePeriod{ -1.0f };  // her death writhe's size and s per cycle (-1: random)
			float                              speedCap{ -1.0f }; // m/s: the next drive caps every Skyrim body at this (her death's spikes), once
			float                              calibAge{};    // age when Skyrim's ragdoll was calibrated
			bool                               following{};   // torque drive: Delirium in FOLLOW mode on Skyrim's ragdoll
			bool                               haveDrv{};
			std::array<float, 21 * 6>          drv{};         // Delirium's force and torque per part from its last step (world)
			std::array<int, 21>                partBody{};    // the body (index in bodies) each part's drive goes to
			std::array<float, 21 * 3>          acc{};         // Delirium's joint accelerations from its last step (world)
			std::array<float, 21 * 3>          ext{};         // its torques on the parts themselves (a shot's spin, electrocute's jolts; world, N m)
			float                              maxDw{}, maxWant{};
			float                              actSum{}, extMax{};  // trace: Skyrim's bodies' spin vs the pelvis (deg/s, summed per step); behaviours' biggest torque turn
			int                                actN{};
			std::vector<Q>                     actRef;   // trace: each body's turn against the pelvis body when the second began
			std::vector<float>                 actSwing; // and the most it has swung from it since (deg)
			int                                sleepMax{}; // trace: most frames a body has been counted still by Havok (deactivation) this second
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
			std::array<Q, 21>                  restRot{};  // the body's own standing stance, each part in its parent's frame (legs use it)
			std::array<V3, 21>                 restPos{};
			std::string                        trackFile;  // [Test] bRecordPoses: where her held poses go, empty when not recorded
			int                                trackN{};
			float                              maxRootFix{};
			// trace: the pelvis when the last arrow hit, and the way it flew (flat)
			bool                               haveShot{};
			float                              burnUntil{ -1.0f }, shockUntil{ -1.0f };  // age the fire / shock stops (-1: none)
			float                              chillUntil{ -1.0f };                        // a frost channel: freezing until then
			float                              reelUntil{ -1.0f };  // a hard hit: staggering back on loose legs until then, then she catches herself
			int                                flying{};            // thrown: 1 in the air, 2 coming down (Fly)
			float                              offBalance{};        // s her balance has been off (a behaviour restarting it is off for a moment: not a fall)
			int                                flyMode{};           // 0 windmill + pedal, 1 rolled up, 2 highFall, 3 curled up kicking
			bool                               braced{};            // thrown at something: arms out at it
			float                              braceCheckAt{};      // age of the next look ahead
			float                              writheFor{};         // s she writhes once she is down (0: she doesn't)
			float                              writheAt{ -1.0f };   // age the writhe starts (she has caught the fall first)
			float                              writheUntil{ -1.0f };
			float                              writheOomph{};       // how hard it was: a bigger writhe
			int                                writheElement{ -1 }; // what took her down: burning rolls, shock convulses, frost curls up shivering
			struct Kick
			{
				int  part{ -1 };  // Delirium part it landed on: its Skyrim body at the drive (-1: all of them)
				V3   j{};         // N s on that body; all of them: m/s
				V3   arm{};       // from the body's centre to where it landed (m): the spin it gives
			};
			std::vector<Kick>                  kicks;  // blows on her while she is down, put on Skyrim's bodies at the next drive
			float                              flyAt{};             // age she was thrown
			int                                filled{};  // trace: frames this second Skyrim skipped its pose copy (ours put on here)
			bool                               filling{};  // in that filler AfterPose: the bones hold our pose, not the animation
			V3                                 shotPelvis, shotDir;
			// the last push (age, N s): a hit and its stagger come together and push once, the stronger one
			float                              lastPushAt{ -10.0f }, lastPush{};
		};

		bool Activate(RE::Actor* a_actor, bool a_takeover = false, bool a_death = false);
		void TryTakeOver(RE::Actor* a_actor, bool a_dead);  // in Skyrim's ragdoll (or death ragdoll): Delirium takes the body over (or skips it)
		void ForgetSkip(RE::Actor* a_actor);   // out of Skyrim's ragdoll: may be taken over next time
		void InheritVelocity(Active& a);
		void Release(std::size_t a_i, const char* a_why);
		void Push(Active& a_act, RE::Actor* a_actor);
		// a push at a_at (metres, the agent's frame) along a_dir; with another push in the last 0.3 s only what this
		// one adds over it. Returns the N s applied
		float AddPush(Active& a, const float* a_at, const V3& a_dir, float a_push);
		float DamageScale(RE::Actor* a_target, float a_damage) const;  // the push's scale for a hit of this much damage
		// a melee blow's push (N s, 0 if it isn't one) and its way (unit): by the attack, the weapon's weight and the damage
		float StrikePush(RE::Actor* a_target, const RE::HitData& a_hit, RE::NiPoint3& a_dir) const;
		// an explosion caught her (direct hit or near it): pushed away from its centre, by its damage and how near she was
		void  Blast(RE::Actor* a_target, const RE::HitData& a_hit, RE::Explosion* a_ex, RE::BGSExplosion* a_base, float a_spellDamage, int a_spellElement);
		void  LookAt(Active& a, RE::Actor* a_by, float* a_out) const;  // her attacker's head, Delirium's frame
		// fire / shock / frost on again after a task stopped every behaviour (their end time stays: nothing is extended)
		void  Overlays(Active& a, const V3& a_away);
		void  Reacting(Active& a);  // a new reaction: no calm-out, the body fully ours again
		// an arrow's hit (game units; a_exact: a_at is where it went in, else the chest): Delirium's shot reaction
		// One blow, whatever dealt it (an arrow, a spell, a sword, a stagger): every push goes through React, which
		// picks the reaction by what the blow is and how hard it is. Its sources only say what hit her and how hard.
		struct Blow
		{
			const char*  what{ "hit" };  // for the log
			std::string  note;           // for the log: what its strength came from
			RE::ActorPtr by;             // who did it (none: a trap, a stagger with no caster seen)
			RE::NiPoint3 at;             // where it landed, game units
			bool         exact{};        // at is on her (else the chest)
			V3           dir{};          // the way it pushes (unit)
			float        push{};         // N s
			int          element{ -1 };  // kFire / kShock: that reaction on top, for its own time
			bool         shot{};         // a projectile went in: Delirium's shot task. Else by strength: a flinch or a step
			bool         canStart{ true };  // may bring her into Delirium (a melee blow only joins a reaction)
			float        oomph{};        // 0-1, how far past a plain hit it is: a harder jolt (the shot's spin, lean, push)
			bool         join{};         // a reaction started this moment (a direct hit, then its blast): only the push adds
			V3           fly{};          // m/s: thrown (an explosion), alive in the air; zero: on her feet
			bool         melee{};        // a weapon blow (or the stagger it makes): its oomph is from how much harder than a plain blow
			int          type{ 0 };      // BlowType: its row in the reaction table
			bool         inherited{};    // fly: she already moves (Skyrim's own knock, taken over): no velocity or tumble of ours
		};
		// the reaction table ([Reactions]): a blow's phase, type and strength pick one rule, a base behaviour and its layers
		enum BlowType : int { kBlowOther, kBlowMelee, kBlowArrow, kBlowSpell, kBlowBlast, kBlowStagger, kBlowKnock, kBlowShout };
		static const char* TypeName(int a_type);  // other, melee, arrow, spell, blast, stagger, knock, shout
		void               Knocked(Active& a, const V3& a_vel);  // Skyrim knocked her down itself (a shout, a trap, a script): her reaction
		struct Rule
		{
			std::string                                base;    // shot, flinch, step, none
			std::vector<std::pair<std::string, float>> layers;  // name, value (NaN: none given)
			std::string                                text;    // as written
			[[nodiscard]] bool  Has(std::string_view a_name) const;
			[[nodiscard]] float Value(std::string_view a_name, float a_or) const;  // the layer's (or base's) value, else a_or
		};
		static Rule ParseRule(std::string_view a_text);
		const Rule& PickRule(const char* a_phase, const Blow& a_blow, bool a_lethal, std::string& a_key) const;
		Rule        ElementRule(int a_element) const;  // the element's layers ([Reactions] fire / shock / frost)
		const Rule* FindRule(std::initializer_list<std::string> a_keys, std::string& a_key) const;  // the first of these rows there is
		static const char* ElementName(int a_element);  // arrow, frost, fire, shock, magic; "" for none
		// one of the row's choices (its names among a_names), by their weights (name:weight, 1 if none given); -1 if none
		static int Choose(const Rule& a_rule, std::initializer_list<std::string_view> a_names);
		void React(RE::Actor* a_target, const Blow& a_blow);
		void ReactOn(Active& a, RE::Actor* a_actor, const Blow& a_blow);  // under lock_
		void DownHit(Active& a, RE::Actor* a_actor, const Blow& a_blow);  // a blow on her while she is down (or a corpse); under lock_
		void StartDying(Active& a, const V3& a_moving, bool a_held = false);  // the killing blow as a shot, then writhe/limp; under lock_
		struct Held
		{
			RE::ActorHandle                       actor, attacker;
			float                                 damage{};
			bool                                  send{}, instant{};
			std::chrono::steady_clock::time_point at{};
			const char*                           why{ "" };
		};
		void DyingOnFeet(RE::Actor* a_actor);              // main thread, after her held killing blow: Delirium has her, wounded
		void CheckHeld(RE::Actor* a_actor);                // every animation update: her time up (or down): she dies
		void KillHeld(const char* a_why);                  // main thread: every held death now
		void KillNow(const Held& a_held, const char* a_why);  // main thread: Skyrim's kill, with the killer and blow it had
		void Fly(Active& a, RE::Actor* a_actor, const Blow& b);  // thrown: arms windmill / legs pedal or tucked in a roll, then a catch
		bool Eligible(RE::Actor* a_target, const char* a_what, bool a_quiet) const;  // a person Delirium can take
		void StartBurn(Active& a, const V3& a_away);  // fire: the arms beat at the flames, a stumble away
		void StopBurn(Active& a);
		void StartShock(Active& a);  // lightning: jolts through the body
		void StopShock(Active& a);
		void StartChill(Active& a);  // frost: hunched, shivering, short stiff steps
		void StopChill(Active& a);
		// a channelled spell (Flames, Frostbite, Sparks): no shot, only the element's reaction while it keeps hitting
		void Channel(Active& a, RE::Actor* a_actor, const RE::NiPoint3& a_vel, int a_kind);
		int  NearestPart(const Active& a, const float* a_at) const;  // the part closest to a point (Delirium's frame)
		float BodyVelocities(const Active& a, std::array<V3, 21>& v, std::array<V3, 21>& w, std::array<bool, 21>& have) const;
		void  SyncFromSkyrim(Active& a);
		bool  TorqueMode(const Active& a) const;
		float LeadShare(const Active& a) const;  // F7's lead-in: 1 at its start, 0 at its end (and after)
		void  MapParts(Active& a);
		void  ApplyTorques(Active& a, float a_dt);
		void  PartsFromBodies(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot) const;
		float PartsFromBones(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot, std::string* detail) const;
		void  SetZeroPose(Active& a, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot);
		void WriteBones(Active& a_act);
		bool SmoothRoot(Active& a, RE::Actor* a_actor);  // ragdolled: the top node turns no faster than cloth physics allows (true: moved)
		void AddWorld(Active& a, RE::Actor* a_actor, const RE::NiPoint3& a_center, bool a_quiet);  // Skyrim's collision around a_center
		void Calibrate(Active& a, RE::Actor* a_actor);
		void DumpRagdoll(const Active& a, RE::Actor* a_actor);  // Skyrim's ragdoll joints to a JSON for the fit-test body
		void RecordPose(Active& a, RE::Actor* a_actor, const std::array<V3, 21>& pos, const std::array<Q, 21>& rot);
		void StandingLegs(const Active& a, std::array<V3, 21>& pos, std::array<Q, 21>& rot) const;
		void Trace(Active& a, RE::Actor* a_actor, const char* a_where);
		void RefreshBones(Active& a);

		bool DeliriumBone(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const;  // havok world
		void BodyTarget(const Active& a, const BodyLink& l, V3& a_pos, Q& a_rot) const;
		void FollowActor(Active& a, RE::Actor* a_actor);

		std::recursive_mutex                  lock_;  // recursive: Skyrim may call a hooked function from inside one of ours
		std::vector<std::unique_ptr<Active>>  active_;
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> releasedAt_;  // let go in Skyrim's ragdoll: when (the get-up log)
		std::vector<RE::ActorHandle>          skip_;  // in Skyrim's ragdoll but not ours (getting up, paralysed, ...)
		std::array<RigPart, 21>               rig_;
		float                                 rigYaw_{};
		NmBuild*                              build_{};
		bool                                  ready_{};

		// settings
		std::string   fredPath_, buildPath_, rigPath_;
		bool          usingGta_{ false };
		std::uint32_t pushKey_{ 0x41 }, releaseKey_{ 0x3C };  // F7, F2
		float         push_{ 80.0f };
		bool          autoRelease_{ true };
		bool          followAnimation_{ true };
		bool          standingLegs_{ true };  // [Test] bStandingLegs: the legs hold the body's own stance, not the animation's
		bool          recordPoses_{ false };  // [Test] bRecordPoses: the held pose each frame while standing, for fit-test sweep STANCE_TRACK=
		int           tracks_{ 0 };
		bool          collision_{ true };
		bool          takeOver_{ true };  // [Ragdoll] bTakeOver
		float         gain_{ 1.0f };      // [Ragdoll] fTorqueGain
		float         damp_{ 0.0f };      // [Ragdoll] fJointDamping: share of a joint's spin its muscles take out per step
		float         friction_{ 0.0f };  // [Ragdoll] fJointFriction: Skyrim's joint friction while Delirium drives (1 = untouched)
		bool          matchMass_{ true }; // [Ragdoll] bMatchMass: Skyrim's bodies weigh what Delirium's parts do while it drives
		bool          matchContact_{ true }; // [Ragdoll] bMatchContact: and grip and bounce as Delirium's parts do
		bool          noKillMoves_{ true };  // [Deaths] bNoKillMoves
		bool          noKillCams_{ true };   // [Deaths] bNoKillCams
		float         dyingHealth_{ 70.0f };  // [Deaths] fDyingHealth: the killing blow as a shot at this health (0: fatal, every muscle off in 0.5 s)
		float         limpMin_{ 1.8f }, limpMax_{ 3.0f };  // [Deaths] fLimpAfterMin/Max: s until the corpse goes limp
		float         dyingWrithe_{ 0.5f };   // [Deaths] fWritheChance: a weak writhe on the ground first
		float         dyingCatch_{ 0.5f };    // [Deaths] fCatchFallChance: the arms go out to break the fall
		float         deathPush_{ 1.5f };     // [Deaths] fDeathPush: m/s away from the killer, in place of Skyrim's death motion
		float         deathMaxSpeed_{ 6.0f }; // [Deaths] fDeathMaxSpeed: m/s cap on any body as she dies
		float         dyingReact_{ 1.2f };    // [Deaths] fShotTime: s the dying shot plays before a writhe
		float         collapseMin_{ 0.8f }, collapseMax_{ 1.6f };  // [Deaths] fCollapseAfterMin/Max: s she fights to stay up, dying
		float         dyingTime_{ 3.0f };
		std::unordered_map<std::string, Rule> rules_;  // [Reactions], over the built-in table
		float         heavyOomph_{ 0.5f };  // [Reactions] fHeavyOomph: from this oomph a blow is heavy
		float         shoutThrowFrom_{ 0.6f };  // [Shouts] fThrowFrom: a shout's stagger from this magnitude throws her
		float         shoutFly_{ 9.0f };        // [Shouts] fFlySpeed: m/s she is thrown at, at magnitude 1     // [Deaths] fDyingTime: s she may die on her feet before the death goes through (0: off)
		std::unordered_map<RE::FormID, Held> held_;      // deaths held back (under lock_)
		std::unordered_set<std::string>      ragdollDumps_;  // limit sets already written by DumpRagdoll
		std::unordered_set<RE::FormID>       heldDied_;  // killed after a hold: her death ragdoll gets no new shot or push
		std::atomic<RE::Actor*>              letKill_{ nullptr };  // the kill we make ourselves goes through
		bool          aboutJoint_{ true };   // [Ragdoll] bTurnAboutJoint: a muscle turns the limb about its joint (the spin and the swing that goes with it)
		// Skyrim's joints' own friction, by joint data (it may be shared between ragdolls): the original and how many hold it
		std::unordered_map<RE::hkpConstraintData*, std::pair<float, int>> eased_;
		void EaseJoints(Active& a);
		void RestoreJoints(Active& a);
		bool          arrows_{ true };      // [Arrows] bShotReaction
		float         arrowPush_{ 55.0f };  // [Arrows] fPush: N s at the hit point
		bool          spells_{ true };      // [Spells] bShotReaction
		float         burnTime_{ 2.5f };    // [Spells] fBurnTime, s
		float         shockTime_{ 2.0f };   // [Spells] fShockTime, s
		bool          staggers_{ true };      // [Stagger] bReplace: Skyrim's staggers are played by Delirium
		float         staggerPush_{ 100.0f };  // [Stagger] fPush: N s for a full stagger (magnitude 1)
		float         staggerMin_{ 0.4f };     // [Stagger] fMinShare: the push's share at magnitude 0
		float         damageRef_{ 15.0f };     // [Hits] fDamageRef: a hit of this much damage pushes at 1x
		float         damageExp_{ 0.8f };      // [Hits] fExponent: twice the damage pushes 2^this as hard
		float         damageMin_{ 0.4f }, damageMax_{ 3.0f };  // [Hits] fScaleMin / fScaleMax
		float         oomphFrom_{ 1.0f }, oomphTo_{ 2.5f };  // [Hits] fOomphFrom / fOomphTo: damage scale for oomph 0 / 1
		float         oomphSpin_{ 2.0f };      // [Hits] fOomphSpin: the shot's twist x(1 + this x oomph)
		float         oomphLean_{ 0.9f };      // [Hits] fOomphLean: the shot's lean back 0.3 + this x oomph
		float         oomphPush_{ 1.0f };      // [Hits] fOomphPush: the push x(1 + this x oomph)
		float         reelAt_{ 0.3f };         // [Hits] fReelAt: from this oomph she reels back (Delirium's staggerFall)
		float         fallAt_{ 1.01f };        // [Hits] fFallAt: from this oomph the reel takes her down (over 1: never forced, she falls only if she can't catch it)
		float         reelTime_{ 0.8f }, reelTimeAdd_{ 1.6f };  // [Hits] fReelTime + fReelTimeAdd x oomph: s she reels
		float         staggerHitBoost_{ 0.5f };  // [Stagger] fHitBoost: a blow that staggers pushes x(1 + this x magnitude)
		bool          melee_{ true };           // [Melee] bReact: blows push her while Delirium has her
		bool          blasts_{ true };          // [Explosions] bPush: an explosion pushes everyone it catches
		float         blastPush_{ 90.0f };      // [Explosions] fPush: N s at its centre for fDamageRef damage
		float         blastFar_{ 0.4f };        // [Explosions] fFarShare: the push's share at its edge
		float         blastUp_{ 0.25f };        // [Explosions] fUp: the push's upward part (0 flat, 1 straight up)
		float         flySpeed_{ 3.0f };        // [Explosions] fFlySpeed: m/s she is thrown at, at the centre for fDamageRef damage
		float         highFallAt_{ 4.5f };      // [Explosions] fHighFallAt: from this m/s she is thrown far: Delirium's highFall
		float         tumble_{ 10.0f };         // [Explosions] fTumble: N s at the head per m/s thrown: she tumbles
		float         writheTime_{ 1.0f }, writheAdd_{ 1.2f };  // [Down] fWritheTime + fWritheAdd x oomph: s she writhes on the ground
		float         foetalChance_{ 0.3f };    // [Down] fFoetalChance: she curls up instead
		float         meleeOomphFrom_{ 1.3f }, meleeOomphTo_{ 4.0f };  // [Melee] fOomphFrom / fOomphTo: x a plain blow's push for oomph 0 / 1
		float         meleeKnockAt_{ 1.01f };   // [Melee] fKnockAt: from this oomph the blow takes her down
		float         headWhip_{ 0.2f };        // [Melee] fHeadWhip: share of the push at the head too
		float         hitMaxPush_{ 150.0f };    // [Hits] fMaxPush: a projectile's push at most (past it, the behaviours carry it)
		float         meleeMaxPush_{ 160.0f };  // [Melee] fMaxPush: a blow's push at most (past it, the behaviours carry it: one shove is a limp ragdoll)
		float         flyUp_{ 0.65f };          // [Explosions] fFlyUp: m/s up per m/s along
		float         flyMin_{ 2.0f }, flyMax_{ 9.0f };  // [Explosions] fFlyMin / fFlyMax: slower than this she only reels; the cap
		float         meleePush_{ 45.0f };      // [Melee] fPush: N s for a plain blow (iron sword, 15% of her health)
		float         meleePower_{ 2.2f }, meleeBash_{ 1.6f }, meleeSneak_{ 1.3f }, meleeBlocked_{ 0.5f };  // [Melee] fPower / fBash / fSneak / fBlocked
		float         meleeWeightRef_{ 9.0f };  // [Melee] fWeightRef: a weapon this heavy pushes at 1x (by the square root)
		float         meleeHeftMin_{ 0.6f }, meleeHeftMax_{ 1.7f };  // [Melee] fHeftMin / fHeftMax
		float         meleeSwing_{ 0.5f };      // [Melee] fSwing: how much a swing's sideways sweep bends the push
		float         meleeChop_{ 0.35f };      // [Melee] fChop: an overhead chop pushes this much down too
		float         flinchBelow_{ 55.0f };    // [Melee] fFlinchBelow: a lighter blow flinches, a heavier one steps
		float         channelLinger_{ 0.6f };  // [Spells] fChannelLinger: s a channel's reaction lasts after its last hit
		float         wounded_{ 0.2f };     // [Arrows] fWoundedBelow: under this share of health a hit takes her down
		float         colRadius_{ 600.0f }, colHeight_{ 300.0f };  // game units around the NPC
		std::size_t   colMaxTris_{ 40000 };
	};
}
