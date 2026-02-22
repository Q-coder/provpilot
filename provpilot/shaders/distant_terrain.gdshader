shader_type spatial;
render_mode unshaded, cull_back, skip_vertex_transform, fog_disabled;

// Distant terrain shader — renders low-res terrain with elevation coloring
// and atmospheric haze/fog for realistic distant scenery.
// Fragments closer than near_clip are discarded to avoid overlap with Terrain3D.
// fog_disabled prevents Godot's environment fog from double-fogging.

uniform sampler2D color_texture : source_color, filter_linear, repeat_disable;
uniform float near_clip : hint_range(0.0, 30000.0) = 6000.0;
uniform float near_fade_width : hint_range(0.0, 10000.0) = 2000.0;
uniform float fog_start : hint_range(0.0, 200000.0) = 80000.0;
uniform float fog_end : hint_range(0.0, 400000.0) = 250000.0;
uniform vec3 fog_color : source_color = vec3(0.75, 0.8, 0.88);
uniform vec3 haze_color : source_color = vec3(0.7, 0.75, 0.85);
uniform float haze_density : hint_range(0.0, 0.0001) = 0.000004;
uniform float brightness : hint_range(0.5, 2.0) = 1.1;

varying float v_distance;
varying vec2 v_uv;
varying vec3 v_world_pos;

void vertex() {
    // Standard vertex transform
    vec4 world_pos = MODEL_MATRIX * vec4(VERTEX, 1.0);
    v_world_pos = world_pos.xyz;
    v_uv = UV;

    // Horizontal distance from camera (ignore vertical for fog/clipping)
    vec2 delta_xz = world_pos.xz - CAMERA_POSITION_WORLD.xz;
    v_distance = length(delta_xz);

    POSITION = PROJECTION_MATRIX * VIEW_MATRIX * world_pos;
}

void fragment() {
    // Discard fragments within the Terrain3D loaded area
    if (v_distance < near_clip) {
        discard;
    }

    // Fade in at the near edge (near_clip to near_clip + near_fade_width)
    float near_alpha = clamp((v_distance - near_clip) / near_fade_width, 0.0, 1.0);
    // Smooth step for the fade
    near_alpha = near_alpha * near_alpha * (3.0 - 2.0 * near_alpha);

    // Sample the elevation-based color texture
    vec3 terrain_color = texture(color_texture, v_uv).rgb * brightness;

    // Distance fog: linear blend from terrain to fog color
    float fog_factor = clamp((v_distance - fog_start) / (fog_end - fog_start), 0.0, 1.0);
    // Smooth the fog transition
    fog_factor = fog_factor * fog_factor * (3.0 - 2.0 * fog_factor);

    // Atmospheric haze (exponential, adds bluish tint at distance)
    float haze_factor = 1.0 - exp(-haze_density * v_distance);
    haze_factor = clamp(haze_factor, 0.0, 0.7);

    // Combine: first apply haze, then fog
    vec3 color = mix(terrain_color, haze_color, haze_factor);
    color = mix(color, fog_color, fog_factor);

    ALBEDO = color;

    // Fade out alpha at extreme distance (beyond fog_end)
    float far_alpha = 1.0 - clamp((v_distance - fog_end) / (fog_end * 0.3), 0.0, 1.0);
    ALPHA = near_alpha * far_alpha;
}
