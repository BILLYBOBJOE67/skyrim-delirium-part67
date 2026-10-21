#pragma once

namespace delirium
{
	// Static world geometry near a point, as triangles in physics space (metres, relative to an origin).
	struct Tris
	{
		std::vector<float>         pos;  // xyz per vertex
		std::vector<std::uint32_t> idx;  // 3 per triangle
		std::size_t                Count() const { return idx.size() / 3; }
	};

	struct GatherStats
	{
		int bodies{}, skippedLayer{}, shapes{}, boxes{}, hulls{}, triangles{}, fallbacks{}, culled{}, capped{};
		std::string unknown;  // shape types we boxed
	};

	// Skyrim's Havok collision of fixed things (statics, terrain, trees...) inside a box around `a_center`
	// (game units), turned into triangles relative to `a_origin` (game units), in metres. Main thread.
	bool GatherStatic(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_center, const RE::NiPoint3& a_origin, float a_radius, float a_height,
		std::size_t a_maxTris, Tris& a_out, GatherStats& a_stats);
}
