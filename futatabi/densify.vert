#version 450 core
#extension GL_ARB_shader_viewport_layer_array : require

layout(location=0) in vec2 position;
out vec2 image_pos;
flat out vec2 flow_du;
flat out float mean_diff;
flat out int image0_layer, image1_layer;

uniform vec2 patch_size;  // In 0..1 coordinates.
uniform sampler2DArray flow_tex;

void main()
{
	int num_patches = textureSize(flow_tex, 0).x * textureSize(flow_tex, 0).y;
	int patch_layer = gl_InstanceID / num_patches;
	int patch_x = gl_InstanceID % textureSize(flow_tex, 0).x;
	int patch_y = (gl_InstanceID % num_patches) / textureSize(flow_tex, 0).x;

	// Convert the patch index to being the full 0..1 range, to match where
	// the motion search puts the patches. We don't bother with the locking
	// to texel centers, though.
	vec2 patch_center = ivec2(patch_x, patch_y) / (textureSize(flow_tex, 0).xy - 1.0);

	// Increase the patch size a bit; since patch spacing is not necessarily
	// an integer number of pixels, and we don't use conservative rasterization,
	// we could be missing the outer edges of the patch. And it seemingly helps
	// a little bit in general to have some more candidates as well -- although
	// this is measured without variational refinement, so it might be moot
	// with it.
	//
	// This maps [0.0,1.0] to [-0.25,1.25], ie. extends the patch by 25% in
	// all directions.
	vec2 grown_pos = (position * 1.5) - 0.25;

	image_pos = patch_center + patch_size * (grown_pos - 0.5f);

	// Find the flow value for this patch, and send it on to the fragment shader.
	vec3 flow_du_and_mean_diff = texelFetch(flow_tex, ivec3(patch_x, patch_y, patch_layer), 0).xyz;
	flow_du = flow_du_and_mean_diff.xy;
	mean_diff = flow_du_and_mean_diff.z;

	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * image_pos.x - 1.0, 2.0 * image_pos.y - 1.0, -1.0, 1.0);
	gl_Layer = patch_layer;

	// Forward flow (0) goes from 0 to 1. Backward flow (1) goes from 1 to 0.
	image0_layer = patch_layer;
	image1_layer = 1 - patch_layer;
}
