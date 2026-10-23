// What the renderer reads for her bones: Delirium's finished pose, never one being written.
//
// Skyrim's render threads build each skinned mesh's bone matrices (SkyrimSE AE +E4FF90, once a frame, from
// NiSkinInstance::boneWorldTransforms) while the animation update of the next frame runs. From RefreshBones to the end
// of WriteBones the bones hold Skyrim's animated pose, then ours bone by bone: a mesh skinned in that window drew a mix
// (in a stagger, far from ours: the face at the shoulder, a second pair of arms). Seen with a Cheat Engine read
// breakpoint on NPC Head: the renderer read it 844 times inside that window in 5 s.
//
// So while Delirium writes her bones, her meshes read their bones from Active::shown, a copy made once the pose is done.
#include "shared.h"

namespace delirium
{
	namespace
	{
		using Shown = std::unordered_map<RE::NiAVObject*, RE::NiTransform>;

		// a render thread may have read a pointer into a copy just before we turned it back: keep copies 1 s before freeing
		std::mutex                                                     g_retiredLock;
		std::vector<std::pair<std::chrono::steady_clock::time_point, Shown>> g_retired;

		void Retire(Shown&& a_shown)
		{
			const auto      now = std::chrono::steady_clock::now();
			std::scoped_lock l(g_retiredLock);
			std::erase_if(g_retired, [&](const auto& r) { return now - r.first > std::chrono::seconds(1); });
			if (!a_shown.empty()) {
				g_retired.emplace_back(now, std::move(a_shown));
			}
		}
	}

	void Engine::Show(Active& a, RE::NiAVObject* a_root)
	{
		// the finished pose into the copy (nodes still in her 3D this frame only: gear taken off takes its bones along)
		for (auto& [node, t] : a.shown) {
			if (a.drawnWorld.contains(node)) {
				t = node->world;
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
							skin->boneWorldTransforms[i] = &a.shown.try_emplace(b, b->world).first->second;
							moved = true;
						}
					}
					if (moved && std::ranges::find(a.shownSkins, skin, [](const auto& p) { return p.get(); }) == a.shownSkins.end()) {
						a.shownSkins.emplace_back(skin);
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
	}

	void Engine::Unshow(Active& a)
	{
		if (a.shownSkins.empty()) {
			return;
		}
		// the skins are kept alive by shownSkins; their bones may be gone with her 3D (only their addresses are used)
		for (auto& skin : a.shownSkins) {
			if (!skin->skinData || !skin->bones || !skin->boneWorldTransforms) {
				continue;
			}
			for (std::uint32_t i = 0; i < skin->skinData->bones; ++i) {
				auto* b = skin->bones[i];
				if (const auto it = b ? a.shown.find(b) : a.shown.end(); it != a.shown.end() && skin->boneWorldTransforms[i] == &it->second) {
					skin->boneWorldTransforms[i] = &b->world;
				}
			}
		}
		a.shownSkins.clear();
		Retire(std::move(a.shown));
		a.shown.clear();
	}
}
