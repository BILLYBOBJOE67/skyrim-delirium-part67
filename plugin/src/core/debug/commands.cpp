// The command file (tools\hotreload.ps1 -Cmd) and the test keys (F7 push, F2 release).
#include "shared.h"

namespace delirium
{
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

}
