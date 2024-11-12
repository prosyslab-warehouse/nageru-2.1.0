#version 450 core

/*
  The motion search is one of the two major components of DIS. It works more or less
  like you'd expect; there's a bunch of overlapping patches (8x8 or 12x12 pixels) in
  a grid, and for each patch, there's a search to try to find the most similar patch
  in the other frame.

  Unlike in a typical video codec, the DIS patch search is based on gradient descent;
  conceptually, you start with an initial guess (the value from the previous level,
  or the zero flow for the very first level), subtract the reference (“template”)
  patch from the candidate, look at the gradient to see in what direction there is
  a lower difference, and then inch a bit toward that direction. (There is seemingly
  nothing like AdaM, Momentum or similar, but the searched value is only in two
  dimensions, so perhaps it doesn't matter as much then.)

  DIS does a tweak to this concept. Since the procedure as outlined above requires
  computing the gradient of the candidate patch, it uses the reference patch as
  candidate (thus the “inverse” name), and thus uses _its_ gradient to understand
  in which direction to move. (This is a bit dodgy, but not _that_ dodgy; after
  all, the two patches are supposed to be quite similar, so their surroundings and
  thus also gradients should also be quite similar.) It's not entirely clear whether
  this is still a win on GPU, where calculations are much cheaper, especially
  the way we parallelize the search, but we've kept it around for now.

  The inverse search is explained and derived in the supplementary material of the
  paper, section A. Do note that there's a typo; the text under equation 9 claims
  that the matrix H is n x n (where presumably n is the patch size), while in reality,
  it's 2x2.

  Our GPU parallellization is fairly dumb right now; we do one patch per fragment
  (ie., parallellize only over patches, not within each patch), which may not
  be optimal. In particular, in the initial level, we only have 40 patches,
  which is on the low side for a GPU, and the memory access patterns may also not
  be ideal.
 */

in vec3 flow_tc;
in vec2 patch_center;
flat in int ref_layer, search_layer;
out vec3 out_flow;

uniform sampler2DArray flow_tex, image_tex;
uniform usampler2DArray grad_tex;  // Also contains the corresponding reference image.
uniform vec2 inv_image_size, inv_prev_level_size;
uniform uint patch_size;
uniform uint num_iterations;

vec3 unpack_gradients(uint v)
{
	uint vi = v & 0xffu;
	uint xi = (v >> 8) & 0xfffu;
	uint yi = v >> 20;
	vec3 r = vec3(xi * (1.0f / 4095.0f) - 0.5f, yi * (1.0f / 4095.0f) - 0.5f, vi * (1.0f / 255.0f));
	return r;
}

// Note: The third variable is the actual pixel value.
vec3 get_gradients(vec3 tc)
{
	vec3 grad = unpack_gradients(texture(grad_tex, tc).x);

	// Zero gradients outside the image. (We'd do this with a sampler,
	// but we want the repeat behavior for the actual texels, in the
	// z channel.)
	if (any(lessThan(tc.xy, vec2(0.0f))) || any(greaterThan(tc.xy, vec2(1.0f)))) {
		grad.xy = vec2(0.0f);
	}

	return grad;
}

void main()
{
	vec2 image_size = textureSize(grad_tex, 0).xy;

	// Lock the patch center to an integer, so that we never get
	// any bilinear artifacts for the gradient. (NOTE: This assumes an
	// even patch size.) Then calculate the bottom-left texel of the patch.
	vec2 base = (round(patch_center * image_size) - (0.5f * patch_size - 0.5f))
		* inv_image_size;

	// First, precompute the pseudo-Hessian for the template patch.
	// This is the part where we really save by the inverse search
	// (ie., we can compute it up-front instead of anew for each
	// patch).
	//
	//  H = sum(S^T S)
	//
	// where S is the gradient at each point in the patch. Note that
	// this is an outer product, so we get a (symmetric) 2x2 matrix,
	// not a scalar.
	mat2 H = mat2(0.0f);
	vec2 grad_sum = vec2(0.0f);  // Used for patch normalization.
	float template_sum = 0.0f;
	for (uint y = 0; y < patch_size; ++y) {
		for (uint x = 0; x < patch_size; ++x) {
			vec2 tc = base + uvec2(x, y) * inv_image_size;
			vec3 grad = get_gradients(vec3(tc, ref_layer));
			H[0][0] += grad.x * grad.x;
			H[1][1] += grad.y * grad.y;
			H[0][1] += grad.x * grad.y;

			template_sum += grad.z;  // The actual template pixel value.
			grad_sum += grad.xy;
		}
	}
	H[1][0] = H[0][1];

	// Make sure we don't get a singular matrix even if e.g. the picture is
	// all black. (The paper doesn't mention this, but the reference code
	// does it, and it seems like a reasonable hack to avoid NaNs. With such
	// a H, we'll go out-of-bounds pretty soon, though.)
	if (determinant(H) < 1e-6) {
		H[0][0] += 1e-6;
		H[1][1] += 1e-6;
	}

	mat2 H_inv = inverse(H);

	// Fetch the initial guess for the flow, and convert from the previous size to this one.
	vec2 initial_u = texture(flow_tex, flow_tc).xy * (image_size * inv_prev_level_size);
	vec2 u = initial_u;
	float mean_diff, first_mean_diff;

	for (uint i = 0; i < num_iterations; ++i) {
		vec2 du = vec2(0.0, 0.0);
		float warped_sum = 0.0f;
		vec2 u_norm = u * inv_image_size;  // In [0..1] coordinates instead of pixels.
		for (uint y = 0; y < patch_size; ++y) {
			for (uint x = 0; x < patch_size; ++x) {
				vec2 tc = base + uvec2(x, y) * inv_image_size;
				vec3 grad = get_gradients(vec3(tc, ref_layer));
				float t = grad.z;
				float warped = texture(image_tex, vec3(tc + u_norm, search_layer)).x;
				du += grad.xy * (warped - t);
				warped_sum += warped;
			}
		}

		// Subtract the mean for patch normalization. We've done our
		// sums without subtracting the means (because we didn't know them
		// beforehand), ie.:
		//
		//   sum(S^T * ((x + µ1) - (y + µ2))) = sum(S^T * (x - y)) + (µ1 – µ2) sum(S^T)
		//
		// which gives trivially
		//
		//   sum(S^T * (x - y)) = [what we calculated] - (µ1 - µ2) sum(S^T)
		//
		// so we can just subtract away the mean difference here.
		mean_diff = (warped_sum - template_sum) * (1.0 / float(patch_size * patch_size));
		du -= grad_sum * mean_diff;

		if (i == 0) {
			first_mean_diff = mean_diff;
		}

		// Do the actual update.
		u -= H_inv * du;
	}

	// Reject if we moved too far. Note that the paper says “too far” is the
	// patch size, but the DIS code uses half of a patch size. The latter seems
	// to give much better overall results.
	//
	// Also reject if the patch goes out-of-bounds (the paper does not mention this,
	// but the code does, and it seems to be critical to avoid really bad behavior
	// at the edges).
	vec2 patch_center = (base * image_size - 0.5f) + patch_size * 0.5f + u;
	if (length(u - initial_u) > (patch_size * 0.5f) ||
	    patch_center.x < -(patch_size * 0.5f) ||
	    image_size.x - patch_center.x < -(patch_size * 0.5f) ||
	    patch_center.y < -(patch_size * 0.5f) ||
	    image_size.y - patch_center.y < -(patch_size * 0.5f)) {
		u = initial_u;
		mean_diff = first_mean_diff;
	}

	// NOTE: The mean patch diff will be for the second-to-last patch,
	// not the true position of du. But hopefully, it will be very close.
	u *= inv_image_size;
	out_flow = vec3(u.x, u.y, mean_diff);
}
