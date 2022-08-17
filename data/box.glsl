// sokol-shdc -i sgl.glsl -o box.h -l glsl330:glsl100:hlsl4:metal_macos:metal_ios:metal_sim:wgpu -b
@vs vs
uniform vs_params {
    mat4 mvp;
    mat4 tm;
};
in vec4 position;
in vec4 texcoord0;
in vec4 color0;
in vec2 psize;
out vec4 uv;
out vec4 color;
out vec2 esize;
void main() {
    gl_Position = mvp * position;
    uv = texcoord0;
    color = color0;
    esize = psize;
}
@end

@fs fs
float sdRoundBox( in vec2 p, in vec2 b, in float r ) {
    vec2 q = abs(p)-b+r;
    return min(max(q.x,q.y),0.0) + length(max(q,0.0)) - r;
}

uniform sampler2D tex;
in vec4 uv;
in vec4 color;
in vec2 esize;
out vec4 frag_color; 
void main() {
    float d = sdRoundBox( uv.xy, uv.zw, esize.x );
    float w = fwidth(d);
    d = 1.0-smoothstep(0.01-w-esize.y,0.01,d);
    frag_color = vec4(texture(tex, vec2(0.0,0.0)).xyz,d) * color;
}
@end

@program box vs fs
