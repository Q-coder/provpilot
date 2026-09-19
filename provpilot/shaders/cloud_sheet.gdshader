// Thin high cloud layer (altostratus / altocumulus / cirrus): a textured
// sheet at base_m following the planet's curvature, alpha from 2D noise.
// Local frame = West/Up/North at the camera's ground point.
shader_type spatial;
render_mode blend_mix, depth_draw_never, cull_disabled, unshaded, fog_disabled;

uniform sampler2D sheet_noise : filter_linear_mipmap, repeat_enable;
uniform float base_m = 6000.0;
uniform float coverage = 0.5;
uniform int cloud_type = 7;                // 5 altostratus, 6 altocumulus, 7 cirrus
uniform float planet_radius = 6371000.0;
uniform vec3 sun_dir = vec3(0.0, 1.0, 0.0);
uniform vec3 sun_color : source_color = vec3(1.0, 0.97, 0.92);
uniform float sun_energy = 1.0;
uniform vec3 ambient_color : source_color = vec3(0.55, 0.65, 0.85);
uniform float ambient_energy = 0.6;
uniform vec2 wind_offset = vec2(0.0);
uniform vec2 world_offset = vec2(0.0);   // (-E, N) of the frame origin, see cloud_layer
uniform float noise_scale = 20000.0;
uniform float fog_density = 0.00005;
uniform vec3 fog_color : source_color = vec3(0.7, 0.75, 0.85);
uniform vec3 cam_local = vec3(0.0);   // camera in the layer node's frame (from GDScript)

varying vec3 v_local;
varying float v_dist;

void vertex() {
	// Drop the sheet with the curvature so it stays at base_m above the
	// ellipsoid instead of floating away toward the horizon.
	float r2 = VERTEX.x * VERTEX.x + VERTEX.z * VERTEX.z;
	VERTEX.y = base_m - r2 / (2.0 * (planet_radius + base_m));
	v_local = VERTEX;
	v_dist = length(VERTEX - cam_local);
}

void fragment() {
	vec2 uv = (v_local.xz + world_offset + wind_offset) / noise_scale;
	float n1 = texture(sheet_noise, uv).r;
	float n2 = texture(sheet_noise, uv * 3.7 + vec2(0.31, 0.77)).r;
	float n = n1 * 0.7 + n2 * 0.3;
	float cov = clamp(coverage, 0.0, 1.0);
	float a;
	if (cloud_type == 5) {           // altostratus: smooth veil
		a = smoothstep(1.0 - cov, 1.0 - cov * 0.5 + 0.05, n) * 0.7;
	} else if (cloud_type == 6) {    // altocumulus: broken patches
		float n3 = texture(sheet_noise, uv * 9.0 + vec2(0.5, 0.2)).r;
		a = smoothstep(1.0 - cov, 1.0 - cov + 0.15, n) * smoothstep(0.35, 0.65, n3) * 0.75;
	} else {                         // cirrus: streaky, translucent
		float s = texture(sheet_noise, vec2(uv.x * 0.5, uv.y * 4.0)).r;
		a = smoothstep(1.0 - cov, 1.0, n * 0.6 + s * 0.4) * 0.35;
	}
	if (a <= 0.003) discard;
	// Fade at grazing angles / far distance to hide the sheet edge.
	a *= 1.0 - smoothstep(60000.0, 90000.0, v_dist);
	float lit = 0.6 + 0.4 * clamp(sun_dir.y, 0.0, 1.0);
	vec3 rgb = sun_color * sun_energy * lit * 0.9 + ambient_color * ambient_energy * 0.6;
	float fog = 1.0 - exp(-fog_density * v_dist);
	rgb = mix(rgb, fog_color, fog);
	ALBEDO = rgb;
	ALPHA = a;
}
