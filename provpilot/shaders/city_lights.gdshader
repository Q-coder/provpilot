// Town and city lights as point sprites (src/city_lights.cpp): one mesh per
// 1024 m square, PRIMITIVE_POINTS, COLOR = lamp colour rgb + size scale.
// `intensity` 0..1 is the host's night factor; the haze dims a lamp with its
// distance, and a lamp smaller than a pixel keeps its light by fading
// instead of flickering.
shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled, fog_disabled;

uniform float intensity = 0.0;
uniform float size_px = 7000.0;      // pixel size at 1 m on a 1080-line screen; scaled by 1/distance
uniform float energy = 1.7;          // over 1: the glow pass blooms the lamps
uniform float fog_density = 0.00005;
uniform float fade_start = 38000.0;
uniform float fade_end = 45000.0;
uniform float near_radius = 9000.0;   // inside: the near level (UV.x 0), outside: the far level (UV.x 1)

varying flat vec3 col;
varying flat float vis;

void vertex() {
	// camera in model space from the modelview matrix (double build: the
	// model matrix is camera-relative, CAMERA_POSITION_WORLD is not)
	vec3 cam_model = (inverse(MODELVIEW_MATRIX) * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	float dist = max(length(cam_model - VERTEX), 1.0);
	// sprite sizes are in pixels: scale them with the screen (a 4K frame
	// would show half-size lamps)
	float px = VIEWPORT_SIZE.y / 1080.0;
	float sz = size_px * COLOR.a / dist;
	POINT_SIZE = clamp(sz, 1.8, 6.5) * px;
	col = COLOR.rgb;
	vis = intensity * exp(-fog_density * dist * 0.7) * (1.0 - smoothstep(fade_start, fade_end, dist));
	float far_w = smoothstep(near_radius * 0.8, near_radius, dist);
	vis *= UV.x > 0.5 ? far_w : 1.0 - far_w;
	// below the smallest sprite the lamp's light spreads over the pixel
	vis *= clamp(sz / 1.8, 0.4, 1.0);
}

void fragment() {
	vec2 p = POINT_COORD * 2.0 - 1.0;
	float r = length(p);
	float a = smoothstep(1.0, 0.3, r);
	a = a * 0.7 + smoothstep(0.5, 0.0, r) * 0.6;
	float o = clamp(a * vis, 0.0, 1.0);
	ALBEDO = col * o * energy;
	ALPHA = o;
	if (o < 0.004) {
		discard;
	}
}
