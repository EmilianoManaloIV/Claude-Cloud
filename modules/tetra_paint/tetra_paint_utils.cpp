#include "tetra_paint_utils.h"

#include "tetra_layer_modes.h"

#include "core/io/image.h"
#include "core/templates/local_vector.h"

#include <cmath>
#include <cstring>

// srgb8_thresholds[c] is the smallest linear value that rounds to code c.
static float srgb8_thresholds[257];
static uint8_t srgb8_lookup[4097];

void tetra_paint_utils_initialize() {
	for (int c = 0; c <= 256; c++) {
		srgb8_thresholds[c] = TetraLayerModes::srgb_to_linear((float(c) - 0.5f) / 255.0f);
	}
	for (int i = 0; i <= 4096; i++) {
		const float encoded = TetraLayerModes::linear_to_srgb(float(i) / 4096.0f);
		srgb8_lookup[i] = uint8_t(encoded * 255.0f + 0.5f);
	}
}

uint8_t tetra_linear_to_srgb8(float p_value) {
	if (!(p_value > 0.0f)) {
		return 0;
	}
	if (p_value >= 1.0f) {
		return 255;
	}
	int code = srgb8_lookup[int(p_value * 4096.0f)];
	while (code < 255 && p_value >= srgb8_thresholds[code + 1]) {
		code++;
	}
	while (code > 0 && p_value < srgb8_thresholds[code]) {
		code--;
	}
	return uint8_t(code);
}

static void build_kernel(float p_sigma, LocalVector<float> &r_kernel) {
	const int radius = p_sigma > 0.0f ? int(ceilf(p_sigma * 3.0f)) : 0;
	r_kernel.resize(radius * 2 + 1);
	float sum = 0.0f;
	for (int i = -radius; i <= radius; i++) {
		const float weight = radius == 0 ? 1.0f : expf(-float(i * i) / (2.0f * p_sigma * p_sigma));
		r_kernel[i + radius] = weight;
		sum += weight;
	}
	for (float &weight : r_kernel) {
		weight /= sum;
	}
}

void tetra_gaussian_blur(const float *p_src, float *p_dst, int p_width, int p_height, int p_channels, float p_sigma_x, float p_sigma_y) {
	LocalVector<float> kernel_x, kernel_y;
	build_kernel(p_sigma_x, kernel_x);
	build_kernel(p_sigma_y, kernel_y);
	const int radius_x = int(kernel_x.size() / 2);
	const int radius_y = int(kernel_y.size() / 2);
	const int stride = p_width * p_channels;

	LocalVector<float> horizontal;
	horizontal.resize(size_t(stride) * p_height);
	tetra_parallel_rows(p_height, p_width * (radius_x + 1), [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			const float *src = p_src + size_t(y) * stride;
			float *dst = horizontal.ptr() + size_t(y) * stride;
			if (radius_x == 0) {
				memcpy(dst, src, sizeof(float) * stride);
				continue;
			}
			for (int x = 0; x < p_width; x++) {
				for (int c = 0; c < p_channels; c++) {
					float sum = 0.0f;
					for (int k = -radius_x; k <= radius_x; k++) {
						const int sx = x + k < 0 ? 0 : (x + k >= p_width ? p_width - 1 : x + k);
						sum += src[sx * p_channels + c] * kernel_x[k + radius_x];
					}
					dst[x * p_channels + c] = sum;
				}
			}
		}
	});
	tetra_parallel_rows(p_height, p_width * (radius_y + 1), [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			float *dst = p_dst + size_t(y) * stride;
			if (radius_y == 0) {
				memcpy(dst, horizontal.ptr() + size_t(y) * stride, sizeof(float) * stride);
				continue;
			}
			memset(dst, 0, sizeof(float) * stride);
			for (int k = -radius_y; k <= radius_y; k++) {
				const int sy = y + k < 0 ? 0 : (y + k >= p_height ? p_height - 1 : y + k);
				const float *src = horizontal.ptr() + size_t(sy) * stride;
				const float weight = kernel_y[k + radius_y];
				for (int i = 0; i < stride; i++) {
					dst[i] += src[i] * weight;
				}
			}
		}
	});
}

Vector<float> tetra_image_to_linear(const Ref<Image> &p_image, bool p_force_linearise) {
	Vector<float> pixels;
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), pixels);
	Ref<Image> image = p_image->duplicate();
	if (image->is_compressed()) {
		ERR_FAIL_COND_V(image->decompress() != OK, pixels);
	}
	if (image->has_mipmaps()) {
		image->clear_mipmaps();
	}
	const Image::Format format = image->get_format();
	const bool is_float = format >= Image::FORMAT_RF && format <= Image::FORMAT_RGBE9995;
	image->convert(Image::FORMAT_RGBAF);
	const int w = image->get_width();
	const int h = image->get_height();
	pixels.resize(int64_t(w) * h * 4);
	float *dst = pixels.ptrw();
	memcpy(dst, image->ptr(), sizeof(float) * 4 * size_t(w) * h);
	if (!is_float || p_force_linearise) {
		tetra_parallel_rows(h, w, [&](int p_first, int p_end) {
			for (int64_t i = int64_t(p_first) * w; i < int64_t(p_end) * w; i++) {
				for (int c = 0; c < 3; c++) {
					dst[i * 4 + c] = TetraLayerModes::srgb_to_linear(dst[i * 4 + c]);
				}
			}
		});
	}
	return pixels;
}
