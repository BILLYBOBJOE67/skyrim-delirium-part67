#pragma once

#include "engine.h"

namespace delirium
{
	inline Q Slerp(Q a, Q b, float t)
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

	// ---- small math: Skyrim's NiMatrix3 is row-major with world = R * local, Delirium's quaternions are glam's
	inline Q Mul(const Q& a, const Q& b)
	{
		return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
	}
	inline Q  Conj(const Q& q) { return { -q.x, -q.y, -q.z, q.w }; }
	inline V3 Rot(const Q& q, const V3& v)
	{
		const V3 u{ q.x, q.y, q.z };
		const V3 t{ 2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x) };
		return { v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
			v.z + q.w * t.z + (u.x * t.y - u.y * t.x) };
	}
	inline V3 Add(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline V3 Sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline V3 Scl(const V3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }

	inline Q FromMat(const RE::NiMatrix3& m)
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

	inline float Angle(Q a, const Q& b)
	{
		const float d = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
		return 2.0f * std::acos(std::min(d, 1.0f));
	}

	inline RE::NiMatrix3 ToMat(const Q& q)
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
}
