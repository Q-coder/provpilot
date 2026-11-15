// The sea (src/sea_surface.cpp, docs/SEA.md): one surface at 0 m on a
// camera-centred polar mesh, in the node's West / Up / North frame at the
// camera's ground point. The coast is cut into the terrain
// (scripts/build_world_sea.py), so the depth buffer shapes the shore: the
// water is clear over the first decimetres and takes its body colour with
// depth. Sky reflection by Fresnel, a sun glint, small ripples as normals.
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled;

uniform sampler2D depth_tex : hint_depth_texture, filter_nearest, repeat_disable;

uniform vec3 cam_local = vec3(0.0, 1000.0, 0.0);
uniform float planet_radius = 6371000.0;
uniform vec3 sun_dir = vec3(0.0, 1.0, 0.0);          // local frame, toward the sun
uniform vec3 sun_color : source_color = vec3(1.0, 0.97, 0.92);
uniform float sun_energy = 1.0;
uniform float sky_light = 1.0;                        // 0 night .. 1 day (twilight)
uniform vec3 sky_zenith : source_color = vec3(0.16, 0.34, 0.68);
uniform vec3 sky_horizon : source_color = vec3(0.62, 0.72, 0.86);
uniform vec3 deep_color : source_color = vec3(0.012, 0.070, 0.125);
uniform vec3 shallow_color : source_color = vec3(0.070, 0.260, 0.270);
uniform vec2 world_offset = vec2(0.0);                // ripple anchor: metres, wrapped by the host
uniform vec2 wind_offset = vec2(0.0);                 // ripple drift, metres
uniform float ripple = 1.0;                           // 0 calm .. 1.5 rough

varying vec3 v_local;

float hash(vec2 p) {
	p = fract(p * vec2(0.1031, 0.1030));
	p += dot(p, p.yx + 33.33);
	return fract((p.x + p.y) * p.x);
}

float noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// slope of a two-octave ripple field at p (metres)
vec2 ripple_slope(vec2 p, float scale) {
	float e = 0.35 * scale;
	float h0 = noise(p / scale) + 0.5 * noise(p * 2.3 / scale + 17.0);
	float hx = noise((p + vec2(e, 0.0)) / scale) + 0.5 * noise((p + vec2(e, 0.0)) * 2.3 / scale + 17.0);
	float hz = noise((p + vec2(0.0, e)) / scale) + 0.5 * noise((p + vec2(0.0, e)) * 2.3 / scale + 17.0);
	return vec2(hx - h0, hz - h0) / 0.35;
}

void vertex() {
	v_local = VERTEX;
}

void fragment() {
	vec3 to_cam = cam_local - v_local;
	float dist = max(length(to_cam), 0.1);
	vec3 V = to_cam / dist;
	vec3 up = normalize(v_local - vec3(0.0, -planet_radius, 0.0));

	// ripples: two scales, fading with distance before they alias
	vec2 wp = v_local.xz + world_offset + wind_offset;
	float near_w = exp(-dist / 900.0);
	float mid_w = exp(-dist / 9000.0);
	vec2 s = ripple_slope(wp, 7.0) * 0.10 * near_w + ripple_slope(wp * 0.13 + 31.0, 9.0) * 0.07 * mid_w;
	vec3 N = normalize(up + vec3(s.x, 0.0, s.y) * ripple);

	float nv = clamp(dot(N, V), 0.0, 1.0);
	float fres = 0.02 + 0.98 * pow(1.0 - nv, 5.0);
	// far water is a mirror of the horizon haze, seen ever flatter
	fres = mix(fres, max(fres, 0.55), smoothstep(8000.0, 60000.0, dist));

	vec3 R = reflect(-V, N);
	float ry = clamp(dot(R, up), 0.0, 1.0);
	vec3 sky = mix(sky_horizon, sky_zenith, pow(ry, 0.55)) * sky_light;
	// warm band toward a low sun
	float toward_sun = pow(max(dot(normalize(R - up * dot(R, up) + vec3(1e-5)), normalize(sun_dir - up * dot(sun_dir, up) + vec3(1e-5))), 0.0), 6.0);
	float low_sun = clamp(1.0 - abs(dot(sun_dir, up)) / 0.25, 0.0, 1.0);
	sky = mix(sky, vec3(1.0, 0.55, 0.32) * max(sky_light, 0.15), 0.5 * toward_sun * low_sun * (1.0 - ry));

	// sun glint: tight close in, a wide glitter path far out
	float rough = mix(900.0, 60.0, smoothstep(300.0, 20000.0, dist));
	float glint = pow(max(dot(R, normalize(sun_dir)), 0.0), rough) * (0.6 + 0.4 * rough / 900.0);
	vec3 sun_ref = sun_color * sun_energy * glint * 2.5 * step(0.0, dot(sun_dir, up));

	// water depth from the scene behind the surface
	float dz = texture(depth_tex, SCREEN_UV).x;
	float depth_m = 1.0e6;
	if (dz > 0.0) {   // reversed-Z: 0 = far plane
		vec4 vp = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, dz, 1.0);
		float scene = length(vp.xyz / vp.w);
		float mine = length(VERTEX);
		depth_m = max(scene - mine, 0.0) * max(nv, 0.08);
	}
	float body_light = sun_energy * clamp(dot(sun_dir, up), 0.0, 1.0) * 0.75 + sky_light * 0.25 + 0.012;
	vec3 body = mix(shallow_color, deep_color, 1.0 - exp(-depth_m / 5.0)) * body_light;

	vec3 col = mix(body, sky, fres) + sun_ref;
	// clear over the first decimetres: the shore has no hard edge
	float a = clamp(depth_m / 0.45, 0.0, 1.0);
	a = max(a * (0.82 + 0.18 * (1.0 - exp(-depth_m / 2.5))), fres * a);
	ALBEDO = col;
	ALPHA = a;
}
