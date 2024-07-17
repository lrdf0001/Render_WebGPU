
struct MyUniforms {    
    color: vec4f,
    time: f32
};

@group(0) @binding(0) var<uniform> uMyUniforms: MyUniforms;

struct VertexInput {
    @location(0) position: vec3f,
    @location(1) color: vec3f,
};

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) color: vec3f,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput  {
	var out: VertexOutput; // create the output struct
	let ratio = 640.0 / 480.0;

    let angle = uMyUniforms.time; // you can multiply it go rotate faster
    let alpha = cos(angle);
    let beta = sin(angle);
    var position = vec3f(
        in.position.x,
        alpha * in.position.y + beta * in.position.z,
        alpha * in.position.z - beta * in.position.y,
    );

    out.position = vec4f(position.x, position.y * ratio, position.z * 0.5 + 0.5, 1.0);
    out.color = in.color; // forward the color attribute to the fragment shader
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let color = in.color;
    return vec4f(color, uMyUniforms.color.a);
}