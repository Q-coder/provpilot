shader_type sky;

// Physically-inspired single-scattering atmosphere for ProVPilot.
//
// Renders a curved planet limb from orbit and a blue daytime sky at ground
// level, so the horizon matches the world3d globe's Earth curvature at every
// altitude (fixes the flat procedural-sky horizon that showed above ~90 km).
// The sun_direction / sun_disk_energy uniforms are updated every frame by
// sky_atmosphere.gd from the simulated time of day, so day/night and the sun
// disk track the DirectionalLight3D that casts the scene shadows.
//
// EYEDIR and POSITION are world-space in ProVPilot's flat local frame
// (+X = West, +Y = Up, +Z = North). The globe's centre sits at planet_center
// (0, -planet_radius, 0) in that frame, so the camera's distance from the
// centre — hence its altitude — follows POSITION.y near the origin.

uniform vec3 sun_direction = vec3(0.0, 1.0, 0.0);        // world dir TO the sun
uniform float sun_disk_energy : hint_range(0.0, 1.0) = 1.0; // 0 when the sun is below the horizon
uniform vec3 planet_center = vec3(0.0, -6371000.0, 0.0);
uniform float planet_radius = 6371000.0;
uniform float atmosphere_radius = 6471000.0;
uniform vec3 rayleigh_beta = vec3(5.8e-6, 13.5e-6, 33.1e-6);
uniform float mie_beta = 3.0e-6;
uniform float rayleigh_scale = 8000.0;
uniform float mie_scale = 1200.0;
uniform float mie_g : hint_range(-0.99, 0.99) = 0.76;
uniform float sun_intensity = 22.0;
uniform vec3 night_sky_color : source_color = vec3(0.004, 0.006, 0.013);
uniform float star_intensity : hint_range(0.0, 4.0) = 1.0;
uniform float exposure : hint_range(0.1, 4.0) = 1.0;
// Dim ground hemisphere so downward view directions (and the sky-derived
// ambient probe) aren't pitch black — models light bouncing off the ground.
// Without this, surfaces whose normals face below the horizon receive no
// ambient at all and render black.
uniform vec3 ground_color : source_color = vec3(0.17, 0.18, 0.15);
uniform float ground_strength : hint_range(0.0, 1.0) = 1.0;

const int PRIMARY_STEPS = 16;
const int LIGHT_STEPS = 8;

// Ray/sphere intersection (sphere centred at origin). Returns (near, far) t
// distances, or a degenerate (big, -big) pair on a miss.
vec2 ray_sphere(vec3 ro, vec3 rd, float radius) {
	float b = dot(ro, rd);
	float c = dot(ro, ro) - radius * radius;
	float d = b * b - c;
	if (d < 0.0) {
		return vec2(1e12, -1e12);
	}
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}

// In-scattered sky colour along a view ray (ro/rd relative to the planet
// centre). Stops at the planet surface so the horizon band gets aerial
// perspective; samples in the planet's shadow contribute nothing (night side).
vec3 compute_atmosphere(vec3 ro, vec3 rd, vec3 sun_dir) {
	vec2 atm = ray_sphere(ro, rd, atmosphere_radius);
	if (atm.x > atm.y) {
		return vec3(0.0); // ray never enters the atmosphere shell (deep space)
	}
	atm.x = max(atm.x, 0.0);
	vec2 grd = ray_sphere(ro, rd, planet_radius);
	if (grd.x <= grd.y && grd.x > 0.0) {
		atm.y = min(atm.y, grd.x);
	}

	float ray_len = (atm.y - atm.x) / float(PRIMARY_STEPS);
	float t = atm.x;

	vec3 total_rlh = vec3(0.0);
	vec3 total_mie = vec3(0.0);
	float od_rlh = 0.0;
	float od_mie = 0.0;

	float mu = dot(rd, sun_dir);
	float g2 = mie_g * mie_g;
	float phase_rlh = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
	float phase_mie = 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + mu * mu)) /
		(pow(1.0 + g2 - 2.0 * mu * mie_g, 1.5) * (2.0 + g2));

	for (int i = 0; i < PRIMARY_STEPS; i++) {
		vec3 pos = ro + rd * (t + ray_len * 0.5);
		float h = length(pos) - planet_radius;
		float d_rlh = exp(-h / rayleigh_scale) * ray_len;
		float d_mie = exp(-h / mie_scale) * ray_len;
		od_rlh += d_rlh;
		od_mie += d_mie;
		// Attenuate to the sample's MIDPOINT. Using the running total charges
		// each sample its own whole step, and towards the horizon one step
		// spans ~70 km — a blue optical depth near 2 — so the daytime horizon
		// came out sunset-yellow at every hour.
		float view_od_rlh = od_rlh - d_rlh * 0.5;
		float view_od_mie = od_mie - d_mie * 0.5;

		// Skip samples the planet occludes from the sun (Earth's own shadow).
		vec2 lg = ray_sphere(pos, sun_dir, planet_radius);
		bool in_shadow = (lg.x <= lg.y && lg.y > 0.0);

		if (!in_shadow) {
			vec2 ls = ray_sphere(pos, sun_dir, atmosphere_radius);
			float l_len = ls.y / float(LIGHT_STEPS);
			float lt = 0.0;
			float lod_rlh = 0.0;
			float lod_mie = 0.0;
			for (int j = 0; j < LIGHT_STEPS; j++) {
				vec3 lpos = pos + sun_dir * (lt + l_len * 0.5);
				float lh = length(lpos) - planet_radius;
				lod_rlh += exp(-lh / rayleigh_scale) * l_len;
				lod_mie += exp(-lh / mie_scale) * l_len;
				lt += l_len;
			}
			vec3 att = exp(-(mie_beta * (view_od_mie + lod_mie) + rayleigh_beta * (view_od_rlh + lod_rlh)));
			total_rlh += d_rlh * att;
			total_mie += d_mie * att;
		}
		t += ray_len;
	}
	return sun_intensity * (phase_rlh * rayleigh_beta * total_rlh + phase_mie * mie_beta * total_mie);
}

float hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

// Sparse procedural stars, only meaningful where the sky is dark.
float stars(vec3 dir) {
	vec3 p = dir * 260.0;
	vec3 id = floor(p);
	float h = hash13(id);
	if (h < 0.994) {
		return 0.0;
	}
	vec3 f = fract(p) - 0.5;
	float d = length(f);
	return smoothstep(0.32, 0.0, d) * (h - 0.994) / 0.006;
}

void sky() {
	vec3 ro = POSITION - planet_center;
	vec3 rd = normalize(EYEDIR);
	vec3 sun_dir = normalize(sun_direction);

	vec3 col = compute_atmosphere(ro, rd, sun_dir);

	// Lower hemisphere: a dim, sun-lit ground colour instead of black, so
	// down-facing normals still receive ambient (the sky-derived ambient probe
	// samples this too). Uses the geodetic up so it stays correct from orbit.
	vec3 up_dir = normalize(ro);
	float below = clamp(-dot(rd, up_dir), 0.0, 1.0);
	float sun_elev = clamp(dot(sun_dir, up_dir), 0.0, 1.0);
	vec3 grnd = ground_color * (0.12 + 0.88 * sun_elev);
	col = mix(col, grnd, below * ground_strength);

	// Night-side floor + stars fade in where the scattered light is dark.
	float bright = clamp(max(col.r, max(col.g, col.b)) * 6.0, 0.0, 1.0);
	col += night_sky_color * (1.0 - bright);
	col += vec3(stars(rd) * star_intensity * (1.0 - bright));

	// Sun disk, hidden once the sun drops below the horizon.
	float sd = dot(rd, sun_dir);
	float disk = smoothstep(0.9995, 0.99975, sd);
	col += vec3(1.0, 0.96, 0.9) * disk * sun_disk_energy * 40.0;

	COLOR = col * exposure;
}
