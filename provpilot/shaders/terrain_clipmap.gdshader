shader_type spatial;
render_mode unshaded, cull_back, skip_vertex_transform, fog_disabled, depth_draw_opaque;

// Clipmap ring terrain shader with geomorphing.
//
// Each ring covers [inner_radius, outer_radius].  Near the outer boundary
// the vertex shader blends the fine heightmap toward a coarse heightmap
// (the next-outer ring's mip data) so both rings produce identical geometry
// at the seam.  The fragment shader performs the same blend on orthophoto
// color, then applies atmospheric haze + distance fog.
//
// fog_disabled prevents Godot's environment fog from double-fogging.

// ── Uniforms ───────────────────────────────────────────────────────────

// Fine textures (this ring's own mip level)
uniform sampler2D heightmap : filter_linear, repeat_disable;
uniform sampler2D color_texture : source_color, filter_linear, repeat_disable;

// Coarse textures (the next-outer ring's ACTUAL textures, shared directly).
// UV transform maps this ring's UV space into the coarse texture's UV space.
uniform sampler2D coarse_heightmap : filter_linear, repeat_disable;
uniform sampler2D coarse_color_texture : source_color, filter_linear, repeat_disable;
uniform bool has_coarse = false;
uniform float coarse_uv_scale : hint_range(0.0, 1.0) = 0.25;
uniform float coarse_uv_offset : hint_range(0.0, 1.0) = 0.375;

// Ring geometry
uniform float inner_radius : hint_range(0.0, 300000.0) = 8500.0;
uniform float outer_radius : hint_range(0.0, 400000.0) = 300000.0;

// Geomorph transition zone at the outer boundary.
// morph_start..outer_radius: heights blend fine→coarse.
uniform float morph_start : hint_range(0.0, 300000.0) = 14000.0;

// Atmosphere
uniform float fog_start : hint_range(0.0, 200000.0) = 80000.0;
uniform float fog_end : hint_range(0.0, 400000.0) = 250000.0;
uniform vec3 fog_color : source_color = vec3(0.75, 0.8, 0.88);
uniform vec3 haze_color : source_color = vec3(0.7, 0.75, 0.85);
uniform float haze_density : hint_range(0.0, 0.0001) = 0.000004;
uniform float brightness : hint_range(0.5, 2.0) = 1.1;

// Sun for basic diffuse lighting
uniform vec3 sun_direction = vec3(0.3, -0.8, -0.5);

// Phase 4 — Terrain3D ↔ Ring 0 seam blending.
// Ring 0 carries an extra heightmap built from the same 1 m tiles that
// Terrain3D uses (bilinear downsample instead of the mip2 median).
// Near the inner boundary the shader blends heights from the seam texture
// toward the mip2 data, adds a depth bias so T3D wins the depth test in
// the overlap zone, and fades the colour toward haze at the inner edge.
uniform sampler2D seam_heightmap : filter_linear, repeat_disable;
uniform bool has_seam = false;
uniform float seam_blend_start : hint_range(0.0, 20000.0) = 8500.0;
uniform float seam_blend_end   : hint_range(0.0, 20000.0) = 10500.0;
uniform float seam_depth_bias  : hint_range(0.0, 20.0) = 5.0;
uniform float inner_fade_end   : hint_range(0.0, 20000.0) = 9000.0;

// ── Varyings ───────────────────────────────────────────────────────────

varying float v_distance;
varying float v_morph;    // 0 = fully fine, 1 = fully coarse
varying vec2 v_uv;

// ── Vertex Shader ──────────────────────────────────────────────────────

void vertex() {
	vec4 world_pos = MODEL_MATRIX * vec4(VERTEX, 1.0);
	v_uv = UV;

	// Sample fine height (mip2 data for Ring 0)
	float fine_h = textureLod(heightmap, UV, 0.0).r;

	// Phase 4: Near the inner boundary, blend toward seam heightmap
	// (built from the same 1 m tiles Terrain3D uses).
	if (has_seam) {
		// Need distance BEFORE height displacement
		vec2 pre_delta = world_pos.xz - CAMERA_POSITION_WORLD.xz;
		float pre_dist = length(pre_delta);

		if (pre_dist < seam_blend_end) {
			float seam_h = textureLod(seam_heightmap, UV, 0.0).r;
			// 0 at seam_blend_start → 1 at seam_blend_end
			float seam_t = smoothstep(seam_blend_start, seam_blend_end, pre_dist);
			fine_h = mix(seam_h, fine_h, seam_t);
		}
	}

	// Horizontal distance from camera (before height displacement)
	vec2 delta_xz = world_pos.xz - CAMERA_POSITION_WORLD.xz;
	v_distance = length(delta_xz);

	// Geomorph factor: 0 inside morph_start, 1 at outer_radius
	v_morph = (has_coarse && morph_start < outer_radius)
		? clamp((v_distance - morph_start) / (outer_radius - morph_start), 0.0, 1.0)
		: 0.0;

	// Blend height when geomorphing
	float final_h = fine_h;
	if (v_morph > 0.0) {
		vec2 coarse_uv = UV * coarse_uv_scale + coarse_uv_offset;
		float coarse_h = textureLod(coarse_heightmap, coarse_uv, 0.0).r;
		final_h = mix(fine_h, coarse_h, v_morph);
	}
	world_pos.y = final_h;

	// Phase 4: Depth bias — lower Ring 0 near the inner boundary so
	// Terrain3D always wins the depth test in the overlap zone.
	if (has_seam) {
		float bias_t = 1.0 - smoothstep(inner_radius, seam_blend_end, v_distance);
		world_pos.y -= seam_depth_bias * bias_t;
	}

	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * world_pos;
}

// ── Fragment Shader ────────────────────────────────────────────────────

void fragment() {
	// Hard discard outside the ring's valid radial zone
	if (v_distance < inner_radius || v_distance > outer_radius) {
		discard;
	}

	// Orthophoto color — cross-fade fine → coarse to match height geomorphing
	vec3 terrain_color = texture(color_texture, v_uv).rgb;
	if (v_morph > 0.0) {
		vec2 coarse_uv = v_uv * coarse_uv_scale + coarse_uv_offset;
		vec3 coarse_color = texture(coarse_color_texture, coarse_uv).rgb;
		terrain_color = mix(terrain_color, coarse_color, v_morph);
	}

	// Compute approximate normal from heightmap via central differences
	vec2 texel = vec2(1.0) / vec2(textureSize(heightmap, 0));
	float hL = textureLod(heightmap, v_uv + vec2(-texel.x, 0.0), 0.0).r;
	float hR = textureLod(heightmap, v_uv + vec2( texel.x, 0.0), 0.0).r;
	float hD = textureLod(heightmap, v_uv + vec2(0.0, -texel.y), 0.0).r;
	float hU = textureLod(heightmap, v_uv + vec2(0.0,  texel.y), 0.0).r;
	// texel_world_size depends on how many metres each texel covers;
	// we approximate using the distance between samples (2 texels).
	// For a 2048-texel texture covering 2*half_extent, each texel is pixel_res metres.
	// We'll let the uniform-driven outer_radius serve as a proxy:
	//   half_extent ≈ outer_radius (close enough for normal scale).
	// For correct scale we'd pass pixel_res as a uniform but a rough normal
	// is fine for diffuse shading at these distances.
	float pixel_world = outer_radius / 1024.0;   // approximate metres per texel
	vec3 normal = normalize(vec3(hL - hR, 2.0 * pixel_world, hD - hU));

	// Basic diffuse lighting
	vec3 sun_dir = normalize(-sun_direction);
	float ndotl = max(dot(normal, sun_dir), 0.0);
	float diffuse = 0.45 + 0.55 * ndotl;   // ambient 0.45, diffuse 0.55
	terrain_color *= brightness * diffuse;

	// Phase 4: Fade terrain colour toward haze at the inner edge so the
	// T3D boundary is invisible.  At inner_radius the fragment is pure
	// haze; by inner_fade_end it is full terrain colour.
	if (has_seam) {
		float inner_fade = smoothstep(inner_radius, inner_fade_end, v_distance);
		terrain_color = mix(haze_color * brightness, terrain_color, inner_fade);
	}

	ALBEDO = terrain_color;
}
