// Volumetric cloud layer (design doc §10.2): one spherical shell between
// base_m and top_m above the ellipsoid, raymarched inside a camera-centred
// box mesh whose local frame is West/Up/North at the camera's ground point
// (so the planet centre is (0, -R, 0) in local space). Noise/erosion/lighting
// follow Clay John's volumetric-cloud demo (MIT); the shell geometry, the
// depth clip against the terrain and the fog are ours.
shader_type spatial;
render_mode blend_mix, depth_draw_never, cull_front, unshaded, fog_disabled;

uniform sampler3D shape_noise : filter_linear_mipmap, repeat_enable;
uniform sampler3D detail_noise : filter_linear_mipmap, repeat_enable;
uniform sampler2D depth_tex : hint_depth_texture;

uniform float base_m = 1500.0;
uniform float top_m = 2500.0;
uniform float coverage = 0.5;        // oktas / 8
uniform int cloud_type = 1;          // 1 cumulus, 2 stratus, 3 stratocumulus, 4 cumulonimbus
uniform float planet_radius = 6371000.0;
uniform vec3 sun_dir = vec3(0.0, 1.0, 0.0);   // local frame, toward the sun
uniform vec3 sun_color : source_color = vec3(1.0, 0.97, 0.92);
uniform float sun_energy = 1.0;
uniform vec3 ambient_color : source_color = vec3(0.55, 0.65, 0.85);
uniform float ambient_energy = 0.6;
uniform vec2 wind_offset = vec2(0.0);          // metres (animated drift)
// The node frame is re-anchored under the camera every frame, so noise must
// be sampled at a world-fixed position: world_offset = (-E, N) of the frame
// origin in LV95 metres (x = west, z = north), or the clouds follow the aircraft.
uniform vec2 world_offset = vec2(0.0);
uniform float shape_scale = 7000.0;            // metres per shape-noise period
uniform float detail_scale = 700.0;            // metres per detail period
uniform float macro_scale = 32000.0;           // metres per coverage-field period: cumuli come in clumps and gaps
uniform float sigma_ext = 0.02;                // extinction per metre at density 1
uniform float density_scale = 1.0;
uniform int steps = 40;
uniform float max_distance = 60000.0;
uniform float fog_density = 0.00005;
uniform vec3 fog_color : source_color = vec3(0.7, 0.75, 0.85);
uniform int debug_mode = 0;   // 1: tint where the ray crosses the shell, 2: tint every fragment
// Camera position and the mesh's offset, both in the layer node's frame
// (West/Up/North at the camera's ground point). Passed from GDScript in
// double precision: the shader's CAMERA_POSITION_WORLD / MODEL_MATRIX are
// camera-relative floats in the double build and cannot be mixed safely.
uniform vec3 cam_local = vec3(0.0);
uniform vec3 mesh_offset = vec3(0.0);

varying vec3 v_local;

// Returns (t_near, t_far) of ray/sphere, t_near <= t_far; (-1,-1) if none.
vec2 sphere_hits(vec3 ro, vec3 rd, vec3 c, float r) {
	vec3 oc = ro - c;
	float b = dot(oc, rd);
	float cc = dot(oc, oc) - r * r;
	float disc = b * b - cc;
	if (disc < 0.0) return vec2(-1.0);
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

float height_of(vec3 p, vec3 c) {
	return length(p - c) - planet_radius;
}

float remap(float v, float lo, float hi, float nlo, float nhi) {
	return nlo + (clamp(v, lo, hi) - lo) / max(hi - lo, 1e-5) * (nhi - nlo);
}

float height_profile(float hf) {
	// Vertical density shaping per cloud type (0 at base, 1 at top).
	if (cloud_type == 2) {          // stratus: flat, uniform
		return smoothstep(0.0, 0.08, hf) * (1.0 - smoothstep(0.85, 1.0, hf));
	} else if (cloud_type == 3) {   // stratocumulus
		return smoothstep(0.0, 0.1, hf) * (1.0 - smoothstep(0.6, 1.0, hf));
	} else if (cloud_type == 4) {   // cumulonimbus: tall, dense, wider top
		return smoothstep(0.0, 0.05, hf) * (1.0 - smoothstep(0.8, 1.0, hf));
	}
	return smoothstep(0.0, 0.08, hf) * (1.0 - smoothstep(0.4, 1.0, hf));   // cumulus: sharp flat base, domed top
}

// The coverage field: a low-frequency sample of the shape noise over the
// map, so cumuli gather in clumps with gaps between (a uniform threshold
// made one continuous blob at 5/8). Stratus stays uniform.
float local_coverage(vec3 q) {
	float cov = clamp(coverage, 0.0, 1.0);
	if (cloud_type == 2) return cov;
	float macro = texture(shape_noise, vec3(q.x, 3737.0, q.z) / macro_scale).r;
	// below the mean field the clouds thin out and vanish, above it they thicken
	return clamp(cov * remap(macro, 0.3, 0.75, 0.2, 1.6), 0.0, 1.0);
}

// Schneider / Horizon: perlin-worley base carved by the worley FBM, the
// height profile, the coverage remap (which also thins the cloud at low
// coverage), then edge erosion by the detail worley.
float density_at(vec3 p, vec3 c, bool with_detail) {
	float h = height_of(p, c);
	float hf = (h - base_m) / max(top_m - base_m, 1.0);
	if (hf <= 0.0 || hf >= 1.0) return 0.0;
	vec3 q = vec3(p.x + world_offset.x + wind_offset.x, h, p.z + world_offset.y + wind_offset.y);
	float cov = local_coverage(q);
	if (cov <= 0.005) return 0.0;
	// R is the equalised perlin-worley base (uniform 0..1), so the coverage
	// threshold below covers exactly its fraction of the volume.
	vec4 low = texture(shape_noise, q / shape_scale);
	float shape = low.r * height_profile(hf);
	// the threshold rises with height for the puffy types: domed tops over a flat base
	float lift = (cloud_type == 2) ? 0.0 : ((cloud_type == 4) ? 0.1 : 0.3);
	float d = remap(shape, (1.0 - cov) + hf * lift * (1.0 - cov), 1.0, 0.0, 1.0) * cov;
	if (d <= 0.0) return 0.0;
	if (with_detail) {
		vec3 hi = texture(detail_noise, q / detail_scale).rgb;
		float hfbm = hi.r * 0.625 + hi.g * 0.25 + hi.b * 0.125;
		// wispy near the base, billowy toward the top; the body keeps some of
		// the worley cells too (a smooth slab otherwise)
		float erode = mix(hfbm, 1.0 - hfbm, clamp(hf * 8.0, 0.0, 1.0));
		d = remap(d, erode * 0.5, 1.0, 0.0, 1.0);
		d *= 0.75 + 0.25 * hfbm;
	}
	return clamp(d, 0.0, 1.0) * density_scale;
}

// Optical depth toward the sun: six samples, the step growing so the march
// reaches through a thick layer (cone-like).
float light_depth(vec3 p, vec3 c) {
	float thick = top_m - base_m;
	float dl = thick * 0.08;
	float od = 0.0;
	for (int i = 0; i < 6; i++) {
		p += sun_dir * dl;
		od += density_at(p, c, i < 3) * dl;
		dl *= 1.5;
	}
	return od;
}

// how much of the base is open to the ground: thin where the light depth is small
float beer_floor(float ld) {
	return 0.25 + 0.75 * exp(-ld * 0.5);
}

float hg(float mu, float g) {
	return (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * mu, 1.5);   // x 4 pi: isotropic = 1
}

void vertex() {
	v_local = VERTEX + mesh_offset;
}

// Returns (rgb, alpha); alpha < 0 means "nothing here" (fragment discards).
vec4 march(vec3 ro, vec3 rd, vec2 screen_uv, mat4 inv_proj) {
	vec3 c = vec3(0.0, -planet_radius, 0.0);

	// Terrain / objects in front of the clouds limit the march.
	float dz = texture(depth_tex, screen_uv).x;
	float scene_dist = 1.0e12;
	if (dz > 0.0) {   // reversed-Z: 0 = far plane (sky)
		vec3 ndc = vec3(screen_uv * 2.0 - 1.0, dz);
		vec4 vp = inv_proj * vec4(ndc, 1.0);
		vp.xyz /= vp.w;
		scene_dist = length(vp.xyz);
	}

	float h0 = height_of(ro, c);
	vec2 hi = sphere_hits(ro, rd, c, planet_radius + base_m);
	vec2 ho = sphere_hits(ro, rd, c, planet_radius + top_m);
	float t0, t1;
	if (h0 < base_m) {                       // below the layer: enter at the inner shell
		if (hi.y <= 0.0) return vec4(0.0, 0.0, 0.0, -1.0);
		t0 = hi.y;
		t1 = ho.y;
	} else if (h0 <= top_m) {                // inside the layer
		t0 = 0.0;
		t1 = (hi.x > 0.0) ? hi.x : ho.y;
	} else {                                 // above: enter at the outer shell
		if (ho.x <= 0.0) return vec4(0.0, 0.0, 0.0, -1.0);
		t0 = ho.x;
		t1 = (hi.x > 0.0) ? hi.x : ho.y;
	}
	t1 = min(t1, min(scene_dist, max_distance));
	if (t1 <= t0) return vec4(0.0, 0.0, 0.0, -1.0);
	if (debug_mode == 1) return vec4(1.0, 0.0, 0.0, 0.4);

	// Step length: the segment split into `steps`, but never finer than a
	// tenth of the layer thickness — a ray straight through a 1 km layer
	// needs ~10 samples, not 32; oblique rays toward the horizon keep the
	// full count. The loop stops at t1 either way.
	// Step length: the segment split into `steps`, never finer than 1/40 of
	// the thickness, never coarser than a third of it (a grazing ray toward
	// the horizon spans tens of km: coarse steps there smeared every cloud
	// into horizontal streaks). The loop may run up to 3 x steps to reach t1.
	int n = max(steps, 8);
	float thick = top_m - base_m;
	float min_step = thick / 40.0;
	float ds = clamp((t1 - t0) / float(n), min_step, thick / 4.0);
	int n_max = n * 4;
	// Jitter the start to hide banding.
	// Interleaved gradient noise: finer-grained than a sin hash, less visible
	// as dither at close range.
	vec2 px = screen_uv * vec2(1920.0, 1080.0);
	float jitter = fract(52.9829189 * fract(0.06711056 * px.x + 0.00583715 * px.y));
	float t = t0 + ds * jitter;

	float sigma = sigma_ext;
	float mu = dot(rd, sun_dir);
	// two-lobe phase: strong forward peak (silver lining) + a little back scatter
	float phase = mix(hg(mu, 0.72), hg(mu, -0.2), 0.3);
	float trans = 1.0;
	vec3 col = vec3(0.0);
	float t_weight = 0.0;
	float t_sum = 0.0;
	int empty_run = 0;
	for (int i = 0; i < n_max; i++) {
		if (t > t1) break;
		vec3 p = ro + rd * t;
		// Cheap shape-only probe first; step twice as far through empty air
		// after a few misses, drop back to the fine step on a hit.
		float d = density_at(p, c, false);
		if (d <= 0.001) {
			empty_run++;
			t += (empty_run > 2) ? ds * 2.0 : ds;
			continue;
		}
		if (empty_run > 2) {
			// back up half a coarse step so the edge is not missed
			t -= ds;
			empty_run = 0;
			continue;
		}
		empty_run = 0;
		d = density_at(p, c, true);
		if (d > 0.001) {
			float od = d * sigma * ds;
			float a = 1.0 - exp(-od);
			float hf = (height_of(p, c) - base_m) / max(top_m - base_m, 1.0);
			// Beer-Lambert toward the sun with the powder term (Schneider):
			// dark creases where the light has not scattered yet, bright
			// sunlit domes. A floor keeps the undersides from going black:
			// multiple scattering the march does not model.
			float ld = (debug_mode == 3) ? 0.0 : light_depth(p, c) * sigma;
			float beer = exp(-ld);
			float powder = 1.0 - exp(-ld * 2.0);
			float lt = max(beer * mix(1.0, powder, 0.5) * 1.6, 0.06);
			vec3 sun = sun_color * sun_energy * lt * (0.45 + 0.55 * phase);
			// ambient: sky light from above, graded to the base, plus a little
			// ground bounce under the base; thin wisps let more through.
			float sky_amb = mix(0.4, 1.0, hf) * mix(1.0, 0.65, clamp(d, 0.0, 1.0));
			vec3 ground_bounce = vec3(0.07, 0.075, 0.07) * (1.0 - hf);
			vec3 amb = ambient_color * ambient_energy * sky_amb + sun_color * sun_energy * ground_bounce * beer_floor(ld);
			col += trans * a * (sun + amb);
			t_sum += trans * a * t;
			t_weight += trans * a;
			trans *= 1.0 - a;
			if (trans < 0.02) break;
		}
		t += ds;
	}
	float alpha = 1.0 - trans;
	if (alpha <= 0.002) return vec4(0.0, 0.0, 0.0, -1.0);
	vec3 rgb = col / alpha;
	// Distance haze toward the cloud's mean depth; fade out toward the box
	// edge so the layer has no hard far edge on the horizon.
	float tm = (t_weight > 0.0) ? t_sum / t_weight : t0;
	alpha *= 1.0 - smoothstep(max_distance * 0.7, max_distance, tm);
	float fog = 1.0 - exp(-fog_density * tm);
	rgb = mix(rgb, fog_color, fog);
	return vec4(rgb, alpha);
}

void fragment() {
	vec3 ro = cam_local;
	vec3 rd = normalize(v_local - ro);
	vec4 res = (debug_mode == 2) ? vec4(1.0, 0.0, 1.0, 0.4) : march(ro, rd, SCREEN_UV, INV_PROJECTION_MATRIX);
	if (res.a < 0.0) {
		discard;
	}
	ALBEDO = res.rgb;
	ALPHA = res.a;
}
