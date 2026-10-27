// Delirium's body at her size: a person's is the rig's at her scale; a creature's (draugr, skeletons, falmer: a person's
// bones at other lengths) is rebuilt on her own bones, or her feet sink into the ground and her hands miss.
#include "shared.h"

namespace delirium
{
	namespace
	{
		// each part's frame and joint in the build's own pose
		bool Frames(const NmBuild* b, std::array<V3, 21>& pos, std::array<Q, 21>& rot, std::array<V3, 21>& joint)
		{
			for (int i = 0; i < 21; ++i) {
				float p[3]{}, q[4]{}, j[3]{};
				if (dl.nm_build_part_frame(b, i, p, q) < 0 || dl.nm_build_part_joint(b, i, j) < 0) {
					return false;
				}
				pos[i] = { p[0], p[1], p[2] };
				rot[i] = { q[0], q[1], q[2], q[3] };
				joint[i] = { j[0], j[1], j[2] };
			}
			return true;
		}

		float Len(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

		float Degrees(const Q& q) { return 2.0f * std::acos(std::min(1.0f, std::abs(q.w))) * 57.2958f; }

		// Each part's turn in its parent (the rig's part frames) standing idle, recorded in game on 2026-10-27: a person
		// (Bandit Scout) and a falmer. Their difference is how far a falmer's hunch is from a person's stance.
		constexpr std::array<Q, 21> kPersonStance{ {
			{ 0, 0, 0, 1 },
			{ 0.11810f, -0.04277f, -0.74055f, 0.66016f }, { -0.13048f, -0.00015f, 0.00326f, 0.99145f }, { 0.03010f, 0.00056f, 0.07687f, 0.99659f },
			{ -0.05753f, -0.15468f, -0.66371f, 0.72956f }, { -0.13048f, -0.00021f, -0.00311f, 0.99145f }, { 0.09256f, -0.01378f, -0.02469f, 0.99531f },
			{ 0.01642f, 0.02271f, 0.00559f, 0.99959f }, { -0.05504f, -0.05903f, 0.01265f, 0.99666f }, { -0.18449f, 0.12161f, 0.01359f, 0.97519f },
			{ 0.00000f, -0.00000f, 0.00000f, 1.00000f }, { -0.10096f, -0.07665f, -0.72933f, 0.67232f }, { 0.09156f, -0.23471f, -0.03057f, 0.96726f },
			{ -0.94066f, -0.11280f, -0.11212f, 0.29979f }, { 0.49739f, 0.82270f, -0.21256f, 0.17487f }, { 0.16838f, 0.03311f, 0.07251f, 0.98249f },
			{ 0.02808f, -0.18822f, -0.05583f, 0.98014f }, { 0.19053f, 0.07602f, 0.19714f, -0.95868f }, { -0.16159f, -0.24708f, 0.41048f, -0.86275f },
			{ 0.25516f, -0.04642f, -0.03957f, 0.96497f }, { -0.01661f, 0.11699f, -0.00574f, 0.99298f },
		} };
		constexpr std::array<Q, 21> kFalmerStance{ {
			{ 0, 0, 0, 1 },
			{ -0.62130f, 0.51505f, 0.50259f, -0.31002f }, { -0.49546f, 0.00717f, -0.01876f, 0.86840f }, { -0.02334f, -0.05508f, 0.13896f, 0.98849f },
			{ -0.30937f, 0.61350f, 0.36497f, -0.62825f }, { -0.66916f, -0.00590f, 0.02557f, 0.74265f }, { 0.37304f, 0.26367f, -0.13719f, 0.87892f },
			{ 0.00953f, -0.09007f, 0.01700f, 0.99574f }, { 0.01006f, 0.21229f, 0.05319f, 0.97571f }, { 0.00383f, 0.25083f, 0.10363f, 0.96246f },
			{ 0.00000f, -0.00000f, 0.00000f, 1.00000f }, { 0.33464f, -0.04126f, -0.74129f, 0.58035f }, { -0.41542f, -0.29902f, -0.25524f, -0.82028f },
			{ 0.93651f, 0.31089f, 0.15420f, 0.05012f }, { 0.20009f, 0.91013f, -0.26323f, -0.24966f }, { 0.34657f, -0.00647f, -0.01122f, 0.93794f },
			{ -0.02748f, -0.13279f, 0.01546f, 0.99064f }, { 0.01477f, -0.10668f, -0.26353f, 0.95862f }, { -0.37351f, 0.11685f, 0.32776f, -0.85989f },
			{ 0.46364f, -0.01201f, 0.02070f, 0.88570f }, { -0.07428f, 0.17246f, -0.13380f, 0.97306f },
		} };
		// hinges keep their range: the shins (knees) and lower arms (elbows) bend through a falmer's stance as they are
		constexpr bool Hinge(std::size_t i) { return i == 2 || i == 5 || i == 15 || i == 19; }
	}

	void Engine::PersonOffsets(Active& a) const
	{
		a.fitted = false;
		for (std::size_t i = 0; i < 21; ++i) {
			a.anchor[i] = Scl(rig_[i].anchor, a.scale);  // the rig is for a scale-1 skeleton
			a.inParent[i] = Scl(rig_[i].inParent, a.scale);
			float m = 0;
			a.partMass[i] = dl.nm_build_part_info(build_, static_cast<int>(i), &m, nullptr, nullptr, nullptr) >= 0 ? m * a.scale * a.scale * a.scale : 0.0f;
		}
	}

	// Her joints, carried over into the person's own standing pose: each joint where it sits in its parent part on
	// her (her part's turn from its bone through the rig), placed with the person's parent part. Delirium rebuilds the
	// body on those joints (nm_build_remap): each part stretched to her bone, its shape, limits and muscles kept.
	NmBuild* Engine::FitCreature(Active& a, RE::Actor* a_actor) const
	{
		std::array<V3, 21> bp, bj;
		std::array<Q, 21>  bq;
		if (!Frames(build_, bp, bq, bj)) {
			logger::warn("{:08X}: fit: the body's frames: {}", a_actor->GetFormID(), LastError());
			return nullptr;
		}
		auto rel = [&](RE::NiAVObject* n) {
			const auto w = (n->world.translate - a.origin) * kUnit;
			return V3{ w.x, w.y, w.z };
		};
		// hers, as she stands now: each part's turn and joint (the pelvis: its origin, as the rig puts it on her pelvis bone)
		std::array<Q, 21>  cq;
		std::array<V3, 21> cj;
		for (std::size_t i = 0; i < 21; ++i) {
			cq[i] = Mul(FromMat(a.ref[i]->world.rotate), Conj(rig_[i].offset));
			const V3 ja = rel(a.ja[i]), jb = rel(a.jb[i]);
			cj[i] = i == 0 ? Sub(ja, Rot(cq[0], rig_[0].anchor)) : Scl(Add(ja, jb), 0.5f);
		}
		// how far she stands from the person at each joint (her part in its parent vs the person's at rest): a falmer's
		// hunch. The body's joint limits are centred on the person's, so a big bend here is a stance near or past them.
		std::string bend;
		for (std::size_t i = 1; i < 21; ++i) {
			const auto  p = static_cast<std::size_t>(kParent[i]);
			const Q     d = Mul(Conj(Mul(Conj(bq[p]), bq[i])), Mul(Conj(cq[p]), cq[i]));
			const float deg = Degrees(d);
			if (deg > 10.0f) {
				bend += fmt::format(" {} {:.0f}", rig_[i].name, deg);
			}
		}
		std::array<V3, 21> joints;
		joints[0] = bp[0];
		for (std::size_t i = 1; i < 21; ++i) {
			const auto p = static_cast<std::size_t>(kParent[i]);
			joints[i] = Add(joints[p], Rot(bq[p], Rot(Conj(cq[p]), Sub(cj[i], cj[p]))));
		}
		std::array<float, 21 * 3> jf;
		std::array<float, 21 * 4> rf;
		for (std::size_t i = 0; i < 21; ++i) {
			jf[i * 3] = joints[i].x, jf[i * 3 + 1] = joints[i].y, jf[i * 3 + 2] = joints[i].z;
			rf[i * 4] = bq[i].x, rf[i * 4 + 1] = bq[i].y, rf[i * 4 + 2] = bq[i].z, rf[i * 4 + 3] = bq[i].w;
		}
		NmBuild* fit = dl.nm_build_remap(build_, jf.data(), rf.data(), 0);
		if (!fit) {
			logger::warn("{:08X}: fit: remap: {}", a_actor->GetFormID(), LastError());
			return nullptr;
		}
		// a falmer stands hunched, past a person's joint limits at the hips and spine: each ball joint's limits moved on
		// by how far a falmer's stance is from a person's (in the parent part's frame)
		std::string turned;
		if (Hunched(a_actor)) {
			for (std::size_t i = 1; i < 21; ++i) {
				if (Hinge(i)) {
					continue;
				}
				const Q     d = Mul(kFalmerStance[i], Conj(kPersonStance[i]));
				const float q[4]{ d.x, d.y, d.z, d.w };
				if (dl.nm_build_turn_limit(fit, static_cast<int>(i), q) < 0) {
					logger::warn("{:08X}: fit: {}'s limits: {}", a_actor->GetFormID(), rig_[i].name, LastError());
				} else if (Degrees(d) > 5.0f) {
					turned += fmt::format(" {} {:.0f}", rig_[i].name, Degrees(d));
				}
			}
			logger::info("{:08X}: falmer: joint limits moved on to her stance (deg):{}", a_actor->GetFormID(), turned.empty() ? " none" : turned);
		}
		std::array<V3, 21> fp, fj;
		std::array<Q, 21>  fq;
		if (!Frames(fit, fp, fq, fj)) {
			logger::warn("{:08X}: fit: the fitted body's frames: {}", a_actor->GetFormID(), LastError());
			dl.nm_build_destroy(fit);
			return nullptr;
		}
		// the offsets from the fitted body, in the rig's terms: joint = part + turn * anchor; a part's joint in its parent
		// is the rig's inParent. The person's own body through the same code must give the rig back (logged: the check).
		auto offsets = [&](const std::array<V3, 21>& p, const std::array<Q, 21>& q, const std::array<V3, 21>& j, std::array<V3, 21>& an, std::array<V3, 21>& in) {
			for (std::size_t i = 0; i < 21; ++i) {
				an[i] = i == 0 ? rig_[0].anchor : Rot(Conj(q[i]), Sub(j[i], p[i]));
				in[i] = i == 0 ? V3{} : Rot(Conj(q[static_cast<std::size_t>(kParent[i])]), Sub(j[i], p[static_cast<std::size_t>(kParent[i])]));
			}
		};
		std::array<V3, 21> pa, pi;
		offsets(bp, bq, bj, pa, pi);
		float check = 0;
		for (std::size_t i = 1; i < 21; ++i) {
			check = std::max({ check, Len(Sub(pa[i], rig_[i].anchor)), Len(Sub(pi[i], rig_[i].inParent)) });
		}
		offsets(fp, fq, fj, a.anchor, a.inParent);
		std::string s;
		float       mass = 0, personMass = 0;
		for (std::size_t i = 0; i < 21; ++i) {
			float m = 0, pm = 0;
			dl.nm_build_part_info(fit, static_cast<int>(i), &m, nullptr, nullptr, nullptr);
			dl.nm_build_part_info(build_, static_cast<int>(i), &pm, nullptr, nullptr, nullptr);
			a.partMass[i] = m;
			mass += m;
			personMass += pm;
			if (i > 0) {
				const auto  p = static_cast<std::size_t>(kParent[i]);
				const float was = Len(Sub(bj[i], bj[p])), now = Len(Sub(fj[i], fj[p]));
				if (std::abs(now - was) > 0.005f) {
					s += fmt::format(" {} {:+.0f}cm", rig_[p].name, (now - was) * 100.0f);
				}
			}
		}
		a.fitted = true;
		logger::info("{:08X}: body fitted to her bones (rig offsets from the person's body off by {:.4f} m: the check), {:.0f} kg (a person {:.0f}):{}",
			a_actor->GetFormID(), check, mass, personMass, s.empty() ? " no part changed" : s);
		logger::info("{:08X}: her stance off the person's at rest (deg, over 10):{}", a_actor->GetFormID(), bend.empty() ? " none" : bend);
		return fit;
	}
}
