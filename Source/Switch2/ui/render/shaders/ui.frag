#version 450
// SPDX-License-Identifier: ISC
// Adapted from ounce-hbmenu switch2/shaders/quad.frag (Copyright 2026 nx-hbmenu Switch 2 port contributors).
// Texel times vertex colour, or an antialiased rounded box (filled or stroked inside its edge). Coverage is the
// signed distance to the edge at the pixel centre, so a shape on whole-pixel edges is exact.

layout(set = 0, binding = 0) uniform sampler2D image;

layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec2 v_local;
layout(location = 2) flat in vec4 v_shape;
layout(location = 3) flat in float v_mode;
layout(location = 4) in vec4 v_color;

layout(location = 0) out vec4 out_color;

float roundBox(vec2 p, vec2 half_size, float radius) {
    vec2 q = abs(p) - half_size + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

void main() {
    if (v_mode < 0.5) {
        out_color = texture(image, v_texcoord) * v_color;
        return;
    }
    float d = roundBox(v_local, v_shape.xy, v_shape.z);
    float coverage = clamp(0.5 - d, 0.0, 1.0);
    if (v_mode > 1.5)
        coverage *= clamp(d + v_shape.w + 0.5, 0.0, 1.0);
    out_color = vec4(v_color.rgb, v_color.a * coverage);
}
