#pragma once

#include "core/io/file_access.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// Reader for GIMP's native XCF files (every version GIMP 3.2.8 reads: 0 to
// 26). It produces what a TetraImageDocument can hold: canvas-sized linear
// float RGBA layers, bottom to top, and an optional selection mask. What the
// document has no place for (groups, masks, layer offsets, effects, paths,
// channels) is folded in or dropped, and reported in `warnings`.
struct TetraXcfLayer {
	String name;
	bool visible = true;
	float opacity = 1.0f;
	int mode = 0; // TetraLayerModes::Mode.
	bool lock_alpha = false;
	Vector<float> pixels; // width * height * 4, linear light, straight alpha.
};

struct TetraXcfImage {
	int width = 0;
	int height = 0;
	int version = 0;
	LocalVector<TetraXcfLayer> layers; // Bottom to top.
	int active_layer = -1; // Index into layers.
	Vector<float> selection; // width * height, or empty when nothing is selected.
	PackedStringArray warnings;
	String error; // Why reading failed.
};

// Reads p_file from its start. On failure r_image.error says why and
// r_image holds nothing usable. Never reads outside the file, and never
// allocates more than a few canvas-sized buffers beyond the layers
// themselves; the layers together are limited to half of the physical memory
// (at least 2 GiB).
Error tetra_xcf_read(const Ref<FileAccess> &p_file, TetraXcfImage &r_image);
