#include "fg_animator.h"

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
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
        ClassDB::bind_method(D_METHOD("unknown_properties"), &FGAnimator::unknown_properties);
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
                if (bm == nullptr || !bm->get_feature(BaseMaterial3D::FEATURE_EMISSION))
                    continue;
                Ref<Texture2D> albedo = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
                Color e = bm->get_emission();
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
        for (int i = 0; i < node_states.size(); i++)
            if (node_states[i].node != nullptr && ObjectDB::get_instance(node_states[i].node->get_instance_id()) != nullptr)
            {
                node_states[i].node->set_transform(node_states[i].base);
                node_states[i].node->set_visible(true);
            }
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
        model_roots.clear();
        Array subs = m.get("submodels", Array());
        for (int i = 0; i < subs.size(); i++)
        {
            Dictionary sd = subs[i];
            String tag = sd.get("tag", "");
            String node = sd.get("node", "");
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
            else
            {
                skipped++;
                continue;
            }
            a.model = rec.get("model", "");
            // targets
            Array objs = rec.get("objects", Array());
            Vector<Node *> targets;
            for (int j = 0; j < objs.size(); j++)
            {
                String nm = objs[j];
                if (by_name.has(nm))
                {
                    const Vector<Node *> &v = by_name[nm];
                    for (int k = 0; k < v.size(); k++)
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
                a.axis = fg_vec(axis, "x", "y", "z", Vector3(1, 0, 0));
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
            case A_MATERIAL:
            {
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
            Node *frame_root = model_roots.has(a.model) ? model_roots[a.model] : file_root;
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
                if (a.type == A_MATERIAL || a.type == A_TEXTRANSLATE)
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
                                " materials, ", prop_names.size(), " properties, ", skipped, " records of other types skipped");
        return true;
    }

    // ── properties ─────────────────────────────────────────────────────

    void FGAnimator::set_property(const String &path, const Variant &value)
    {
        int id = prop_id(path);
        if (id < 0)
            return;
        prop_set.write[id] = true;
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
            if (a.has_emission)
            {
                double f = 1.0;
                if (a.emission_is_prop && a.emission_factor_prop >= 0)
                    f = prop_values[a.emission_factor_prop];
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
            if (a.type != A_MATERIAL && a.type != A_TEXTRANSLATE)
                continue;
            bool on = eval(a.cond);
            for (int j = 0; j < a.targets.size(); j++)
            {
                MatState &ms = mat_states.write[a.targets[j]];
                if (a.type == A_MATERIAL)
                    apply_material(a, ms, on);
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
    }

    Dictionary FGAnimator::stats() const
    {
        Dictionary d;
        d["animations"] = anims.size();
        d["nodes"] = node_states.size();
        d["materials"] = mat_states.size();
        d["properties"] = prop_names.size();
        d["skipped"] = skipped;
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
