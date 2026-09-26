// One aircraft light as a point sprite (src/aircraft_lights.cpp). The single
// vertex sits at the lens centre of the model's light mesh; the sprite is
// sized in screen pixels by view distance so a strobe reads from a few km.
// src/aircraft_lights.cpp places the vertex just outside the lens; here it is
// pulled a few model-centimetres (pull_m, world metres) toward the camera
// against z-fighting with the lens. The depth test stays on, so the
// airframe occludes lights on its far side. Never pull further: a wingtip
// is thin and the light shows through it.
shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled;

uniform vec3 colour = vec3(1.0, 1.0, 1.0);
uniform float on = 1.0;          // 0..1 brightness of the fixture
uniform float size_scale = 1.0;  // relative size of this fixture class
uniform float size_px = 1200.0;  // pixel size at 1 m, divided by distance
uniform float pull_m = 0.01;     // set per fixture from the model's world scale

varying flat float vis;

void vertex() {
	vec4 v = MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	float dist = max(length(v.xyz), 0.5);
	v.xyz -= v.xyz / dist * min(pull_m, dist * 0.5);
	POSITION = PROJECTION_MATRIX * v;
	POINT_SIZE = clamp(size_px * size_scale / dist, 4.0, 18.0);
	vis = on;
}

void fragment() {
	vec2 p = POINT_COORD * 2.0 - 1.0;
	float r = length(p);
	float a = smoothstep(1.0, 0.3, r) * 0.7 + smoothstep(0.4, 0.0, r) * 0.7;
	float k = clamp(a * vis, 0.0, 1.0);
	ALBEDO = mix(colour, vec3(1.0), smoothstep(0.35, 0.0, r) * 0.6) * k;
	ALPHA = k;
	if (k < 0.01) {
		discard;
	}
}
