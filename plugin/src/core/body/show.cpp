// What the renderer reads for her bones: Delirium's finished pose, never one being written.
//
// Skyrim's render threads build each skinned mesh's bone matrices (SkyrimSE AE +E4FF90, once a frame, from
// NiSkinInstance::boneWorldTransforms) while the animation update of the next frame runs. From RefreshBones to the end
// of WriteBones the bones hold Skyrim's animated pose, then ours bone by bone: a mesh skinned in that window drew a mix
// (in a stagger, far from ours: the face at the shoulder, a second pair of arms). Seen with a Cheat Engine read
// breakpoint on NPC Head: the renderer read it 844 times inside that window in 5 s.
//
// So while Delirium writes her bones, her meshes read their bones from Active::shown, a copy made once the pose is done.
//
// The copy itself changes only between render frames. Show used to update it in place, but Show runs while the
// renderer is partway through the frame (half the time some of her skins were built, never none): her head mesh drew
// last frame's pose and her brows, eyes, beard and hair this one's, 3-7 units apart, in about 1 frame in 40 (they
// sank in, floated, the eyes popped through; seen with a ce-mcp log of the face skins' matrix builds). Show now leaves
// the finished pose in Bone::next and PublishShown, at IDXGISwapChain::Present (hooks.cpp), copies it into what the
// skins read: one frame, one pose. Skyrim starts its next render frame right after Present, so the pose is drawn in
// the same frame as before when Show came after the renderer, which was the other half of the time.
//
// Physics mods that write her bones' world transforms (SMP: FSMP, HDT-SMP; cloth, hair, SMP breasts) do it after Show:
// FSMP at its frame sync, about 1.4 ms after Show and before her meshes are drawn; Skyrim's own update writes them
// back from local before Present. The copy never had them: her SMP parts hung fixed or snapped. So the host calls
// FrameSync right after that sync point (the call FSMP hooks, chained after it): the bones written over since Show
// (world no longer parent * local) go into her next pose, each where it is now from the nearest copied bone above it,
// and the pose is published there, with Present only a fallback for frames with no sync.
#include "shared.h"

namespace delirium
{
	namespace
	{
		using Bones = decltype(ShownPose::bones);

		// a render thread may have read a pointer into a copy just before we turned it back: keep copies 1 s before freeing
		std::mutex                                                           g_retiredLock;
		std::vector<std::pair<std::chrono::steady_clock::time_point, Bones>> g_retired;

		void Retire(Bones&& a_bones)
		{
			const auto      now = std::chrono::steady_clock::now();
			std::scoped_lock l(g_retiredLock);
			std::erase_if(g_retired, [&](const auto& r) { return now - r.first > std::chrono::seconds(1); });
			if (!a_bones.empty()) {
				g_retired.emplace_back(now, std::move(a_bones));
			}
		}

		// every pose her skins read, and their bones: Show and Unshow (animation threads, under lock_) and PublishShown
		// (Present, main thread). Never takes lock_ itself: AfterPose holds that through the physics
		std::mutex              g_showLock;
		std::vector<ShownPose*> g_shown;

		::CRITICAL_SECTION* SkinLock(RE::NiSkinInstance* a_skin) { return reinterpret_cast<::CRITICAL_SECTION*>(&a_skin->lock); }

		// her skins all at once, never waiting: false (none held) while a render thread builds one's matrices (it holds
		// the skin's lock for that)
		bool LockSkins(const ShownPose& a_p)
		{
			std::size_t n = 0;
			while (n < a_p.skins.size() && TryEnterCriticalSection(SkinLock(a_p.skins[n].get()))) {
				++n;
			}
			if (n == a_p.skins.size()) {
				return true;
			}
			while (n > 0) {
				LeaveCriticalSection(SkinLock(a_p.skins[--n].get()));
			}
			return false;
		}
		void UnlockSkins(const ShownPose& a_p)
		{
			for (auto& s : a_p.skins) {
				LeaveCriticalSection(SkinLock(s.get()));
			}
		}

		void Publish(ShownPose& a_p)
		{
			for (auto& [node, b] : a_p.bones) {
				b.drawn = b.next;
			}
			++a_p.published;
		}

		// written as a world transform by someone since its parent's update (SMP writes world only): no longer parent * local
		bool WrittenOver(const RE::NiAVObject* a_node, const RE::NiAVObject* a_parent)
		{
			const auto e = a_parent->world * a_node->local;
			const auto& w = a_node->world;
			if ((e.translate - w.translate).Length() > 0.01f) {
				return true;
			}
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					if (std::abs(e.rotate.entry[i][j] - w.rotate.entry[i][j]) > 1e-3f) {
						return true;
					}
				}
			}
			return false;
		}

		// what a physics mod wrote over her bones since Show, into her next pose: each such bone where it is now from the
		// bone above it, put on that bone as we pose it. Her pose moved since Show only as a whole (her root), so the rest
		// stays as Show copied it.
		void PhysicsIn(ShownPose& a_p)
		{
			for (auto& f : a_p.follow) {
				if (f.node->parent == f.parent.get() && WrittenOver(f.node.get(), f.parent.get())) {
					f.bone->next = f.anchorBone->next * (f.anchor->world.Invert() * f.node->world);
				}
			}
		}

		void PublishAll()
		{
			for (auto* p : g_shown) {
				// a render thread still on one of her skins: her pose waits for the next frame (never twice in a row: a
				// torn frame rather than a stuck pose)
				const bool locked = LockSkins(*p);
				if (!locked && !p->heldLast) {
					p->heldLast = true;
					++p->held;
					continue;
				}
				Publish(*p);
				p->heldLast = false;
				if (locked) {
					UnlockSkins(*p);
				}
			}
		}

		std::atomic<bool> g_synced{ false };  // FrameSync ran since the last Present: it published this frame's poses
	}

	void PublishShown()
	{
		if (g_synced.exchange(false)) {
			return;
		}
		std::scoped_lock l(g_showLock);
		PublishAll();
	}

	void FrameSync()
	{
		std::scoped_lock l(g_showLock);
		for (auto* p : g_shown) {
			PhysicsIn(*p);
		}
		PublishAll();
		g_synced = true;
	}

	void Engine::Show(Active& a, RE::NiAVObject* a_root)
	{
		auto&            p = a.shown;
		std::scoped_lock l(g_showLock);
		// the finished pose, drawn from the next frame (nodes still in her 3D this frame only: gear taken off takes its bones along)
		for (auto& [node, b] : p.bones) {
			if (a.drawnWorld.contains(node)) {
				b.next = node->world;
			}
		}
		// the bones a physics mod may write over before she is drawn, each with the nearest copied bone above it, parents first
		p.follow.clear();
		{
			std::vector<std::pair<int, ShownPose::Follow>> f;
			for (auto& [node, b] : p.bones) {
				if (!node->parent || !a.drawnWorld.contains(node)) {
					continue;
				}
				int             depth = 0;
				RE::NiAVObject* anchor{};
				ShownPose::Bone* anchorBone{};
				for (auto* up = node->parent; up; up = up->parent, ++depth) {
					if (!anchor) {
						if (const auto it = p.bones.find(up); it != p.bones.end() && a.drawnWorld.contains(up)) {
							anchor = up;
							anchorBone = &it->second;
						}
					}
				}
				if (anchor) {
					f.push_back({ depth, { RE::NiPointer(node), RE::NiPointer(node->parent), RE::NiPointer(anchor), &b, anchorBone } });
				}
			}
			std::ranges::sort(f, {}, &std::pair<int, ShownPose::Follow>::first);
			for (auto& [d, e] : f) {
				p.follow.push_back(std::move(e));
			}
		}
		// meshes that still read the live bones (new ones: gear put on, blood decals) move over to the copy
		std::vector<RE::NiAVObject*> stack{ a_root };
		while (!stack.empty()) {
			auto* n = stack.back();
			stack.pop_back();
			if (auto* g = n->AsGeometry()) {
				auto* skin = g->GetGeometryRuntimeData().skinInstance.get();
				if (skin && skin->skinData && skin->bones && skin->boneWorldTransforms) {
					bool moved = false;
					for (std::uint32_t i = 0; i < skin->skinData->bones; ++i) {
						auto* b = skin->bones[i];
						if (b && skin->boneWorldTransforms[i] == &b->world && a.drawnWorld.contains(b)) {
							skin->boneWorldTransforms[i] = &p.bones.try_emplace(b, ShownPose::Bone{ b->world, b->world }).first->second.drawn;
							moved = true;
						}
					}
					if (moved && std::ranges::find(p.skins, skin, [](const auto& s) { return s.get(); }) == p.skins.end()) {
						p.skins.emplace_back(skin);
					}
				}
			}
			if (auto* node = n->AsNode()) {
				for (auto& c : node->GetChildren()) {
					if (c) {
						stack.push_back(c.get());
					}
				}
			}
		}
		if (p.skins.empty()) {
			return;
		}
		if (std::ranges::find(g_shown, &p) == g_shown.end()) {
			g_shown.push_back(&p);
		}
		if (!g_atPresent) {
			Publish(p);  // Present not hooked: drawn as soon as it is done
		}
	}

	void Engine::Unshow(Active& a)
	{
		auto&            p = a.shown;
		std::scoped_lock l(g_showLock);
		std::erase(g_shown, &p);
		if (p.skins.empty()) {
			return;
		}
		// the skins are kept alive by p.skins; their bones may be gone with her 3D (only their addresses are used)
		for (auto& skin : p.skins) {
			if (!skin->skinData || !skin->bones || !skin->boneWorldTransforms) {
				continue;
			}
			for (std::uint32_t i = 0; i < skin->skinData->bones; ++i) {
				auto* b = skin->bones[i];
				if (const auto it = b ? p.bones.find(b) : p.bones.end(); it != p.bones.end() && skin->boneWorldTransforms[i] == &it->second.drawn) {
					skin->boneWorldTransforms[i] = &b->world;
				}
			}
		}
		// trace: poses drawn, and held back a frame (a skin mid-build at Present)
		logger::info("shown: {} poses drawn, {} held a frame", p.published, p.held);
		p.skins.clear();
		p.follow.clear();
		Retire(std::move(p.bones));
		p.bones.clear();
		p.published = p.held = 0;
		p.heldLast = false;
	}
}
