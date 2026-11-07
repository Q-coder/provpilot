#include "fg_animator.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <cstdio>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/math.hpp>

using namespace godot;

namespace jsbgodot
{
    FGAnimator::FGAnimator() {}

    void FGAnimator::_bind_methods()
    {
        ClassDB::bind_method(D_METHOD("load", "manifest_path", "model_root"), &FGAnimator::load);
        ClassDB::bind_method(D_METHOD("clear"), &FGAnimator::clear);
        ClassDB::bind_method(D_METHOD("set_property", "path", "value"), &FGAnimator::set_property);
        ClassDB::bind_method(D_METHOD("set_properties", "values"), &FGAnimator::set_properties);
        ClassDB::bind_method(D_METHOD("get_property", "path"), &FGAnimator::get_property);
        ClassDB::bind_method(D_METHOD("property_names"), &FGAnimator::property_names);
        ClassDB::bind_method(D_METHOD("set_texture_root", "dir"), &FGAnimator::set_texture_root);
        ClassDB::bind_method(D_METHOD("get_texture_root"), &FGAnimator::get_texture_root);
        ClassDB::bind_method(D_METHOD("unknown_properties"), &FGAnimator::unknown_properties);
        ClassDB::bind_method(D_METHOD("get_aliases"), &FGAnimator::get_aliases);
        ClassDB::bind_method(D_METHOD("update", "delta"), &FGAnimator::update);
        ClassDB::bind_method(D_METHOD("animation_count"), &FGAnimator::animation_count);
        ClassDB::bind_method(D_METHOD("node_count"), &FGAnimator::node_count);
        ClassDB::bind_method(D_METHOD("stats"), &FGAnimator::stats);
        ClassDB::bind_method(D_METHOD("animated_transform", "node_name"), &FGAnimator::animated_transform);
    }

    // ── helpers ────────────────────────────────────────────────────────

    String FGAnimator::normalise(const String &path)
    {
        String p = path.strip_edges();
        while (p.begins_with("/"))
            p = p.substr(1);
        p = p.replace("[0]", "");
        return p;
    }

    bool FGAnimator::is_number(const String &s)
    {
        return s.is_valid_float() || s.is_valid_int();
    }

    double FGAnimator::num(const Variant &v, double def)
    {
        switch (v.get_type())
        {
        case Variant::FLOAT:
        case Variant::INT:
            return (double)v;
        case Variant::STRING:
        {
            String s = v;
            s = s.strip_edges();
            return is_number(s) ? s.to_float() : def;
        }
        default:
            return def;
        }
    }

    int FGAnimator::prop_id(const String &path)
    {
        String p = normalise(path);
        if (p.is_empty())
            return -1;
        if (prop_index.has(p))
            return prop_index[p];
        int id = prop_names.size();
        prop_index[p] = id;
        prop_names.push_back(p);
        prop_values.push_back(0.0);
        prop_strings.push_back(String());
        prop_set.push_back(false);
        prop_is_string.push_back(false);
        return id;
    }

    // FlightGear (x aft, y right, z up) -> glTF/Godot (x aft, y up, z left)
    Vector3 FGAnimator::fg_vec(const Dictionary &d, const char *x, const char *y, const char *z, const Vector3 &def)
    {
        if (d.is_empty())
            return def;
        double fx = num(d.get(x, Variant()), 0.0);
        double fy = num(d.get(y, Variant()), 0.0);
        double fz = num(d.get(z, Variant()), 0.0);
        return Vector3(fx, fz, -fy);
    }

    // ── conditions ─────────────────────────────────────────────────────
    // A condition element is a dict; sibling children are implicitly ANDed.
    // Repeated tags arrive as lists.

    int FGAnimator::parse_cond_children(const Dictionary &d, CondKind combine)
    {
        Cond c;
        c.kind = combine;
        Array keys = d.keys();
        for (int i = 0; i < keys.size(); i++)
        {
            String key = keys[i];
            if (key == "@")
                continue;
            Variant v = d[keys[i]];
            Array items;
            if (v.get_type() == Variant::ARRAY)
                items = v;
            else
                items.push_back(v);
            for (int j = 0; j < items.size(); j++)
            {
                Dictionary one;
                one[key] = items[j];
                int idx = parse_cond(one);
                if (idx >= 0)
                    c.children.push_back(idx);
            }
        }
        if (c.children.size() == 1 && combine == C_AND)
            return c.children[0];
        conds.push_back(c);
        return conds.size() - 1;
    }

    int FGAnimator::parse_cond(const Variant &v)
    {
        if (v.get_type() != Variant::DICTIONARY)
        {
            // bare <property>x</property> as text
            if (v.get_type() == Variant::STRING)
            {
                Cond c;
                c.kind = C_PROP;
                c.prop = prop_id(v);
                conds.push_back(c);
                return conds.size() - 1;
            }
            return -1;
        }
        Dictionary d = v;
        Array keys = d.keys();
        if (keys.size() != 1)
            return parse_cond_children(d, C_AND);
        String key = keys[0];
        Variant body = d[keys[0]];
        if (key == "and")
            return parse_cond_children(body.get_type() == Variant::DICTIONARY ? (Dictionary)body : Dictionary(), C_AND);
        if (key == "or")
            return parse_cond_children(body.get_type() == Variant::DICTIONARY ? (Dictionary)body : Dictionary(), C_OR);
        if (key == "not")
        {
            Cond c;
            c.kind = C_NOT;
            int inner = body.get_type() == Variant::DICTIONARY ? parse_cond_children((Dictionary)body, C_AND) : parse_cond(body);
            if (inner >= 0)
                c.children.push_back(inner);
            conds.push_back(c);
            return conds.size() - 1;
        }
        if (key == "property")
        {
            Cond c;
            c.kind = C_PROP;
            c.prop = prop_id(body);
            conds.push_back(c);
            return conds.size() - 1;
        }
        CondKind k = C_TRUE;
        if (key == "equals")
            k = C_EQ;
        else if (key == "not-equals")
            k = C_NE;
        else if (key == "less-than")
            k = C_LT;
        else if (key == "less-than-equals")
            k = C_LE;
        else if (key == "greater-than")
            k = C_GT;
        else if (key == "greater-than-equals")
            k = C_GE;
        else
            return -1; // unknown element (e.g. <value> at top level)
        Cond c;
        c.kind = k;
        if (body.get_type() == Variant::DICTIONARY)
        {
            Dictionary b = body;
            Variant p = b.get("property", Variant());
            if (p.get_type() == Variant::ARRAY)
            {
                Array pa = p;
                if (pa.size() > 0)
                    c.prop = prop_id(pa[0]);
                if (pa.size() > 1)
                    c.prop2 = prop_id(pa[1]);
            }
            else if (p.get_type() == Variant::STRING)
                c.prop = prop_id(p);
            if (b.has("value"))
            {
                Variant val = b["value"];
                String sv = val.get_type() == Variant::STRING ? (String)val : String::num((double)val);
                if (is_number(sv.strip_edges()))
                    c.value = sv.to_float();
                else
                {
                    c.is_string = true;
                    c.svalue = sv.strip_edges();
                }
            }
        }
        conds.push_back(c);
        return conds.size() - 1;
    }

    bool FGAnimator::eval(int idx) const
    {
        if (idx < 0)
            return true;
        const Cond &c = conds[idx];
        switch (c.kind)
        {
        case C_TRUE:
            return true;
        case C_PROP:
            return c.prop >= 0 && (prop_values[c.prop] != 0.0 || (!prop_strings[c.prop].is_empty() && prop_strings[c.prop] != "false"));
        case C_AND:
            for (int i = 0; i < c.children.size(); i++)
                if (!eval(c.children[i]))
                    return false;
            return true;
        case C_OR:
            for (int i = 0; i < c.children.size(); i++)
                if (eval(c.children[i]))
                    return true;
            return c.children.size() == 0;
        case C_NOT:
            return c.children.size() == 0 ? false : !eval(c.children[0]);
        default:
            break;
        }
        if (c.prop < 0)
            return false;
        if (c.is_string)
        {
            bool eq = prop_strings[c.prop] == c.svalue;
            return c.kind == C_EQ ? eq : (c.kind == C_NE ? !eq : false);
        }
        double a = prop_values[c.prop];
        double b = c.prop2 >= 0 ? prop_values[c.prop2] : c.value;
        switch (c.kind)
        {
        case C_EQ:
            return Math::abs(a - b) < 1e-9;
        case C_NE:
            return Math::abs(a - b) >= 1e-9;
        case C_LT:
            return a < b;
        case C_LE:
            return a <= b;
        case C_GT:
            return a > b;
        case C_GE:
            return a >= b;
        default:
            return false;
        }
    }

    // ── loading ────────────────────────────────────────────────────────

    // AC3D materials with a flat "emis 1 1 1" (the A-10's lit caution lamps,
    // "A-10-warn-panel-on") arrive as a white emission that floods the label.
    // FlightGear's lit result shows the texture itself; draw such surfaces
    // unshaded so the lamp reads (the texture carries its own brightness).
    void FGAnimator::fixup_emissive_materials(Node *root)
    {
        Vector<Node *> stack;
        stack.push_back(root);
        int fixed = 0;
        int blended = 0;
        while (!stack.is_empty())
        {
            Node *n = stack[stack.size() - 1];
            stack.remove_at(stack.size() - 1);
            for (int i = 0; i < n->get_child_count(); i++)
                stack.push_back(n->get_child(i));
            MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(n);
            if (mi == nullptr)
                continue;
            Ref<Mesh> mesh = mi->get_mesh();
            if (mesh.is_null())
                continue;
            for (int s = 0; s < mesh->get_surface_count(); s++)
            {
                Ref<Material> m = mi->get_active_material(s);
                BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(m.ptr());
                if (bm == nullptr)
                    continue;
                // FlightGear blends any texture that carries alpha (instrument
                // glass, shadow planes, decals); the AC3D import leaves the
                // material opaque, so the transparent parts came out grey
                if (bm->get_transparency() == BaseMaterial3D::TRANSPARENCY_DISABLED)
                {
                    Ref<Texture2D> tex = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
                    Ref<Image> img = tex.is_valid() ? tex->get_image() : Ref<Image>();
                    if (img.is_valid() && img->detect_alpha() != Image::ALPHA_NONE)
                    {
                        bm->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
                        blended++;
                    }
                }
                if (!bm->get_feature(BaseMaterial3D::FEATURE_EMISSION))
                    continue;
                Ref<Texture2D> albedo = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
                Color e = bm->get_emission();
                // an imported emissive texture is the lit texture itself: colour x texture, not colour + texture
                if (bm->get_texture(BaseMaterial3D::TEXTURE_EMISSION).is_valid())
                    bm->set_emission_operator(BaseMaterial3D::EMISSION_OP_MULTIPLY);
                if (albedo.is_valid() && bm->get_texture(BaseMaterial3D::TEXTURE_EMISSION).is_null() && e.r > 0.9 && e.g > 0.9 && e.b > 0.9)
                {
                    // a self-lit texture: show it as it is, no sunlight on top,
                    // no flat emission (which saturated the label to white)
                    bm->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
                    bm->set_feature(BaseMaterial3D::FEATURE_EMISSION, false);
                    fixed++;
                }
            }
        }
        if (fixed > 0)
            UtilityFunctions::print("FGAnimator: ", fixed, " self-lit textured surfaces drawn unshaded");
        if (blended > 0)
            UtilityFunctions::print("FGAnimator: ", blended, " surfaces with alpha textures alpha-blended");
    }

    void FGAnimator::collect_nodes(Node *n, HashMap<String, Vector<Node *>> &by_name)
    {
        String name = n->get_name();
        // Blender duplicate suffix ".001" arrives as "_001": index under the stem too
        by_name[name].push_back(n);
        if (name.length() > 4 && name[name.length() - 4] == '_')
        {
            String tail = name.substr(name.length() - 3);
            if (tail.is_valid_int())
                by_name[name.substr(0, name.length() - 4)].push_back(n);
        }
        for (int i = 0; i < n->get_child_count(); i++)
            collect_nodes(n->get_child(i), by_name);
    }

    int FGAnimator::node_state_for(Node3D *n, Node *frame_root)
    {
        String key = String::num_int64((int64_t)n->get_instance_id());
        if (node_index.has(key))
            return node_index[key];
        NodeState s;
        s.node = n;
        s.node_id = n->get_instance_id();
        s.base = n->get_transform();
        // accumulate the un-animated transforms from the model root down to the parent
        Transform3D pf;
        Node *p = n->get_parent();
        while (p != nullptr && p != model_root && p != frame_root)
        {
            Node3D *p3 = Object::cast_to<Node3D>(p);
            if (p3 != nullptr)
                pf = p3->get_transform() * pf;
            p = p->get_parent();
        }
        s.parent_file = pf;
        s.parent_file_inv = pf.affine_inverse();
        node_states.push_back(s);
        node_index[key] = node_states.size() - 1;
        return node_states.size() - 1;
    }

    int FGAnimator::mat_state_for(MeshInstance3D *m)
    {
        String key = String::num_int64((int64_t)m->get_instance_id());
        if (mat_index.has(key))
            return mat_index[key];
        MatState s;
        s.mesh = m;
        Ref<Mesh> mesh = m->get_mesh();
        int n = mesh.is_valid() ? mesh->get_surface_count() : 0;
        for (int i = 0; i < n; i++)
        {
            Ref<Material> src = m->get_active_material(i);
            Ref<Material> dup;
            if (src.is_valid())
            {
                dup = src->duplicate();
                m->set_surface_override_material(i, dup);
            }
            s.mats.push_back(dup);
        }
        mat_states.push_back(s);
        mat_index[key] = mat_states.size() - 1;
        return mat_states.size() - 1;
    }

    void FGAnimator::clear()
    {
        // the previous model may already be freed (aircraft swap): look the
        // node up by its ObjectID, never touch the pointer first
        for (int i = 0; i < node_states.size(); i++)
            if (node_states[i].node != nullptr && ObjectDB::get_instance(ObjectID(node_states[i].node_id)) != nullptr)
            {
                node_states[i].node->set_transform(node_states[i].base);
                node_states[i].node->set_visible(true);
            }
        for (int i = 0; i < text_states.size(); i++)
        {
            Object *o = ObjectDB::get_instance(ObjectID(text_states[i].label_id));
            Node *n = Object::cast_to<Node>(o);
            if (n != nullptr)
                n->queue_free();
        }
        text_states.clear();
        fonts.clear();
        conds.clear();
        anims.clear();
        node_states.clear();
        mat_states.clear();
        node_index.clear();
        mat_index.clear();
        prop_index.clear();
        prop_names.clear();
        prop_values.clear();
        prop_strings.clear();
        prop_set.clear();
        type_counts.clear();
        skipped = 0;
        model_root = nullptr;
        file_root = nullptr;
        model_roots.clear();
    }

    bool FGAnimator::load(const String &manifest_path, Node *p_model_root)
    {
        clear();
        if (p_model_root == nullptr)
            return false;
        model_root = p_model_root;
        String text = FileAccess::get_file_as_string(manifest_path);
        if (text.is_empty())
        {
            UtilityFunctions::push_warning("FGAnimator: cannot read ", manifest_path);
            return false;
        }
        Variant parsed = JSON::parse_string(text);
        if (parsed.get_type() != Variant::DICTIONARY)
        {
            UtilityFunctions::push_warning("FGAnimator: manifest is not valid JSON: ", manifest_path);
            return false;
        }
        Dictionary m = parsed;
        HashMap<String, Vector<Node *>> by_name;
        collect_nodes(p_model_root, by_name);
        fixup_emissive_materials(p_model_root);
        String root_name = m.get("root", "");
        file_root = nullptr;
        if (!root_name.is_empty() && by_name.has(root_name) && by_name[root_name].size() > 0)
            file_root = by_name[root_name][0];
        if (file_root == nullptr)
            UtilityFunctions::push_warning("FGAnimator: manifest root node '", root_name, "' not found; using the model root as the file frame");
        aliases = m.get("aliases", Dictionary());
        load_texts(m, manifest_path.get_base_dir(), by_name);
        model_roots.clear();
        model_dirs.clear();
        textures.clear();
        Array subs = m.get("submodels", Array());
        for (int i = 0; i < subs.size(); i++)
        {
            Dictionary sd = subs[i];
            String tag = sd.get("tag", "");
            String node = sd.get("node", "");
            // "Models/Instruments/n1L.xml" -> "Instruments": texture-prop files
            // are relative to the record's XML, the liveries tree to Models/
            String xdir = String(sd.get("xml", "")).get_base_dir();
            if (xdir == "Models")
                xdir = String();
            else if (xdir.begins_with("Models/"))
                xdir = xdir.substr(7);
            if (!model_dirs.has(tag))
                model_dirs[tag] = xdir;
            if (by_name.has(node) && by_name[node].size() > 0)
                model_roots[tag] = by_name[node][0];
        }
        // FlightGear knob / slider records are the part's own rotate / translate
        // animation plus the click bindings: animate them like the rest
        Array list = m.get("animations", Array());
        Array ctrls = m.get("controls", Array());
        for (int i = 0; i < ctrls.size(); i++)
        {
            Dictionary rec = ctrls[i];
            String ct = rec.get("type", "");
            Dictionary raw = rec.get("raw", Dictionary());
            if ((ct == "knob" || ct == "slider") && raw.has("property"))
            {
                Dictionary copy = rec.duplicate(true);
                copy["type"] = ct == "knob" ? "rotate" : "translate";
                copy["control"] = true;
                list.push_back(copy);
            }
        }
        for (int i = 0; i < list.size(); i++)
        {
            Dictionary rec = list[i];
            String type = rec.get("type", "");
            type_counts[type] = (int)type_counts.get(type, 0) + 1;
            Dictionary raw = rec.get("raw", Dictionary());
            Anim a;
            if (type == "rotate")
                a.type = A_ROTATE;
            else if (type == "translate")
                a.type = A_TRANSLATE;
            else if (type == "spin")
                a.type = A_SPIN;
            else if (type == "select")
                a.type = A_SELECT;
            else if (type == "material")
                a.type = A_MATERIAL;
            else if (type == "textranslate")
                a.type = A_TEXTRANSLATE;
            else if (type == "texrotate")
                a.type = A_TEXROTATE;
            else
            {
                skipped++;
                continue;
            }
            a.model = rec.get("model", "");
            // the frame node of this record's own sub-model instance ("frame",
            // exact name): a file imported several times shares its tag, and a
            // centre resolved in another copy's frame swings the part metres away
            Node *rec_frame = nullptr;
            const String frame_name = rec.get("frame", "");
            if (!frame_name.is_empty() && by_name.has(frame_name))
            {
                const Vector<Node *> &fv = by_name[frame_name];
                for (int j = 0; j < fv.size(); j++)
                    if (String(fv[j]->get_name()) == frame_name)
                        rec_frame = fv[j];
            }
            // targets
            Array objs = rec.get("objects", Array());
            // "exact": the record names one node (a sub-model's placement empty,
            // from <model><condition>); the Blender duplicate stem must not pull
            // in the other copies of the same file
            const bool exact = bool(rec.get("exact", false));
            Vector<Node *> targets;
            for (int j = 0; j < objs.size(); j++)
            {
                String nm = objs[j];
                if (by_name.has(nm))
                {
                    const Vector<Node *> &v = by_name[nm];
                    for (int k = 0; k < v.size(); k++)
                        if (!exact || String(v[k]->get_name()) == nm)
                            targets.push_back(v[k]);
                }
            }
            if (targets.is_empty())
                continue;
            // common fields
            a.prop = prop_id(raw.get("property", ""));
            a.factor = num(raw.get("factor", Variant()), 1.0);
            if (raw.has("condition"))
                a.cond = parse_cond(raw["condition"]);
            Dictionary interp = raw.get("interpolation", Dictionary());
            if (!interp.is_empty())
            {
                Variant ent = interp.get("entry", Variant());
                Array entries;
                if (ent.get_type() == Variant::ARRAY)
                    entries = ent;
                else if (ent.get_type() == Variant::DICTIONARY)
                    entries.push_back(ent);
                for (int j = 0; j < entries.size(); j++)
                {
                    Dictionary e = entries[j];
                    a.table_in.push_back(num(e.get("ind", 0.0), 0.0));
                    a.table_out.push_back(num(e.get("dep", 0.0), 0.0));
                }
                // FlightGear sorts the entries by their input (the ASK 21's airbrake
                // lever lists 1 -> 0 before 0 -> 0.21); the lookup needs ascending keys
                for (int j = 1; j < a.table_in.size(); j++)
                {
                    for (int k = j; k > 0 && a.table_in[k - 1] > a.table_in[k]; k--)
                    {
                        const double ti = a.table_in[k - 1], to = a.table_out[k - 1];
                        a.table_in.write[k - 1] = a.table_in[k];
                        a.table_out.write[k - 1] = a.table_out[k];
                        a.table_in.write[k] = ti;
                        a.table_out.write[k] = to;
                    }
                }
            }
            switch (a.type)
            {
            case A_ROTATE:
            case A_SPIN:
            {
                a.offset = num(raw.get("offset-deg", Variant()), 0.0);
                a.has_min = raw.has("min-deg");
                a.has_max = raw.has("max-deg");
                a.min_v = num(raw.get("min-deg", Variant()), 0.0);
                a.max_v = num(raw.get("max-deg", Variant()), 0.0);
                Dictionary axis = raw.get("axis", Dictionary());
                Dictionary center = raw.get("center", Dictionary());
                if (axis.has("x1-m") || axis.has("y1-m") || axis.has("z1-m"))
                {
                    Vector3 p1 = fg_vec(axis, "x1-m", "y1-m", "z1-m", Vector3());
                    Vector3 p2 = fg_vec(axis, "x2-m", "y2-m", "z2-m", Vector3());
                    a.axis = (p2 - p1);
                    a.center = center.is_empty() ? p1 : fg_vec(center, "x-m", "y-m", "z-m", p1);
                }
                else
                {
                    a.axis = fg_vec(axis, "x", "y", "z", Vector3(0, 1, 0));
                    a.center = fg_vec(center, "x-m", "y-m", "z-m", Vector3());
                }
                if (a.axis.length_squared() < 1e-12)
                    a.axis = Vector3(0, 1, 0);
                a.axis.normalize();
                break;
            }
            case A_TRANSLATE:
            {
                a.offset = num(raw.get("offset-m", Variant()), 0.0);
                a.has_min = raw.has("min-m");
                a.has_max = raw.has("max-m");
                a.min_v = num(raw.get("min-m", Variant()), 0.0);
                a.max_v = num(raw.get("max-m", Variant()), 0.0);
                Dictionary axis = raw.get("axis", Dictionary());
                if (axis.has("x1-m") || axis.has("y1-m") || axis.has("z1-m"))
                {
                    // the direction from one point to another (the ASK 21's airbrake lever slides along its slot)
                    a.axis = fg_vec(axis, "x2-m", "y2-m", "z2-m", Vector3()) - fg_vec(axis, "x1-m", "y1-m", "z1-m", Vector3());
                }
                else
                {
                    a.axis = fg_vec(axis, "x", "y", "z", Vector3(1, 0, 0));
                }
                if (a.axis.length_squared() < 1e-12)
                    a.axis = Vector3(1, 0, 0);
                a.axis.normalize();
                break;
            }
            case A_TEXTRANSLATE:
            {
                a.offset = num(raw.get("offset", Variant()), 0.0);
                a.step = num(raw.get("step", Variant()), 0.0);
                a.scroll = num(raw.get("scroll", Variant()), 0.0);
                a.bias = num(raw.get("bias", Variant()), 0.0);
                Dictionary axis = raw.get("axis", Dictionary());
                a.tex_axis = Vector2(num(axis.get("x", 0.0), 0.0), num(axis.get("y", 0.0), 0.0));
                if (a.tex_axis.length_squared() < 1e-12)
                    a.tex_axis = Vector2(1, 0);
                break;
            }
            case A_TEXROTATE:
            {
                // FlightGear rotates the texture about <center> (texture coordinates)
                // by property * factor degrees; the yaw string is drawn this way
                Dictionary ctr = raw.get("center", Dictionary());
                a.tex_center = Vector2(num(ctr.get("x", 0.5), 0.5), num(ctr.get("y", 0.5), 0.5));
                a.offset = num(raw.get("offset", Variant()), 0.0);
                break;
            }
            case A_MATERIAL:
            {
                a.transparent = bool(raw.get("transparent", false));
                if (raw.has("texture-prop"))
                {
                    const String pbase = raw.get("property-base", "");
                    const String tp = raw["texture-prop"];
                    a.tex_prop = prop_id(pbase.is_empty() ? tp : pbase.path_join(tp));
                    a.tex_default = raw.get("texture", "");
                    a.tex_dir = model_dirs.has(a.model) ? model_dirs[a.model] : String();
                }
                Dictionary em = raw.get("emission", Dictionary());
                if (!em.is_empty())
                {
                    a.has_emission = true;
                    a.emission = Color(num(em.get("red", 0.0), 0.0), num(em.get("green", 0.0), 0.0), num(em.get("blue", 0.0), 0.0));
                    if (em.has("factor-prop"))
                    {
                        a.emission_is_prop = true;
                        a.emission_factor_prop = prop_id(em["factor-prop"]);
                    }
                }
                Dictionary di = raw.get("diffuse", Dictionary());
                if (!di.is_empty() && di.has("red"))
                {
                    a.has_diffuse = true;
                    a.diffuse = Color(num(di.get("red", 0.0), 0.0), num(di.get("green", 0.0), 0.0), num(di.get("blue", 0.0), 0.0));
                    if (di.has("factor-prop"))
                        a.diffuse_factor_prop = prop_id(di["factor-prop"]);
                }
                Dictionary tr = raw.get("transparency", Dictionary());
                if (!tr.is_empty())
                {
                    if (tr.has("alpha-prop"))
                        a.alpha_prop = prop_id(tr["alpha-prop"]);
                    if (tr.has("alpha"))
                    {
                        a.has_alpha = true;
                        a.alpha = num(tr["alpha"], 1.0);
                    }
                }
                break;
            }
            default:
                break;
            }
            Node *frame_root = rec_frame != nullptr ? rec_frame : (model_roots.has(a.model) ? model_roots[a.model] : file_root);
            if ((bool)rec.get("control", false) && (a.type == A_ROTATE || a.type == A_SPIN) && !((Dictionary)raw.get("center", Dictionary())).size())
            {
                // a knob without <center> turns about its own bounds centre
                MeshInstance3D *mi = nullptr;
                for (int j = 0; j < targets.size() && mi == nullptr; j++)
                {
                    mi = Object::cast_to<MeshInstance3D>(targets[j]);
                    if (mi == nullptr)
                    {
                        Vector<Node *> stack;
                        stack.push_back(targets[j]);
                        while (!stack.is_empty() && mi == nullptr)
                        {
                            Node *n = stack[stack.size() - 1];
                            stack.remove_at(stack.size() - 1);
                            mi = Object::cast_to<MeshInstance3D>(n);
                            for (int k = 0; k < n->get_child_count() && mi == nullptr; k++)
                                stack.push_back(n->get_child(k));
                        }
                    }
                }
                if (mi != nullptr)
                {
                    Transform3D f;
                    Node *p = mi;
                    while (p != nullptr && p != model_root && p != frame_root)
                    {
                        Node3D *p3 = Object::cast_to<Node3D>(p);
                        if (p3 != nullptr)
                            f = p3->get_transform() * f;
                        p = p->get_parent();
                    }
                    a.center = f.xform(mi->get_aabb().get_center());
                }
            }
            int ai = anims.size();
            anims.push_back(a);
            for (int j = 0; j < targets.size(); j++)
            {
                if (a.type == A_MATERIAL || a.type == A_TEXTRANSLATE || a.type == A_TEXROTATE)
                {
                    // apply to every mesh in the target subtree
                    Vector<Node *> stack;
                    stack.push_back(targets[j]);
                    while (!stack.is_empty())
                    {
                        Node *n = stack[stack.size() - 1];
                        stack.remove_at(stack.size() - 1);
                        MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(n);
                        if (mi != nullptr)
                            anims.write[ai].targets.push_back(mat_state_for(mi));
                        Label3D *lb = Object::cast_to<Label3D>(n);
                        if (lb != nullptr && a.type == A_MATERIAL)
                            anims.write[ai].labels.push_back(lb->get_instance_id());
                        for (int k = 0; k < n->get_child_count(); k++)
                            stack.push_back(n->get_child(k));
                    }
                }
                else
                {
                    Node3D *n3 = Object::cast_to<Node3D>(targets[j]);
                    if (n3 == nullptr)
                        continue;
                    int si = node_state_for(n3, frame_root);
                    anims.write[ai].targets.push_back(si);
                    if (a.type == A_SELECT)
                        node_states.write[si].selects.push_back(ai);
                    else
                        node_states.write[si].anims.push_back(ai);
                }
            }
        }
        UtilityFunctions::print("FGAnimator: ", anims.size(), " animations on ", node_states.size(), " nodes and ", mat_states.size(),
                                " materials, ", text_states.size(), " texts, ", prop_names.size(), " properties, ", skipped, " records of other types skipped");
        return true;
    }

    // ── <text> ─────────────────────────────────────────────────────────
    // FlightGear draws osgText at the text's <offsets> in the model file's
    // frame: a printf-formatted number (number-value), a string property
    // (text-value) or a literal, in a font of <character-size> metres,
    // anchored by <alignment> ("right-bottom"), lying in <axis-alignment>
    // ("yz-plane": baseline along +y, up +z, facing +x aft = towards the
    // pilots). The builder placed an empty per text; a Label3D goes under it.
    void FGAnimator::load_texts(const Dictionary &m, const String &manifest_dir, HashMap<String, Vector<Node *>> &by_name)
    {
        const Dictionary font_files = m.get("fonts", Dictionary());
        const Array texts = m.get("texts", Array());
        int missing = 0;
        for (int i = 0; i < texts.size(); i++)
        {
            const Dictionary rec = texts[i];
            const Dictionary raw = rec.get("raw", Dictionary());
            const String node_name = rec.get("node", "");
            Node3D *anchor = nullptr;
            if (by_name.has(node_name))
            {
                const Vector<Node *> &v = by_name[node_name];
                for (int k = 0; k < v.size() && anchor == nullptr; k++)
                    if (String(v[k]->get_name()) == node_name)
                        anchor = Object::cast_to<Node3D>(v[k]);
            }
            if (anchor == nullptr)
            {
                missing++;
                continue;
            }
            Label3D *lb = memnew(Label3D);
            lb->set_name("Text");
            const String kind = raw.get("type", "literal");
            TextState ts;
            ts.kind = kind == "number-value" ? 1 : (kind == "text-value" ? 2 : 0);
            ts.format = String(raw.get("format", ts.kind == 2 ? "%s" : "%f"));
            if (ts.kind == 0)
                ts.last = String(raw.get("text", ""));
            else
                ts.prop = prop_id(raw.get("property", ""));
            lb->set_text(ts.last);
            // font: the aircraft's own file when the builder copied it, else Godot's default
            const String font = raw.get("font", "");
            if (!font.is_empty() && font_files.has(font))
            {
                if (!fonts.has(font))
                {
                    Ref<FontFile> ff;
                    ff.instantiate();
                    if (ff->load_dynamic_font(manifest_dir.path_join(font_files[font])) == OK)
                        fonts[font] = ff;
                    else
                        fonts[font] = Ref<Font>();
                }
                if (fonts[font].is_valid())
                    lb->set_font(fonts[font]);
            }
            // character-size is the glyph height in metres: 64 px glyphs scaled to it
            const double char_m = num(raw.get("character-size", 0.01), 0.01);
            lb->set_font_size(64);
            lb->set_pixel_size(char_m / 64.0);
            const String align = raw.get("alignment", "left-baseline");
            lb->set_horizontal_alignment(align.begins_with("right") ? HORIZONTAL_ALIGNMENT_RIGHT : (align.begins_with("center") ? HORIZONTAL_ALIGNMENT_CENTER : HORIZONTAL_ALIGNMENT_LEFT));
            lb->set_vertical_alignment(align.ends_with("top") ? VERTICAL_ALIGNMENT_TOP : (align.ends_with("center") ? VERTICAL_ALIGNMENT_CENTER : VERTICAL_ALIGNMENT_BOTTOM));
            // Label3D: baseline +x, up +y, facing +z; FlightGear's planes in the
            // Godot frame (x aft, y up, z left = -y_fg)
            const String plane = raw.get("axis-alignment", "xy-plane");
            Basis b;
            if (plane == "yz-plane")
                b = Basis(Vector3(0, 1, 0), Math_PI / 2); // facing +x (aft), baseline -z (= +y_fg, right)
            else if (plane == "reversed-yz-plane")
                b = Basis(Vector3(0, 1, 0), -Math_PI / 2);
            else if (plane == "xy-plane")
                b = Basis(Vector3(1, 0, 0), -Math_PI / 2); // facing up
            else if (plane == "reversed-xy-plane")
                b = Basis(Vector3(1, 0, 0), Math_PI / 2);
            else if (plane == "reversed-xz-plane")
                b = Basis(Vector3(0, 1, 0), Math_PI);
            // a hair in front of the panel face so the digits never z-fight the window
            lb->set_transform(Transform3D(b, b.xform(Vector3(0, 0, 0.0003))));
            lb->set_draw_flag(Label3D::FLAG_SHADED, false);
            lb->set_draw_flag(Label3D::FLAG_DOUBLE_SIDED, false);
            lb->set_alpha_cut_mode(Label3D::ALPHA_CUT_DISCARD);
            lb->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
            anchor->add_child(lb);
            ts.label_id = lb->get_instance_id();
            text_states.push_back(ts);
            by_name[String(lb->get_name())].push_back(lb);
        }
        if (missing > 0)
            UtilityFunctions::push_warning("FGAnimator: ", missing, " text anchors not found in the model");
        if (texts.size() > 0)
            type_counts["text"] = text_states.size();
    }

    // printf with FlightGear's one-conversion formats ("%6.2f", ".%003.0f",
    // "+%04.0f", "%s"): the value goes through snprintf like osgText does
    String FGAnimator::format_text(const String &fmt, double v, const String &str, int kind)
    {
        const CharString f = fmt.utf8();
        const char *c = f.get_data();
        int convs = 0;
        char conv = 0;
        for (const char *p = c; *p; p++)
        {
            if (*p != '%')
                continue;
            if (p[1] == '%')
            {
                p++;
                continue;
            }
            const char *q = p + 1;
            while (*q && strchr("-+ 0#.123456789", *q))
                q++;
            conv = *q;
            convs++;
        }
        char buf[128];
        if (convs != 1 || conv == 0)
            return kind == 2 ? str : String::num(v, 2);
        if (strchr("fFeEgG", conv))
            snprintf(buf, sizeof(buf), c, v);
        else if (strchr("diouxX", conv))
        {
            // promote the length to long long whatever the format says
            String g = fmt;
            const int at = g.rfind(String::chr(conv));
            g = g.substr(0, at) + "ll" + g.substr(at);
            snprintf(buf, sizeof(buf), g.utf8().get_data(), (long long)std::llround(v));
        }
        else if (conv == 's')
        {
            const CharString sc = str.utf8();
            snprintf(buf, sizeof(buf), c, sc.get_data());
        }
        else
            return kind == 2 ? str : String::num(v, 2);
        return String::utf8(buf);
    }

    int FGAnimator::load_screen_texts(const String &json_path)
    {
        if (model_root == nullptr || json_path.is_empty() || !FileAccess::file_exists(json_path))
            return 0;
        Ref<FileAccess> f = FileAccess::open(json_path, FileAccess::READ);
        if (f.is_null())
            return 0;
        const Variant parsed = JSON::parse_string(f->get_as_text());
        if (parsed.get_type() != Variant::DICTIONARY)
        {
            UtilityFunctions::push_warning("FGAnimator: screen_texts is not a JSON object: ", json_path);
            return 0;
        }
        HashMap<String, Vector<Node *>> by_name;
        collect_nodes(model_root, by_name);
        const Array screens = Dictionary(parsed).get("screens", Array());
        int made = 0;
        for (int i = 0; i < screens.size(); i++)
        {
            const Dictionary sc = screens[i];
            const String node_name = sc.get("node", "");
            MeshInstance3D *mi = nullptr;
            if (by_name.has(node_name))
            {
                const Vector<Node *> &v = by_name[node_name];
                for (int k = 0; k < v.size() && mi == nullptr; k++)
                    mi = Object::cast_to<MeshInstance3D>(v[k]);
            }
            if (mi == nullptr || mi->get_mesh().is_null())
            {
                UtilityFunctions::push_warning("FGAnimator: screen_texts: no mesh '", node_name, "'");
                continue;
            }
            Node3D *parent = Object::cast_to<Node3D>(mi->get_parent());
            if (parent == nullptr)
                continue;
            const Ref<Mesh> mesh = mi->get_mesh();
            const AABB aabb = mesh->get_aabb();
            const Transform3D gt = mi->get_global_transform();
            // The screen normal from the first face; its sign from the model:
            // a Label3D draws its +z side only, so ez must point out of the
            // panel towards the crew. The winding is not reliable (the MD-80's
            // FMA quads face into the glareshield), the model's +x (aft) is;
            // a screen with no aft component at all keeps the face's direction.
            Vector3 ez;
            {
                int nax = 0;
                for (int a = 1; a < 3; a++)
                    if (aabb.size[a] < aabb.size[nax])
                        nax = a;
                ez[nax] = 1.0;
            }
            PackedVector3Array vs;
            const Array arrays = mesh->surface_get_arrays(0);
            if (arrays.size() > Mesh::ARRAY_INDEX)
            {
                vs = arrays[Mesh::ARRAY_VERTEX];
                const PackedInt32Array idx = arrays[Mesh::ARRAY_INDEX];
                if (idx.size() >= 3 && vs.size() >= 3)
                {
                    const Vector3 n = (vs[idx[1]] - vs[idx[0]]).cross(vs[idx[2]] - vs[idx[0]]);
                    if (n.length() > 1e-12)
                        ez = n.normalized();
                }
                else if (vs.size() >= 3)
                {
                    const Vector3 n = (vs[1] - vs[0]).cross(vs[2] - vs[0]);
                    if (n.length() > 1e-12)
                        ez = n.normalized();
                }
            }
            // The model's frame, not the world's: the scene is ECEF, so the
            // world's +y is nowhere near "up" over the aircraft.
            Node3D *root3d = Object::cast_to<Node3D>(model_root);
            const Basis root_basis = root3d != nullptr ? root3d->get_global_transform().basis : Basis();
            const Vector3 aft = root_basis.get_column(0).normalized();
            const Vector3 up = root_basis.get_column(1).normalized();
            if (gt.basis.xform(ez).dot(aft) < 0.0)
                ez = -ez;
            // Rows run along the model's up projected into the screen plane
            // (the mesh's own axes need not follow the screen's edges), the
            // baseline across; the screen's extent is measured along both.
            Vector3 ey = gt.basis.inverse().xform(up);
            ey = ey - ez * ey.dot(ez);
            if (ey.length() < 1e-6)
                ey = Vector3(0, 1, 0) - ez * ez.y;
            ey.normalize();
            const Vector3 ex = ey.cross(ez).normalized();
            const Basis lb_basis(ex, ey, ez);
            const Vector3 centre = aabb.get_center();
            double width = 0.0, height = 0.0;
            if (vs.size() >= 3)
            {
                double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
                for (int k = 0; k < vs.size(); k++)
                {
                    const Vector3 d = vs[k] - centre;
                    x0 = std::min(x0, (double)d.dot(ex));
                    x1 = std::max(x1, (double)d.dot(ex));
                    y0 = std::min(y0, (double)d.dot(ey));
                    y1 = std::max(y1, (double)d.dot(ey));
                }
                width = x1 - x0;
                height = y1 - y0;
            }
            else
            {
                width = aabb.get_longest_axis_size();
                height = aabb.get_shortest_axis_size();
            }
            const Array cells = sc.get("cells", Array());
            const int cols = std::max(1, (int)(int64_t)sc.get("columns", cells.size() > 0 ? (int64_t)cells.size() : 1));
            int rows = std::max(1, (int)(int64_t)sc.get("rows", 2));
            const Color color = Color::html(String(sc.get("color", "#ffb000")));
            const int power = String(sc.get("power", "")).is_empty() ? -1 : prop_id(sc.get("power", ""));
            const double char_m = num(sc.get("character-size", height / rows * 0.55), height / rows * 0.55);
            for (int ci = 0; ci < cells.size() && ci < cols; ci++)
            {
                const Array col = cells[ci];
                for (int ri = 0; ri < col.size() && ri < rows; ri++)
                {
                    Label3D *lb = memnew(Label3D);
                    lb->set_name(String("ScreenText_") + node_name + "_" + String::num_int64(ci) + "_" + String::num_int64(ri));
                    TextState ts;
                    ts.kind = 2;
                    ts.format = "%s";
                    ts.prop = prop_id(col[ri]);
                    ts.power_prop = power;
                    ts.power_min = num(sc.get("power_min", 24.0), 24.0);
                    lb->set_font_size(64);
                    lb->set_pixel_size(char_m / 64.0);
                    lb->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
                    lb->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
                    lb->set_modulate(color);
                    lb->set_draw_flag(Label3D::FLAG_SHADED, false);
                    lb->set_draw_flag(Label3D::FLAG_DOUBLE_SIDED, false);
                    lb->set_alpha_cut_mode(Label3D::ALPHA_CUT_DISCARD);
                    lb->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
                    // cell centre in the mesh frame, a hair in front of the face
                    const double cx = (ci + 0.5) / cols - 0.5;
                    const double cy = 0.5 - (ri + 0.5) / rows;
                    const Vector3 pos = centre + ex * (cx * width) + ey * (cy * height) + ez * 0.001;
                    lb->set_transform(mi->get_transform() * Transform3D(lb_basis, pos));
                    parent->add_child(lb);
                    ts.label_id = lb->get_instance_id();
                    text_states.push_back(ts);
                    made++;
                }
            }
        }
        if (made > 0)
            type_counts["screen-text"] = made;
        return made;
    }

    void FGAnimator::update_texts()
    {
        for (int i = 0; i < text_states.size(); i++)
        {
            TextState &ts = text_states.write[i];
            if (ts.kind == 0)
                continue;
            Label3D *lb = Object::cast_to<Label3D>(ObjectDB::get_instance(ObjectID(ts.label_id)));
            if (lb == nullptr)
                continue;
            if (ts.power_prop >= 0)
            {
                const bool on = prop_values[ts.power_prop] >= ts.power_min;
                if (lb->is_visible() != on)
                    lb->set_visible(on);
            }
            const double v = ts.prop >= 0 ? prop_values[ts.prop] : 0.0;
            // A screen window shows a string property as it is: an empty mode
            // string (or one never written) is an empty window, not "0".
            const bool verbatim = ts.kind == 2 && ts.prop >= 0 && (prop_is_string[ts.prop] || !prop_set[ts.prop]);
            const String str = ts.prop >= 0 ? (prop_strings[ts.prop].is_empty() && !verbatim ? String::num(v, 0) : prop_strings[ts.prop]) : String();
            const String out = format_text(ts.format, v, str, ts.kind);
            if (out != ts.last)
            {
                ts.last = out;
                lb->set_text(out);
            }
        }
    }

    // ── properties ─────────────────────────────────────────────────────

    void FGAnimator::set_property(const String &path, const Variant &value)
    {
        int id = prop_id(path);
        if (id < 0)
            return;
        prop_set.write[id] = true;
        prop_is_string.write[id] = value.get_type() == Variant::STRING;
        if (value.get_type() == Variant::STRING)
        {
            String s = value;
            prop_strings.write[id] = s;
            prop_values.write[id] = is_number(s.strip_edges()) ? s.to_float() : (s == "true" ? 1.0 : 0.0);
        }
        else if (value.get_type() == Variant::BOOL)
        {
            prop_values.write[id] = (bool)value ? 1.0 : 0.0;
            prop_strings.write[id] = (bool)value ? "true" : "false";
        }
        else
        {
            prop_values.write[id] = num(value, 0.0);
            prop_strings.write[id] = String();
        }
    }

    void FGAnimator::set_properties(const Dictionary &values)
    {
        Array keys = values.keys();
        for (int i = 0; i < keys.size(); i++)
            set_property(keys[i], values[keys[i]]);
    }

    Variant FGAnimator::get_property(const String &path) const
    {
        String p = normalise(path);
        if (!prop_index.has(p))
            return Variant();
        int id = prop_index[p];
        if (!prop_strings[id].is_empty())
            return prop_strings[id];
        return prop_values[id];
    }

    PackedStringArray FGAnimator::property_names() const
    {
        PackedStringArray out;
        for (int i = 0; i < prop_names.size(); i++)
            out.push_back(prop_names[i]);
        return out;
    }

    Dictionary FGAnimator::get_aliases() const
    {
        return aliases;
    }

    PackedStringArray FGAnimator::unknown_properties() const
    {
        PackedStringArray out;
        for (int i = 0; i < prop_names.size(); i++)
            if (!prop_set[i])
                out.push_back(prop_names[i]);
        return out;
    }

    // ── evaluation ─────────────────────────────────────────────────────

    double FGAnimator::eval_value(const Anim &a) const
    {
        double v = a.prop >= 0 ? prop_values[a.prop] : 0.0;
        if (a.table_in.size() > 0)
        {
            int n = a.table_in.size();
            if (v <= a.table_in[0])
                v = a.table_out[0];
            else if (v >= a.table_in[n - 1])
                v = a.table_out[n - 1];
            else
            {
                for (int i = 1; i < n; i++)
                    if (v <= a.table_in[i])
                    {
                        double t = (v - a.table_in[i - 1]) / Math::max(1e-12, a.table_in[i] - a.table_in[i - 1]);
                        v = a.table_out[i - 1] + t * (a.table_out[i] - a.table_out[i - 1]);
                        break;
                    }
            }
        }
        else
            v = a.offset + a.factor * v;
        if (a.has_min)
            v = Math::max(v, a.min_v);
        if (a.has_max)
            v = Math::min(v, a.max_v);
        return v;
    }

    void FGAnimator::apply_material(Anim &a, MatState &m, bool on)
    {
        for (int i = 0; i < m.mats.size(); i++)
        {
            BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(m.mats[i].ptr());
            if (bm == nullptr)
                continue;
            if (a.transparent && bm->get_transparency() == BaseMaterial3D::TRANSPARENCY_DISABLED)
            {
                // the texture's alpha decides (shadow planes, glass); drawn after the opaque panel
                bm->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
                bm->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
            }
            if (a.has_emission)
            {
                double f = 1.0;
                if (a.emission_is_prop && a.emission_factor_prop >= 0)
                    f = prop_values[a.emission_factor_prop];
                // FlightGear's colour x factor is an sRGB intent; BaseMaterial3D's
                // emission is a source_color the renderer linearises itself
                Color e = a.emission * (float)f;
                bool lit = on && (e.r + e.g + e.b) > 0.001;
                bm->set_feature(BaseMaterial3D::FEATURE_EMISSION, lit);
                if (lit)
                {
                    // FlightGear adds the emission flat; modulating it by the
                    // albedo texture keeps markings readable under panel light
                    Ref<Texture2D> albedo = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
                    if (albedo.is_valid() && bm->get_texture(BaseMaterial3D::TEXTURE_EMISSION).is_null())
                        bm->set_texture(BaseMaterial3D::TEXTURE_EMISSION, albedo);
                    // Godot's default operator ADDS the texture to the colour
                    // (a black dial came out solid orange, white markings
                    // white); FlightGear's fixed function multiplies
                    if (bm->get_texture(BaseMaterial3D::TEXTURE_EMISSION).is_valid())
                        bm->set_emission_operator(BaseMaterial3D::EMISSION_OP_MULTIPLY);
                    bm->set_emission(e);
                    bm->set_emission_energy_multiplier(1.0);
                }
            }
            if (a.has_diffuse && on)
                bm->set_albedo(Color(a.diffuse.r, a.diffuse.g, a.diffuse.b, bm->get_albedo().a));
            if (on && (a.alpha_prop >= 0 || a.has_alpha))
            {
                double al = a.alpha_prop >= 0 ? prop_values[a.alpha_prop] : a.alpha;
                Color c = bm->get_albedo();
                c.a = (float)Math::clamp(al, 0.0, 1.0);
                bm->set_albedo(c);
                bm->set_transparency(c.a < 0.999 ? BaseMaterial3D::TRANSPARENCY_ALPHA : BaseMaterial3D::TRANSPARENCY_DISABLED);
            }
        }
    }

    void FGAnimator::set_texture_root(const String &dir) { texture_root = dir; }
    String FGAnimator::get_texture_root() const { return texture_root; }

    Ref<Texture2D> FGAnimator::load_texture_file(const String &path)
    {
        if (textures.has(path))
            return textures[path];
        Ref<Texture2D> tex;
        Ref<Image> img = FileAccess::file_exists(path) ? Image::load_from_file(path) : Ref<Image>();
        if (img.is_valid() && !img->is_empty())
        {
            img->generate_mipmaps();
            tex = ImageTexture::create_from_image(img);
        }
        else
            UtilityFunctions::push_warning("FGAnimator: texture-prop file not found: ", path);
        textures[path] = tex;
        return tex;
    }

    // material <texture-prop>: the property's value names the albedo image of
    // the record's objects (a livery, an engine variant's gauge face); empty
    // or the <texture> the glb was built with restores the glb's own
    void FGAnimator::apply_texture_prop(Anim &a)
    {
        const String v = prop_strings[a.tex_prop].strip_edges();
        if (v == a.tex_last)
            return;
        a.tex_last = v;
        const bool restore = v.is_empty() || v == a.tex_default || texture_root.is_empty();
        Ref<Texture2D> tex;
        if (!restore)
        {
            const String rel = a.tex_dir.is_empty() ? v : a.tex_dir.path_join(v);
            tex = load_texture_file(texture_root.path_join(rel));
            if (tex.is_null())
                return; // keep what is shown
        }
        for (int t = 0; t < a.targets.size(); t++)
        {
            if (a.targets[t] < 0 || a.targets[t] >= mat_states.size())
                continue;
            MatState &m = mat_states.write[a.targets[t]];
            if (m.base_albedo.size() != m.mats.size())
            {
                m.base_albedo.clear();
                for (int i = 0; i < m.mats.size(); i++)
                {
                    BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(m.mats[i].ptr());
                    m.base_albedo.push_back(bm != nullptr ? bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO) : Ref<Texture2D>());
                }
            }
            for (int i = 0; i < m.mats.size(); i++)
            {
                BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(m.mats[i].ptr());
                if (bm == nullptr)
                    continue;
                Ref<Texture2D> nt = restore ? m.base_albedo[i] : tex;
                bm->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, nt);
                // the emission texture pairs the albedo (lit panels keep their markings)
                if (bm->get_texture(BaseMaterial3D::TEXTURE_EMISSION).is_valid())
                    bm->set_texture(BaseMaterial3D::TEXTURE_EMISSION, nt);
            }
        }
    }

    void FGAnimator::update(double delta)
    {
        // transform chains
        for (int i = 0; i < node_states.size(); i++)
        {
            NodeState &s = node_states.write[i];
            if (s.node == nullptr)
                continue;
            bool vis = true;
            for (int k = 0; k < s.selects.size(); k++)
                if (!eval(anims[s.selects[k]].cond))
                {
                    vis = false;
                    break;
                }
            if (s.node->is_visible() != vis)
                s.node->set_visible(vis);
            if (s.anims.is_empty())
                continue;
            Transform3D m;
            for (int k = 0; k < s.anims.size(); k++)
            {
                Anim &a = anims.write[s.anims[k]];
                if (!eval(a.cond))
                    continue;
                switch (a.type)
                {
                case A_ROTATE:
                {
                    double deg = eval_value(a);
                    Transform3D r(Basis(a.axis, Math::deg_to_rad(deg)), Vector3());
                    m = m * Transform3D(Basis(), a.center) * r * Transform3D(Basis(), -a.center);
                    break;
                }
                case A_SPIN:
                {
                    // factor x property = revolutions per minute
                    double rpm = (a.prop >= 0 ? prop_values[a.prop] : 0.0) * a.factor;
                    if (k == 0 || true)
                        a.angle = Math::fmod(a.angle + rpm * 6.0 * delta, 360.0);
                    Transform3D r(Basis(a.axis, Math::deg_to_rad(a.angle)), Vector3());
                    m = m * Transform3D(Basis(), a.center) * r * Transform3D(Basis(), -a.center);
                    break;
                }
                case A_TRANSLATE:
                {
                    double d = eval_value(a);
                    m = m * Transform3D(Basis(), a.axis * d);
                    break;
                }
                default:
                    break;
                }
            }
            s.applied = m;
            s.node->set_transform(s.parent_file_inv * m * s.parent_file * s.base);
        }
        // materials
        for (int i = 0; i < anims.size(); i++)
        {
            Anim &a = anims.write[i];
            if (a.type != A_MATERIAL && a.type != A_TEXTRANSLATE && a.type != A_TEXROTATE)
                continue;
            bool on = eval(a.cond);
            if (a.type == A_MATERIAL && a.tex_prop >= 0)
                apply_texture_prop(a);
            if (a.type == A_MATERIAL && on && !a.labels.is_empty() && (a.has_diffuse || a.has_emission))
            {
                // the digits' colour: FlightGear's diffuse (or emission) times its factor property
                Color c = a.has_diffuse ? a.diffuse : a.emission;
                const int fp = a.has_diffuse ? a.diffuse_factor_prop : (a.emission_is_prop ? a.emission_factor_prop : -1);
                if (fp >= 0)
                    c = c * (float)Math::clamp(prop_values[fp], 0.0, 1.0);
                // FlightGear's colour x dimmer factor is a display (sRGB) intent;
                // Label3D's modulate is an sRGB vertex colour (FLAG_SRGB_VERTEX_COLOR),
                // so the numbers go through as they are
                c.a = 1.0;
                for (int j = 0; j < a.labels.size(); j++)
                {
                    Label3D *lb = Object::cast_to<Label3D>(ObjectDB::get_instance(ObjectID(a.labels[j])));
                    if (lb != nullptr && lb->get_modulate() != c)
                        lb->set_modulate(c);
                }
            }
            for (int j = 0; j < a.targets.size(); j++)
            {
                MatState &ms = mat_states.write[a.targets[j]];
                if (a.type == A_MATERIAL)
                    apply_material(a, ms, on);
                else if (a.type == A_TEXROTATE)
                {
                    if (!on)
                        continue;
                    ensure_texrotate_materials(ms);
                    double v = a.prop >= 0 ? prop_values[a.prop] : 0.0;
                    if (a.table_in.size() > 0)
                    {
                        Anim tmp = a;
                        v = eval_value(tmp);
                    }
                    else
                        v = (v + a.offset) * a.factor;
                    for (int s = 0; s < ms.rot_mats.size(); s++)
                    {
                        ShaderMaterial *sm = Object::cast_to<ShaderMaterial>(ms.rot_mats[s].ptr());
                        if (sm != nullptr)
                            sm->set_shader_parameter("angle", Math::deg_to_rad(v));
                    }
                }
                else if (on)
                {
                    double v = a.prop >= 0 ? prop_values[a.prop] : 0.0;
                    if (a.table_in.size() > 0)
                    {
                        Anim tmp = a;
                        v = eval_value(tmp);
                    }
                    else
                    {
                        if (a.step > 0.0)
                        {
                            double base = Math::floor(v / a.step) * a.step;
                            double frac = v - base;
                            if (a.scroll > 0.0 && frac > a.step - a.scroll)
                                base += a.step * (frac - (a.step - a.scroll)) / a.scroll;
                            v = base;
                        }
                        // FlightGear's textranslate adds the offset BEFORE the
                        // factor: (value + offset) * factor. The drum counters
                        // rely on it (offset -100 with factor 0.001 = one digit
                        // back, so the drum's rest digit "1" reads "0").
                        v = (v + a.offset) * a.factor + a.bias;
                    }
                    // The glTF export flips V (top-left origin) against AC3D's
                    // bottom-left origin, so FlightGear's v-axis offset runs the
                    // other way in Godot's uv1_offset (drum counters).
                    Vector3 off(a.tex_axis.x * v, -a.tex_axis.y * v, 0.0);
                    for (int s = 0; s < ms.mats.size(); s++)
                    {
                        BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(ms.mats[s].ptr());
                        if (bm != nullptr)
                            bm->set_uv1_offset(off);
                    }
                }
            }
        }
        update_texts();
    }

    // texrotate: Godot's standard material cannot rotate UVs, so the surface
    // gets a small shader (albedo texture, alpha, unshaded) with the angle as
    // a parameter; the texture, colour and transparency come from the
    // material the glTF import made.
    void FGAnimator::ensure_texrotate_materials(MatState &m)
    {
        if (m.rot_mats.size() == m.mats.size() || m.mesh == nullptr)
            return;
        if (texrotate_shader.is_null())
        {
            texrotate_shader.instantiate();
            texrotate_shader->set_code(
                "shader_type spatial;\n"
                "render_mode cull_disabled, blend_mix, depth_draw_opaque;\n"
                "uniform sampler2D tex : source_color;\n"
                "uniform vec4 tint : source_color = vec4(1.0);\n"
                "uniform vec2 center = vec2(0.5, 0.5);\n"
                "uniform float angle = 0.0;\n"
                "void fragment() {\n"
                "  vec2 uv = UV - center;\n"
                "  float c = cos(angle); float s = sin(angle);\n"
                "  uv = vec2(c * uv.x - s * uv.y, s * uv.x + c * uv.y) + center;\n"
                "  vec4 col = texture(tex, uv) * tint;\n"
                "  ALBEDO = col.rgb; ALPHA = col.a;\n"
                "}\n");
        }
        m.rot_mats.clear();
        for (int s = 0; s < m.mats.size(); s++)
        {
            Ref<ShaderMaterial> sm;
            sm.instantiate();
            sm->set_shader(texrotate_shader);
            BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(m.mats[s].ptr());
            if (bm != nullptr)
            {
                Ref<Texture2D> tex = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
                if (tex.is_valid())
                    sm->set_shader_parameter("tex", tex);
                sm->set_shader_parameter("tint", bm->get_albedo());
            }
            m.mesh->set_surface_override_material(s, sm);
            m.rot_mats.push_back(sm);
        }
    }

    Dictionary FGAnimator::stats() const
    {
        Dictionary d;
        d["animations"] = anims.size();
        d["nodes"] = node_states.size();
        d["materials"] = mat_states.size();
        d["properties"] = prop_names.size();
        d["skipped"] = skipped;
        d["texts"] = text_states.size();
        d["types"] = type_counts;
        return d;
    }

    Transform3D FGAnimator::animated_transform(const String &node_name) const
    {
        for (int i = 0; i < node_states.size(); i++)
            if (node_states[i].node != nullptr && node_states[i].node->get_name() == StringName(node_name))
                return node_states[i].applied;
        return Transform3D();
    }
}
