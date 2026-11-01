#include "fg_property_rules.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/xml_parser.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

static constexpr double FGPR_PI = 3.14159265358979323846;

namespace jsbgodot
{

void FGPropertyRules::_bind_methods()
{
	ClassDB::bind_method(D_METHOD("load", "xml_path"), &FGPropertyRules::load);
	ClassDB::bind_method(D_METHOD("clear"), &FGPropertyRules::clear);
	ClassDB::bind_method(D_METHOD("apply", "props", "dt"), &FGPropertyRules::apply);
	ClassDB::bind_method(D_METHOD("input_names"), &FGPropertyRules::input_names);
	ClassDB::bind_method(D_METHOD("output_names"), &FGPropertyRules::output_names);
	ClassDB::bind_method(D_METHOD("unsupported"), &FGPropertyRules::unsupported);
	ClassDB::bind_method(D_METHOD("get_component_count"), &FGPropertyRules::get_component_count);
	ClassDB::bind_method(D_METHOD("get_value", "prop"), &FGPropertyRules::get_value);
}

FGPropertyRules::FGPropertyRules() {}

const FGPropertyRules::XNode *FGPropertyRules::XNode::child(const char *n) const
{
	for (const XNode &k : kids)
	{
		if (k.name == n)
		{
			return &k;
		}
	}
	return nullptr;
}

bool FGPropertyRules::XNode::has(const char *n) const { return child(n) != nullptr; }

String FGPropertyRules::norm(const String &p)
{
	String s = p.strip_edges();
	while (s.begins_with("/"))
	{
		s = s.substr(1);
	}
	return s.replace("[0]", "");
}

bool FGPropertyRules::is_number(const String &s)
{
	return !s.is_empty() && s.is_valid_float();
}

double FGPropertyRules::num(const Variant &v, double def)
{
	switch (v.get_type())
	{
		case Variant::FLOAT:
		case Variant::INT:
			return (double)v;
		case Variant::BOOL:
			return (bool)v ? 1.0 : 0.0;
		case Variant::STRING:
		{
			const String s = String(v).strip_edges();
			if (is_number(s))
			{
				return s.to_float();
			}
			return s == "true" ? 1.0 : def;
		}
		default:
			return def;
	}
}

// The file as a tree: element name, its own text (stripped, text pieces
// joined), children in order. Comments and whitespace-only text are dropped;
// stray text next to child elements (a typo like "</table>t") is kept on
// the parent and ignored by the evaluators.
bool FGPropertyRules::parse_xml(const String &path, XNode &root)
{
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(path);
	if (bytes.is_empty())
	{
		return false;
	}
	Ref<XMLParser> xp;
	xp.instantiate();
	if (xp->open_buffer(bytes) != OK)
	{
		return false;
	}
	root = XNode();
	root.name = "#document";
	std::vector<XNode *> stack;
	stack.push_back(&root);
	while (xp->read() == OK)
	{
		switch (xp->get_node_type())
		{
			case XMLParser::NODE_ELEMENT:
			{
				XNode n;
				n.name = xp->get_node_name();
				const bool empty = xp->is_empty();
				stack.back()->kids.push_back(n);
				if (!empty)
				{
					stack.push_back(&stack.back()->kids.back());
				}
				break;
			}
			case XMLParser::NODE_ELEMENT_END:
				if (stack.size() > 1)
				{
					stack.pop_back();
				}
				break;
			case XMLParser::NODE_TEXT:
			case XMLParser::NODE_CDATA:
			{
				const String t = xp->get_node_data().strip_edges();
				if (!t.is_empty())
				{
					XNode *top = stack.back();
					top->text = top->text.is_empty() ? t : top->text + " " + t;
				}
				break;
			}
			default:
				break;
		}
	}
	// pointers into a vector stay valid only while no sibling is appended
	// after a child was taken: the stack discipline above guarantees it
	// (a child is pushed, filled, then popped before its parent gets the next).
	return !root.kids.empty();
}

void FGPropertyRules::collect_names(const XNode &n, bool in_output)
{
	if (n.name == "property" && !n.text.is_empty())
	{
		(in_output ? output_set : input_set)[norm(n.text)] = true;
	}
	else if ((n.name == "input" || n.name == "scale" || n.name == "offset" || n.name == "min" || n.name == "max" || n.name == "gain") &&
			n.kids.empty() && !n.text.is_empty() && !is_number(n.text))
	{
		input_set[norm(n.text)] = true;
	}
	else if (n.name == "output" && n.kids.empty() && !n.text.is_empty())
	{
		output_set[norm(n.text)] = true;
	}
	const bool out = in_output || n.name == "output";
	for (const XNode &k : n.kids)
	{
		collect_names(k, out);
	}
}

int FGPropertyRules::load(const String &xml_path)
{
	XNode doc;
	if (!parse_xml(xml_path, doc))
	{
		return -1;
	}
	const XNode *list = doc.child("PropertyList");
	if (list == nullptr)
	{
		return -1;
	}
	int n = 0;
	for (const XNode &k : list->kids)
	{
		if (k.name == "params" || k.name == "property" || k.name == "name")
		{
			continue;
		}
		Component c;
		c.kind = k.name;
		c.node = k;
		if (k.name == "filter")
		{
			const XNode *t = k.child("type");
			c.type = t != nullptr ? t->text : String("gain");
			static const char *const known[] = {"gain", "exponential", "double-exponential", "moving-average", "noise-spike",
					"reciprocal", "derivative", "integrator"};
			bool ok = false;
			for (const char *kn : known)
			{
				ok = ok || c.type == kn;
			}
			if (!ok)
			{
				const String tag = String("filter/") + c.type;
				if (!unsupported_.has(tag))
				{
					unsupported_.push_back(tag);
				}
				continue;
			}
		}
		else if (k.name != "logic")
		{
			if (!unsupported_.has(k.name))
			{
				unsupported_.push_back(k.name);
			}
			continue;
		}
		for (const XNode &o : k.kids)
		{
			if (o.name != "output")
			{
				continue;
			}
			if (o.kids.empty())
			{
				if (!o.text.is_empty())
				{
					c.outputs.push_back(norm(o.text));
				}
			}
			else
			{
				for (const XNode &p : o.kids)
				{
					if (p.name == "property" && !p.text.is_empty())
					{
						c.outputs.push_back(norm(p.text));
					}
				}
			}
		}
		collect_names(k, false);
		comps.push_back(c);
		n++;
	}
	return n;
}

void FGPropertyRules::clear()
{
	comps.clear();
	outputs.clear();
	input_set.clear();
	output_set.clear();
	unsupported_.clear();
}

PackedStringArray FGPropertyRules::input_names() const
{
	PackedStringArray out;
	const Array keys = input_set.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		out.push_back(keys[i]);
	}
	return out;
}

PackedStringArray FGPropertyRules::output_names() const
{
	PackedStringArray out;
	const Array keys = output_set.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		out.push_back(keys[i]);
	}
	return out;
}

PackedStringArray FGPropertyRules::unsupported() const { return unsupported_; }
int FGPropertyRules::get_component_count() const { return (int)comps.size(); }

double FGPropertyRules::get_value(const String &prop) const
{
	const String k = norm(prop);
	return outputs.has(k) ? num(outputs[k]) : 0.0;
}

Variant FGPropertyRules::prop_raw(const String &path) const
{
	const String k = norm(path);
	if (cur.has(k))
	{
		return cur[k];
	}
	if (outputs.has(k))
	{
		return outputs[k];
	}
	return Variant();
}

double FGPropertyRules::prop(const String &path) const { return num(prop_raw(path)); }

// SimGear expression: one element, its children the operands.
double FGPropertyRules::expr(const XNode &n) const
{
	const String &t = n.name;
	auto arg = [&](int i) -> double {
		int c = 0;
		for (const XNode &k : n.kids)
		{
			if (c++ == i)
			{
				return expr(k);
			}
		}
		return 0.0;
	};
	const int nk = (int)n.kids.size();
	if (t == "value")
	{
		return num(n.text);
	}
	if (t == "property")
	{
		return prop(n.text);
	}
	if (t == "expression")
	{
		return nk > 0 ? expr(n.kids.front()) : num(n.text);
	}
	if (t == "table")
	{
		// the argument is the first child that is not an entry
		const XNode *a = nullptr;
		for (const XNode &k : n.kids)
		{
			if (k.name != "entry")
			{
				a = &k;
				break;
			}
		}
		const double x = a != nullptr ? expr(*a) : 0.0;
		double pi = 0.0, pd = 0.0;
		bool first = true;
		for (const XNode &k : n.kids)
		{
			if (k.name != "entry")
			{
				continue;
			}
			const XNode *ci = k.child("ind");
			const XNode *cd = k.child("dep");
			const double xi = ci != nullptr ? num(ci->text) : 0.0;
			const double yi = cd != nullptr ? num(cd->text) : 0.0;
			if (first)
			{
				if (x <= xi)
				{
					return yi;
				}
				first = false;
			}
			else if (x <= xi)
			{
				return xi - pi > 1e-12 ? pd + (yi - pd) * (x - pi) / (xi - pi) : yi;
			}
			pi = xi;
			pd = yi;
		}
		return pd;
	}
	if (t == "sum")
	{
		double s = 0.0;
		for (const XNode &k : n.kids) s += expr(k);
		return s;
	}
	if (t == "product" || t == "prod")
	{
		double p = 1.0;
		for (const XNode &k : n.kids) p *= expr(k);
		return p;
	}
	if (t == "difference" || t == "dif")
	{
		double d = nk > 0 ? arg(0) : 0.0;
		for (int i = 1; i < nk; i++) d -= arg(i);
		return d;
	}
	if (t == "quotient" || t == "div")
	{
		double q = nk > 0 ? arg(0) : 0.0;
		for (int i = 1; i < nk; i++)
		{
			const double d = arg(i);
			q = std::abs(d) > 1e-12 ? q / d : 0.0;
		}
		return q;
	}
	if (t == "min")
	{
		double m = INFINITY;
		for (const XNode &k : n.kids) m = std::min(m, expr(k));
		return nk > 0 ? m : 0.0;
	}
	if (t == "max")
	{
		double m = -INFINITY;
		for (const XNode &k : n.kids) m = std::max(m, expr(k));
		return nk > 0 ? m : 0.0;
	}
	if (t == "clip")
	{
		const XNode *lo = n.child("clipMin");
		const XNode *hi = n.child("clipMax");
		double v = 0.0;
		for (const XNode &k : n.kids)
		{
			if (k.name != "clipMin" && k.name != "clipMax")
			{
				v = expr(k);
				break;
			}
		}
		if (lo != nullptr) v = std::max(v, expr(*lo));
		if (hi != nullptr) v = std::min(v, expr(*hi));
		return v;
	}
	if (t == "abs" || t == "fabs") return std::abs(arg(0));
	if (t == "sqr") { const double v = arg(0); return v * v; }
	if (t == "sqrt") return std::sqrt(std::max(arg(0), 0.0));
	if (t == "exp") return std::exp(arg(0));
	if (t == "log") return std::log(std::max(arg(0), 1e-300));
	if (t == "log10") return std::log10(std::max(arg(0), 1e-300));
	if (t == "sin") return std::sin(arg(0));
	if (t == "cos") return std::cos(arg(0));
	if (t == "tan") return std::tan(arg(0));
	if (t == "asin") return std::asin(std::clamp(arg(0), -1.0, 1.0));
	if (t == "acos") return std::acos(std::clamp(arg(0), -1.0, 1.0));
	if (t == "atan") return std::atan(arg(0));
	if (t == "atan2") return std::atan2(arg(0), arg(1));
	if (t == "pow") return std::pow(arg(0), arg(1));
	if (t == "mod" || t == "fmod") { const double d = arg(1); return std::abs(d) > 1e-12 ? std::fmod(arg(0), d) : 0.0; }
	if (t == "floor") return std::floor(arg(0));
	if (t == "ceil") return std::ceil(arg(0));
	if (t == "rad2deg") return arg(0) * 180.0 / FGPR_PI;
	if (t == "deg2rad") return arg(0) * FGPR_PI / 180.0;
	if (t == "less") return arg(0) < arg(1) ? 1.0 : 0.0;
	if (t == "less-equal") return arg(0) <= arg(1) ? 1.0 : 0.0;
	if (t == "greater") return arg(0) > arg(1) ? 1.0 : 0.0;
	if (t == "greater-equal") return arg(0) >= arg(1) ? 1.0 : 0.0;
	if (t == "equal") return std::abs(arg(0) - arg(1)) < 1e-9 ? 1.0 : 0.0;
	if (t == "nequal") return std::abs(arg(0) - arg(1)) >= 1e-9 ? 1.0 : 0.0;
	if (t == "and") { for (const XNode &k : n.kids) if (expr(k) == 0.0) return 0.0; return 1.0; }
	if (t == "or") { for (const XNode &k : n.kids) if (expr(k) != 0.0) return 1.0; return 0.0; }
	if (t == "not") return arg(0) == 0.0 ? 1.0 : 0.0;
	if (t == "if") return arg(0) != 0.0 ? arg(1) : arg(2);
	// a bare number as the whole expression
	if (nk == 0 && is_number(n.text))
	{
		return n.text.to_float();
	}
	return 0.0;
}

// SimGear condition: a container's children are ANDed.
bool FGPropertyRules::cond_children(const XNode &n) const
{
	for (const XNode &k : n.kids)
	{
		if (!cond(k))
		{
			return false;
		}
	}
	return true;
}

bool FGPropertyRules::compare(const XNode &n) const
{
	const XNode *a = nullptr;
	const XNode *b = nullptr;
	for (const XNode &k : n.kids)
	{
		if (k.name != "property" && k.name != "value" && k.name != "expression")
		{
			continue;
		}
		if (a == nullptr) a = &k;
		else if (b == nullptr) b = &k;
	}
	if (a == nullptr)
	{
		return false;
	}
	const String op = n.name;
	// a non-numeric <value>: string comparison against the property's text
	if (b != nullptr && b->name == "value" && !is_number(b->text))
	{
		const Variant raw = a->name == "property" ? prop_raw(a->text) : Variant(expr(*a));
		const String sv = raw.get_type() == Variant::NIL ? String() : String(raw);
		if (op == "equals") return sv == b->text;
		if (op == "not-equals") return sv != b->text;
		return false;
	}
	const double x = a->name == "property" ? prop(a->text) : expr(*a);
	const double y = b == nullptr ? 0.0 : (b->name == "property" ? prop(b->text) : (b->name == "value" ? num(b->text) : expr(*b)));
	if (op == "equals") return std::abs(x - y) < 1e-9;
	if (op == "not-equals") return std::abs(x - y) >= 1e-9;
	if (op == "less-than") return x < y;
	if (op == "less-than-equals") return x <= y;
	if (op == "greater-than") return x > y;
	if (op == "greater-than-equals") return x >= y;
	return false;
}

bool FGPropertyRules::cond(const XNode &n) const
{
	const String &t = n.name;
	if (t == "and" || t == "condition" || t == "input" || t == "enable")
	{
		return cond_children(n);
	}
	if (t == "or")
	{
		for (const XNode &k : n.kids)
		{
			if (cond(k))
			{
				return true;
			}
		}
		return false;
	}
	if (t == "not")
	{
		return !cond_children(n);
	}
	if (t == "property")
	{
		return prop(n.text) != 0.0;
	}
	if (t == "value")
	{
		return num(n.text) != 0.0;
	}
	if (t == "true")
	{
		return true;
	}
	if (t == "false")
	{
		return false;
	}
	if (t == "equals" || t == "not-equals" || t == "less-than" || t == "less-than-equals" || t == "greater-than" || t == "greater-than-equals")
	{
		return compare(n);
	}
	return true;
}

// FlightGear InputValue: a number or property as text, or <property> /
// <value> / <expression> with <scale> <offset> <min> <max> <abs>.
double FGPropertyRules::input_value(const XNode &n) const
{
	double v = 0.0;
	if (n.kids.empty())
	{
		return is_number(n.text) ? n.text.to_float() : (n.text.is_empty() ? 0.0 : prop(n.text));
	}
	if (const XNode *e = n.child("expression"))
	{
		v = expr(*e);
	}
	else if (const XNode *p = n.child("property"))
	{
		v = prop(p->text);
	}
	else if (const XNode *c = n.child("value"))
	{
		v = num(c->text);
	}
	else if (!n.text.is_empty())
	{
		v = is_number(n.text) ? n.text.to_float() : prop(n.text);
	}
	if (const XNode *s = n.child("scale")) v *= input_value(*s);
	if (const XNode *o = n.child("offset")) v += input_value(*o);
	if (const XNode *lo = n.child("min")) v = std::max(v, input_value(*lo));
	if (const XNode *hi = n.child("max")) v = std::min(v, input_value(*hi));
	if (const XNode *a = n.child("abs"))
	{
		if (a->text.is_empty() || a->text == "true" || a->text == "1")
		{
			v = std::abs(v);
		}
	}
	return v;
}

// The first <name> child whose <condition> holds; def when none.
double FGPropertyRules::input_list(const XNode &parent, const char *name, double def, bool *found) const
{
	if (found != nullptr)
	{
		*found = false;
	}
	for (const XNode &k : parent.kids)
	{
		if (k.name != name)
		{
			continue;
		}
		const XNode *c = k.child("condition");
		if (c != nullptr && !cond(*c))
		{
			continue;
		}
		if (found != nullptr)
		{
			*found = true;
		}
		return input_value(k);
	}
	return def;
}

bool FGPropertyRules::enabled(const XNode &n) const
{
	const XNode *e = n.child("enable");
	if (e == nullptr)
	{
		return true;
	}
	if (const XNode *c = e->child("condition"))
	{
		return cond(*c);
	}
	if (const XNode *p = e->child("prop"))
	{
		const XNode *v = e->child("value");
		const double want = v != nullptr ? num(v->text) : 1.0;
		return std::abs(prop(p->text) - want) < 1e-9;
	}
	return cond_children(*e);
}

void FGPropertyRules::run(Component &c, double dt)
{
	if (!enabled(c.node))
	{
		return;
	}
	double out = c.out;
	if (c.kind == "logic")
	{
		const XNode *in = c.node.child("input");
		bool r = in != nullptr ? cond(*in) : false;
		const XNode *inv = c.node.child("inverted");
		if (inv != nullptr && (inv->text == "true" || inv->text == "1"))
		{
			r = !r;
		}
		out = r ? 1.0 : 0.0;
	}
	else
	{
		const double in = input_list(c.node, "input", 0.0);
		const double gain = input_list(c.node, "gain", 1.0);
		const String &t = c.type;
		if (t == "gain")
		{
			out = gain * in;
		}
		else if (t == "reciprocal")
		{
			out = std::abs(in) > 1e-12 ? gain / in : 0.0;
		}
		else if (t == "exponential" || t == "double-exponential")
		{
			const double tf = input_list(c.node, "filter-time", 1.0);
			const double alpha = dt > 0.0 ? 1.0 / ((tf / dt) + 1.0) : 1.0;
			if (!c.has_out)
			{
				out = in;
				c.prev_in = in;
			}
			else if (t == "exponential")
			{
				out = alpha * in + (1.0 - alpha) * c.out;
			}
			else
			{
				c.prev_in = alpha * in + (1.0 - alpha) * c.prev_in;
				out = alpha * c.prev_in + (1.0 - alpha) * c.out;
			}
		}
		else if (t == "moving-average")
		{
			const int samples = std::max(1, (int)input_list(c.node, "samples", 1.0));
			c.samples.push_back(in);
			while ((int)c.samples.size() > samples)
			{
				c.samples.pop_front();
			}
			double s = 0.0;
			for (double v : c.samples) s += v;
			out = s / (double)c.samples.size();
		}
		else if (t == "noise-spike")
		{
			const double rate = input_list(c.node, "max-rate-of-change", 1.0);
			if (!c.has_out)
			{
				out = in;
			}
			else
			{
				const double step = rate * dt;
				const double d = in - c.out;
				out = std::abs(d) > step ? c.out + (d > 0.0 ? step : -step) : in;
			}
		}
		else if (t == "derivative")
		{
			const double tf = input_list(c.node, "filter-time", 1.0);
			out = c.prev_valid && dt > 0.0 ? (in - c.prev_in) / dt * tf : 0.0;
			c.prev_in = in;
			c.prev_valid = true;
		}
		else if (t == "integrator")
		{
			out = (c.has_out ? c.out : 0.0) + in * gain * dt;
		}
		bool f = false;
		const double lo = input_list(c.node, "u_min", 0.0, &f);
		if (f) out = std::max(out, lo);
		const double hi = input_list(c.node, "u_max", 0.0, &f);
		if (f) out = std::min(out, hi);
	}
	c.out = out;
	c.has_out = true;
	for (int i = 0; i < c.outputs.size(); i++)
	{
		cur[c.outputs[i]] = out;
		outputs[c.outputs[i]] = out;
	}
}

void FGPropertyRules::apply(const Dictionary &props, double dt)
{
	cur = props;
	// last frame's outputs for rules that read a later rule
	const Array keys = outputs.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		if (!cur.has(keys[i]))
		{
			cur[keys[i]] = outputs[keys[i]];
		}
	}
	for (Component &c : comps)
	{
		run(c, dt);
	}
	cur = Dictionary();
}

} // namespace jsbgodot
