#version 450
// SPDX-License-Identifier: ISC
// Adapted from ounce-hbmenu switch2/shaders/quad.vert (Copyright 2026 nx-hbmenu Switch 2 port contributors).
// Quads in device pixels; push.scale maps them to clip space. Shape quads carry local coordinates (pixels from the shape
// centre) and the shape parameters for the fragment shader.

layout(push_constant) uniform Push { vec2 scale; } push;

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;
layout(location = 2) in vec2 local;
layout(location = 3) in vec4 shape;   // half width, half height, corner radius, stroke thickness
layout(location = 4) in float mode;   // 0 textured, 1 filled shape, 2 stroked shape
layout(location = 5) in vec4 color;

layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec2 v_local;
layout(location = 2) flat out vec4 v_shape;
layout(location = 3) flat out float v_mode;
layout(location = 4) out vec4 v_color;

void main() {
    v_texcoord = texcoord;
    v_local = local;
    v_shape = shape;
    v_mode = mode;
    v_color = color;
    gl_Position = vec4(position * push.scale - 1.0, 0.0, 1.0);
}
