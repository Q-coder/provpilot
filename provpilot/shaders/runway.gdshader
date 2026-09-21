// Procedural runway surface and markings (design §12, D17).
// The mesh UV carries metres: UV.x = across (signed, + = right of the
// end0→end1 direction), UV.y = along from end0. Everything is drawn in
// metres so one shader serves every width and length.
shader_type spatial;
render_mode cull_disabled, depth_draw_opaque;

// Per-surface parameters ride in the vertex attributes so ONE material
// serves every runway, taxiway and apron in the scene (see runways.gd):
//   CUSTOM0 = (half_width, length_m, displaced0, displaced1)
//   CUSTOM1 = (glyphs0 packed base 14, glyphs1 packed, kind*64 + bars*2 + aiming, taxiway on_runway flag)
// kind: 0 asphalt, 1 concrete, 2 grass, 3 gravel/dirt, 4 taxiway, 5 apron, 6 taxiway line;
// glyphs: digit 0-9, L=10, R=11, C=12, none=-1 (stored as code+1).
varying vec2 rw;
varying flat vec4 prm0;
varying flat vec4 prm1;

// 5×7 bitmap font, one int per row (bit 4 = leftmost column), 13 glyphs.
const int FONT[91] = {
	14, 17, 19, 21, 25, 17, 14,   // 0
	 4, 12,  4,  4,  4,  4, 14,   // 1
	14, 17,  1,  2,  4,  8, 31,   // 2
	31,  2,  4,  2,  1, 17, 14,   // 3
	 2,  6, 10, 18, 31,  2,  2,   // 4
	31, 16, 30,  1,  1, 17, 14,   // 5
	 6,  8, 16, 30, 17, 17, 14,   // 6
	31,  1,  2,  4,  8,  8,  8,   // 7
	14, 17, 17, 14, 17, 17, 14,   // 8
	14, 17, 17, 15,  1,  2, 12,   // 9
	16, 16, 16, 16, 16, 16, 31,   // L
	30, 17, 17, 30, 20, 18, 17,   // R
	14, 17, 16, 16, 16, 17, 14    // C
};

float hash2(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float vnoise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = hash2(i), b = hash2(i + vec2(1.0, 0.0));
	float c = hash2(i + vec2(0.0, 1.0)), d = hash2(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// 1 inside a glyph pixel. u: 0..6 m across the glyph, v: 0..9 m up the glyph.
float glyph(int code, float u, float v) {
	if (code < 0 || u < 0.0 || u >= 6.0 || v < 0.0 || v >= 9.0) return 0.0;
	int col = int(floor(u / 6.0 * 5.0));
	int row = 6 - int(floor(v / 9.0 * 7.0));     // row 0 = top of the glyph
	int bits = FONT[code * 7 + row];
	return ((bits >> (4 - col)) & 1) == 1 ? 1.0 : 0.0;
}

// Markings for one runway end. t: metres inward from the threshold (0 at
// the threshold, growing toward the far end); xr: across, + = pilot's right.
float end_markings(float t, float xr, ivec3 g, float half_width, int bars, bool aiming_bars) {
	float m = 0.0;
	// Threshold stripes: 30 m long from 6 m past the threshold.
	if (t >= 6.0 && t <= 36.0) {
		float inner = half_width - 3.0;
		if (abs(xr) <= inner) {
			float pitch = 2.0 * inner / float(bars);
			float s = mod(xr + inner, pitch);
			if (s <= 1.8) m = 1.0;
		}
	}
	// Designator: glyphs 6 m wide, 9 m tall, 1.5 m apart, centred, 12 m
	// past the stripes, readable when landing over this threshold.
	int n = (g.x >= 0 ? 1 : 0) + (g.y >= 0 ? 1 : 0) + (g.z >= 0 ? 1 : 0);
	if (n > 0 && t >= 48.0 && t <= 57.0) {
		float total = float(n) * 6.0 + float(n - 1) * 1.5;
		float u = xr + total * 0.5;
		float v = t - 48.0;
		int codes[3] = {g.x, g.y, g.z};
		for (int i = 0; i < 3; i++) {
			if (codes[i] < 0) continue;
			m = max(m, glyph(codes[i], u, v));
			u -= 7.5;
		}
	}
	// Aiming point bars: 45 m long at 300 m, 6 m wide, 22 m apart.
	if (aiming_bars && t >= 300.0 && t <= 345.0 && abs(xr) >= 8.0 && abs(xr) <= 14.0) {
		m = 1.0;
	}
	// Displaced threshold: a bar across at the threshold itself.
	if (t >= -1.8 && t <= 0.0) m = 1.0;
	return m;
}

void vertex() {
	rw = UV;
	prm0 = CUSTOM0;
	prm1 = CUSTOM1;
}

ivec3 unpack_glyphs(float packed) {
	int v = int(packed + 0.5);
	return ivec3(v / 196 - 1, (v / 14) % 14 - 1, v % 14 - 1);
}

void fragment() {
	float x = rw.x;
	float along = rw.y;
	float half_width = prm0.x;
	float length_m = prm0.y;
	float displaced0 = prm0.z;
	float displaced1 = prm0.w;
	int code = int(prm1.z + 0.5);
	int surface_kind = code / 64;
	int bars = (code % 64) / 2;
	bool aiming_bars = (code % 2) == 1;
	ivec3 glyphs0 = unpack_glyphs(prm1.x);
	ivec3 glyphs1 = unpack_glyphs(prm1.y);
	vec3 base;
	float rough = 0.9;
	float n1 = vnoise(vec2(x, along) * 0.35);
	float n2 = vnoise(vec2(x, along) * 2.1);
	if (surface_kind == 0) {            // asphalt
		base = vec3(0.30, 0.30, 0.31) * (0.85 + 0.3 * n1) * (0.9 + 0.2 * n2);
	} else if (surface_kind == 1) {     // concrete
		base = vec3(0.56, 0.55, 0.52) * (0.9 + 0.2 * n1);
		// slab joints every 6 m
		if (mod(along, 6.0) < 0.12 || mod(x + half_width, 6.0) < 0.12) base *= 0.75;
	} else if (surface_kind == 2) {     // grass
		base = mix(vec3(0.33, 0.42, 0.20), vec3(0.42, 0.46, 0.24), n1) * (0.9 + 0.2 * n2);
		// mowing stripes
		if (mod(floor(x / 6.0), 2.0) == 0.0) base *= 0.93;
		rough = 1.0;
	} else {                             // gravel / dirt
		base = mix(vec3(0.50, 0.45, 0.37), vec3(0.58, 0.53, 0.44), n1) * (0.9 + 0.2 * n2);
		rough = 1.0;
	}

	float mark = 0.0;
	vec3 mark_col = vec3(0.92, 0.92, 0.88);
	if (surface_kind == 4) {             // taxiway pavement (the line is kind 6)
		base = vec3(0.32, 0.32, 0.33) * (0.85 + 0.3 * n1) * (0.9 + 0.2 * n2);
		// On a runway pavement (bake flag in CUSTOM1.w, exact at the
		// pavement edge) the taxiway pavement is not drawn at all.
		if (prm1.w > 0.5) {
			discard;
		}
	} else if (surface_kind == 6) {      // taxiway guidance line strip
		base = vec3(0.85, 0.72, 0.15);
		rough = 0.8;
	} else if (surface_kind == 5) {      // apron: concrete slabs, no markings
		base = vec3(0.52, 0.51, 0.49) * (0.9 + 0.2 * n1);
		if (mod(x, 7.5) < 0.12 || mod(along, 7.5) < 0.12) base *= 0.78;
		rough = 0.85;
	} else if (surface_kind <= 1) {
		// Edge stripes.
		float ax = abs(x);
		if (ax >= half_width - 1.5 && ax <= half_width - 0.6) mark = 1.0;
		// Centreline: 30 m dashes, 20 m gaps, 0.9 m wide, clear of the end zones.
		float t0 = along - displaced0;
		float t1 = (length_m - along) - displaced1;
		if (ax <= 0.45 && t0 > 60.0 && t1 > 60.0 && mod(along, 50.0) < 30.0) mark = 1.0;
		mark = max(mark, end_markings(t0, x, glyphs0, half_width, bars, aiming_bars));
		mark = max(mark, end_markings(t1, -x, glyphs1, half_width, bars, aiming_bars));
		// Inside a longer runway's pavement (bake flag) this runway's
		// markings stop; the through runway's markings continue.
		if (prm1.w > 0.5) mark = 0.0;
		// Pavement before a displaced threshold: darker, no markings drawn
		// beyond the threshold bar.
	}
	vec3 col = mix(base, mark_col, mark * 0.9);
	ALBEDO = col;
	ROUGHNESS = rough;
	METALLIC = 0.0;
	SPECULAR = 0.2;
}
