// geo_frame.h — ECEF helpers for the ProVPilot host (design §9.5, step C).
//
// World = WGS84 ECEF metres. "Up" is the local ellipsoid normal; the local
// WUN frame (+X West, +Y Up, +Z North) at a point is what the aircraft node,
// cameras and slew mode reason in. The aircraft node's local axes are the
// scenery convention: nose = +Z, up = +Y, right = -X.
#ifndef GEO_FRAME_H
#define GEO_FRAME_H

#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <cmath>
#include "s3d_proj.h"

namespace jsbgodot
{
namespace geo
{
	using godot::Vector3;
	using godot::Basis;

	struct Geodetic
	{
		double lat = 0.0, lon = 0.0, h = 0.0; // degrees, degrees, metres
	};

	inline Geodetic geodetic_of(const Vector3 &ecef)
	{
		Geodetic g;
		const s3d::proj::LatLon ll = s3d::proj::ecef_to_geodetic(s3d::proj::WGS84,
			s3d::proj::XYZ{(double)ecef.x, (double)ecef.y, (double)ecef.z}, &g.h);
		g.lat = ll.lat;
		g.lon = ll.lon;
		return g;
	}

	inline Vector3 ecef_of(double lat, double lon, double h)
	{
		const s3d::proj::XYZ p = s3d::proj::geodetic_to_ecef(s3d::proj::WGS84, lat, lon, h);
		return Vector3(p.x, p.y, p.z);
	}

	// Local WUN basis (columns = images of local X/Y/Z in ECEF).
	inline Basis wun_basis(double lat_deg, double lon_deg)
	{
		const double lat = s3d::proj::deg2rad(lat_deg), lon = s3d::proj::deg2rad(lon_deg);
		const double sl = std::sin(lat), cl = std::cos(lat), so = std::sin(lon), co = std::cos(lon);
		const Vector3 east(-so, co, 0.0);
		const Vector3 north(-sl * co, -sl * so, cl);
		const Vector3 up(cl * co, cl * so, sl);
		Basis b;
		b.set_column(0, -east);
		b.set_column(1, up);
		b.set_column(2, north);
		return b;
	}

	inline Vector3 up_at(const Vector3 &ecef)
	{
		const Geodetic g = geodetic_of(ecef);
		return wun_basis(g.lat, g.lon).get_column(1);
	}

	// True heading (degrees, clockwise from north) of an aircraft basis whose
	// nose is local +Z, at the given position.
	inline double true_heading_deg(const Basis &ac_basis, double lat, double lon)
	{
		const Basis w = wun_basis(lat, lon);
		const Vector3 f = w.transposed().xform(ac_basis.get_column(2)); // WUN-local nose
		double hdg = s3d::proj::rad2deg(std::atan2(-f.x, f.z));            // east = -X
		if (hdg < 0.0) hdg += 360.0;
		return hdg;
	}

	// Point a node's -Z at a target with the given up hint, keeping its scale.
	// Node3D::look_at() cannot be used in the ECEF world: it rejects targets
	// within CMP_EPSILON * |position| (~64 m at 6.4e6 m) as "the same
	// position", so every close camera aim silently failed.
	inline void aim(godot::Node3D *node, const Vector3 &target, const Vector3 &up)
	{
		if (node == nullptr) return;
		const Vector3 pos = node->get_global_position();
		const Vector3 fwd = target - pos;
		if (fwd.length_squared() < 1e-8 || up.length_squared() < 1e-12) return;
		const Vector3 f = fwd.normalized();
		if (std::abs(f.dot(up.normalized())) > 0.9999) return; // degenerate hint
		const Basis b = Basis::looking_at(f, up.normalized());
		const Vector3 scale = node->get_scale();
		node->set_global_transform(godot::Transform3D(b, pos));
		node->set_scale(scale);
	}

	// Level aircraft basis at (lat, lon) pointing along a true heading, with
	// the node's scale preserved (the scenery convention rotation.y = -heading
	// expressed in the local frame).
	inline Basis ac_basis(double lat, double lon, double true_heading_deg, const Vector3 &scale)
	{
		const Basis w = wun_basis(lat, lon);
		const Basis yaw(Vector3(0, 1, 0), -s3d::proj::deg2rad(true_heading_deg));
		Basis b = w * yaw;
		b.set_column(0, b.get_column(0) * scale.x);
		b.set_column(1, b.get_column(1) * scale.y);
		b.set_column(2, b.get_column(2) * scale.z);
		return b;
	}
}
}

#endif // GEO_FRAME_H
