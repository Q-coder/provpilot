#ifndef FG_PROPERTY_RULES_H
#define FG_PROPERTY_RULES_H

#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <deque>
#include <vector>

using namespace godot;

namespace jsbgodot
{

    // FlightGear property rules (docs/MD80_FLIGHTGEAR_PORT.md, step 12): the
    // <PropertyList> files an aircraft lists under <systems><autopilot><path>
    // (the "XML autopilot" format), run on the bridge's property dictionary
    // every frame. Supported components:
    //   <logic>   output(s) = condition of <input> (optionally <inverted>)
    //   <filter>  <type> gain | exponential | double-exponential |
    //             moving-average | noise-spike | reciprocal | derivative |
    //             integrator; <input> list (first whose <condition> holds;
    //             a number, a property, or <property>/<value>/<expression>
    //             with <scale> <offset> <min> <max> <abs>), <gain>,
    //             <filter-time>, <samples>, <max-rate-of-change>, <u_min>,
    //             <u_max>, <enable>
    // with SimGear's expression (<sum> <product> <difference> <quotient>
    // <table> <min> <max> <clip> <pow> <abs> <sqrt> ... ) and condition
    // (<and> <or> <not> <equals> <less-than> ... <property>) grammars.
    // Components of other kinds (<pid-controller>, <flipflop> ...) are
    // listed in unsupported() and skipped. Property paths are normalised
    // like the animator's (no leading slash, no [0]); a property the
    // dictionary lacks reads 0. Outputs are written into the dictionary and
    // remembered, so a rule may read a later rule's previous value.
    class FGPropertyRules : public RefCounted
    {
        GDCLASS(FGPropertyRules, RefCounted);

    public:
        static void _bind_methods();
        FGPropertyRules();

        // Adds the file's components; returns how many were loaded from it (-1: unreadable).
        int load(const String &xml_path);
        void clear();
        // Runs every component on props (in place) with the frame time.
        void apply(const Dictionary &props, double dt);
        // Property paths the rules read / write (normalised).
        PackedStringArray input_names() const;
        PackedStringArray output_names() const;
        // Component kinds / filter types met in the files that are not run.
        PackedStringArray unsupported() const;
        int get_component_count() const;
        // Last value a rule wrote to this property (0 when none did).
        double get_value(const String &prop) const;
        static String norm(const String &p);

    protected:
        struct XNode
        {
            String name;
            String text;
            std::vector<XNode> kids;
            const XNode *child(const char *n) const;
            bool has(const char *n) const;
        };
        struct Component
        {
            String kind;               // logic | filter
            String type;               // filter type
            XNode node;
            PackedStringArray outputs; // normalised
            double out = 0.0;
            bool has_out = false;
            double prev_in = 0.0;
            bool prev_valid = false;
            std::deque<double> samples;
        };
        std::vector<Component> comps;
        Dictionary outputs;             // normalised path -> last value
        Dictionary input_set, output_set;
        PackedStringArray unsupported_;
        Dictionary cur;                 // the dictionary of the running apply()

        static bool parse_xml(const String &path, XNode &root);
        void collect_names(const XNode &n, bool in_output);
        static double num(const Variant &v, double def = 0.0);
        static bool is_number(const String &s);
        double prop(const String &path) const;
        Variant prop_raw(const String &path) const;
        double expr(const XNode &n) const;
        bool cond(const XNode &n) const;
        bool cond_children(const XNode &n) const;
        bool compare(const XNode &n) const;
        double input_value(const XNode &n) const;
        double input_list(const XNode &parent, const char *name, double def, bool *found = nullptr) const;
        bool enabled(const XNode &n) const;
        void run(Component &c, double dt);
    };

} // namespace jsbgodot

#endif
