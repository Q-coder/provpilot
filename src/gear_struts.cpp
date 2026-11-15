#include "gear_struts.h"
#include "jsbgodot.h"
#include "geo_frame.h"
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace jsbgodot
{

// the shift is faded out while the gear retracts: the wheel has to fit its bay
static double gear_fade(double gear_pos)
{
	return Math::clamp((gear_pos - 0.85) / 0.13, 0.0, 1.0);
}

void GearStruts::reset() { units.clear(); }
void GearStruts::add_unit(const Unit &u) { units.push_back(u); }

// The tyre's lowest vertex below its centre, along the aircraft's up axis. A
// tyre is a body of revolution about its axle, so the figure holds while the
// wheel spins and steers.
// The tyre's radius: its lowest vertex below its centre along the aircraft's
// up axis (measured with the gear down; a tyre mesh may hold a pair of tyres).
void GearStruts::measure(Unit &u, Node3D *ac)
{
	u.measured = true;
	Ref<Mesh> mesh = u.tyre->get_mesh();
	if (mesh.is_null())
	{
		return;
	}
	const Transform3D gx = u.tyre->get_global_transform();
	const Vector3 up = ac->get_global_transform().basis.get_column(1).normalized();
	Vector3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
	double low = 1e30;
	for (int s = 0; s < mesh->get_surface_count(); s++)
	{
		const Array arrays = mesh->surface_get_arrays(s);
		if (arrays.size() <= Mesh::ARRAY_VERTEX)
		{
			continue;
		}
		const PackedVector3Array verts = arrays[Mesh::ARRAY_VERTEX];
		for (int i = 0; i < verts.size(); i++)
		{
			const Vector3 v = verts[i];
			lo = lo.min(v);
			hi = hi.max(v);
			low = Math::min(low, (double)gx.basis.xform(v).dot(up));
		}
	}
	if (low > 1e29)
	{
		return;
	}
	u.centre_local = (lo + hi) * 0.5;
	u.radius = (double)gx.basis.xform(u.centre_local).dot(up) - low;
}

// How far the tyre has to move along the aircraft's up axis to stand on the
// FDM's contact point: both heights are taken along the local vertical (the
// tyre is round: its lowest point is a radius under its centre whatever the
// pitch and bank), from the aircraft node. The node sits terrain_y_offset
// along the local vertical and origin_forward_m along the nose from the CG.
bool GearStruts::shift_needed(const Unit &u, Node3D *ac, const JSBGodot *fdm, double &out) const
{
	const Array contacts = fdm->get_gear_contacts();
	if (u.fdm_unit < 0 || u.fdm_unit >= contacts.size())
	{
		return false;
	}
	const Dictionary c = contacts[u.fdm_unit];
	const Transform3D ax = ac->get_global_transform();
	const Vector3 left = ax.basis.get_column(0).normalized(), up = ax.basis.get_column(1).normalized(), nose = ax.basis.get_column(2).normalized();
	const Vector3 vertical = geo::up_at(ax.origin);
	const double along = up.dot(vertical);
	if (along < 0.3)
	{
		return false; // on its side or inverted: leave the wheels alone
	}
	const Vector3 contact = -vertical * fdm->get_godot_terrain_y_offset() + nose * ((double)c["forward_m"] - fdm->get_origin_forward_m())
			- left * (double)c["right_m"] - up * ((double)c["down_m"] - (double)c["compression_m"]);
	const double tyre_low = (double)(u.tyre->get_global_transform().xform(u.centre_local) - ax.origin).dot(vertical) - u.radius;
	out = ((double)contact.dot(vertical) - tyre_low) / along;
	return true;
}

// The wheels are placed before the FDM steps, the picture is drawn after: a
// frame late. One frame of the shift's own rate is added to make up for it
// (a strut extends at a metre a second at lift-off).
double GearStruts::lead(Unit &u, double total)
{
	const double rate = u.has_last ? Math::clamp(total - u.last_total, -0.05, 0.05) : 0.0;
	u.last_total = total;
	u.has_last = true;
	return total + rate;
}

void GearStruts::undo_node_shifts()
{
	for (Unit &u : units)
	{
		if (u.travel_m > 0.0 || u.node == nullptr || u.applied_local == Vector3())
		{
			continue;
		}
		u.node->set_position(u.node->get_position() - u.applied_local);
		u.applied_local = Vector3();
		u.applied_m = 0.0;
	}
}

void GearStruts::apply_node_shifts(Node3D *ac, const JSBGodot *fdm, double gear_pos)
{
	if (ac == nullptr || fdm == nullptr)
	{
		return;
	}
	const double fade = gear_fade(gear_pos);
	for (Unit &u : units)
	{
		if (u.travel_m > 0.0 || u.node == nullptr || u.tyre == nullptr)
		{
			continue;
		}
		if (!u.measured)
		{
			if (fade < 1.0)
			{
				continue; // measured with the gear down
			}
			measure(u, ac);
		}
		double need = 0.0;
		if (fade <= 0.0 || !shift_needed(u, ac, fdm, need))
		{
			continue;
		}
		// the node is in its drawn pose here (shift undone, transforms rewritten)
		const double shift = Math::clamp(lead(u, need), u.min_m, u.max_m) * fade;
		Node3D *parent = Object::cast_to<Node3D>(u.node->get_parent());
		const Vector3 up_world = ac->get_global_transform().basis.get_column(1).normalized() * shift;
		const Vector3 local = parent != nullptr ? parent->get_global_transform().basis.inverse().xform(up_world) : up_world;
		u.node->set_position(u.node->get_position() + local);
		u.applied_local = local;
		u.applied_m = shift;
	}
}

void GearStruts::update_fg(Node3D *ac, const JSBGodot *fdm, double gear_pos)
{
	if (ac == nullptr || fdm == nullptr)
	{
		return;
	}
	const double fade = gear_fade(gear_pos);
	for (Unit &u : units)
	{
		if (u.travel_m <= 0.0 || u.tyre == nullptr)
		{
			continue;
		}
		if (fade >= 1.0 && !u.measured)
		{
			measure(u, ac);
		}
		// the pose on screen was drawn with last frame's shift: what is still
		// missing comes on top of it
		double need = 0.0;
		if (!u.measured || fade <= 0.0 || !shift_needed(u, ac, fdm, need))
		{
			u.norm = 0.0;
			u.applied_m = 0.0;
			continue;
		}
		if (fade >= 1.0)
		{
			// the full shift from the uncompressed strut
			u.rest_shift = lead(u, u.applied_m + need);
		}
		u.applied_m = Math::clamp(u.rest_shift, u.min_m, u.max_m) * fade;
		u.norm = u.applied_m / u.travel_m;
	}
}

} // namespace jsbgodot
