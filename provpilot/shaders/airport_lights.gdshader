// Airfield lights as point sprites (design §12.6). One mesh per airport,
// PRIMITIVE_POINTS, one vertex per fixture:
//   CUSTOM0 = colour A rgb + size scale       (A: seen from the +axis side)
//   CUSTOM1 = colour B rgb + mode             (B: seen from the -axis side)
//   CUSTOM2 = axis x, axis z (model space), papi angle deg, directivity
// mode: 0 = two-way colour by axis side, 1 = directed toward -axis (approach
// lights: fade when seen from behind), 2 = PAPI (white above papi angle,
// red below, seen from the approach side). `intensity` 0..1 comes from the
// host (dusk, or visibility below 1 km).
shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled;

uniform float intensity = 0.0;
uniform float size_px = 4500.0;   // pixel size at 1 m; scaled by 1/distance

varying flat vec3 col;
varying flat float vis;

void vertex() {
	// Camera position in model space from the modelview matrix: in the
	// double-precision build the model matrix is camera-relative, so
	// MODEL_MATRIX and CAMERA_POSITION_WORLD must not be mixed.
	vec3 cam_model = (inverse(MODELVIEW_MATRIX) * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	vec3 to_cam = cam_model - VERTEX;
	float dist = max(length(to_cam), 1.0);
	vec3 dir = to_cam / dist;
	vec2 axis = CUSTOM2.xy;
	int mode = int(CUSTOM2.w + 0.5);
	float side = dot(normalize(vec2(dir.x, dir.z) + vec2(1e-6, 0.0)), axis);
	col = side >= 0.0 ? CUSTOM0.rgb : CUSTOM1.rgb;
	vis = 1.0;
	if (mode == 1) {
		// directed toward -axis: full when seen from that side, dim behind
		vis = smoothstep(-0.15, 0.35, -side);
	} else if (mode == 2) {
		float angle = degrees(asin(clamp(dir.y, -1.0, 1.0)));
		col = angle > CUSTOM2.z ? vec3(1.0, 0.97, 0.9) : vec3(1.0, 0.12, 0.05);
		vis = smoothstep(-0.15, 0.35, -side);
	}
	float sz = size_px * CUSTOM0.a / dist;
	// PAPI / VASI are lit day and night (gery 2026-11-01): the night factor
	// only dims the runway, approach and threshold lights
	float lit = (mode == 2) ? 1.0 : intensity;
	POINT_SIZE = clamp(sz, 3.0, 12.0) * (0.6 + 0.4 * lit);
	// black colour B = fixture unlit from that side
	if (dot(col, col) < 1e-4) vis = 0.0;
	vis *= lit;
}

void fragment() {
	vec2 p = POINT_COORD * 2.0 - 1.0;
	float r = length(p);
	float a = smoothstep(1.0, 0.35, r);
	// a hot core keeps small distant sprites readable
	a = a * 0.75 + smoothstep(0.45, 0.0, r) * 0.6;
	ALBEDO = col * clamp(a * vis, 0.0, 1.0);
	ALPHA = clamp(a * vis, 0.0, 1.0);
	if (ALPHA < 0.01) {
		discard;
	}
}
