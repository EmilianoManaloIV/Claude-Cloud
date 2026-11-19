// Reader for GIMP's XCF image files.
//
// The file structure, the property numbers, the tile layout and the RLE and
// zlib tile decoders follow GIMP 3.2.8's loader:
//   app/xcf/xcf-load.c, app/xcf/xcf-read.c, app/xcf/xcf-private.h
//   Copyright (C) 1995 Spencer Kimball and Peter Mattis
//   GNU General Public License, version 3 or later.
// The layer mode numbers are GimpLayerMode from
//   app/operations/operations-enums.h (same copyright and licence).

#include "tetra_xcf_reader.h"

#include "tetra_image_document.h"
#include "tetra_layer_modes.h"

#include "core/io/zip_io.h" // zlib's inflate.
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/string/char_utils.h"

#include <cstring>

namespace {

const int XCF_MAX_VERSION = 26;
const int XCF_TILE = 64;
const int XCF_MAX_IMAGE_SIZE = 524288; // GIMP_MAX_IMAGE_SIZE.
const uint32_t XCF_MAX_STRING = 16 * 1024 * 1024; // MAX_XCF_STRING_LEN.
const int XCF_MAX_GROUP_DEPTH = 64;
const uint32_t XCF_MAX_LAYERS = 16384;
const int XCF_MAX_NAMES_IN_WARNING = 4;

enum {
	PROP_END = 0,
	PROP_COLORMAP = 1,
	PROP_ACTIVE_LAYER = 2,
	PROP_SELECTION = 4,
	PROP_FLOATING_SELECTION = 5,
	PROP_OPACITY = 6,
	PROP_MODE = 7,
	PROP_VISIBLE = 8,
	PROP_LOCK_ALPHA = 10,
	PROP_APPLY_MASK = 11,
	PROP_OFFSETS = 15,
	PROP_COMPRESSION = 17,
	PROP_PARASITES = 21,
	PROP_PATHS = 23,
	PROP_VECTORS = 25,
	PROP_TEXT_LAYER_FLAGS = 26,
	PROP_GROUP_ITEM = 29,
	PROP_ITEM_PATH = 30,
	PROP_FLOAT_OPACITY = 33,
	PROP_COMPOSITE_MODE = 35,
	PROP_COMPOSITE_SPACE = 36,
	PROP_BLEND_SPACE = 37,
	PROP_VECTOR_LAYER = 47,
	PROP_LINK_LAYER = 48,
};

enum {
	COMPRESS_NONE = 0,
	COMPRESS_RLE = 1,
	COMPRESS_ZLIB = 2,
	COMPRESS_FRACTAL = 3,
};

enum { BASE_RGB = 0, BASE_GRAY = 1, BASE_INDEXED = 2 };

enum SampleType { SAMPLE_U8, SAMPLE_U16, SAMPLE_U32, SAMPLE_HALF, SAMPLE_FLOAT, SAMPLE_DOUBLE };

// How the samples of a tile become one output pixel.
enum Layout {
	LAYOUT_RGB, // 3 or 4 components to RGBA.
	LAYOUT_GRAY, // 1 or 2 components to RGBA.
	LAYOUT_INDEXED, // 1 or 2 bytes through the colormap to RGBA.
	LAYOUT_CHANNEL, // 1 component to one float (masks, the selection).
};

// GimpLayerMode values that need more than the table below.
const int GIMP_MODE_NORMAL_LEGACY = 0;
const int GIMP_MODE_DISSOLVE = 1;
const int GIMP_MODE_OVERLAY_LEGACY = 5;
const int GIMP_MODE_SOFTLIGHT_LEGACY = 19;
const int GIMP_MODE_LAST_LEGACY = 22;
const int GIMP_MODE_NORMAL = 28;
const int GIMP_MODE_PASS_THROUGH = 61;

using namespace TetraLayerModes;

// GimpLayerMode (the number stored by PROP_MODE) to Tetra's mode; -1 where
// the document has no such mode.
const int8_t GIMP_MODE_TO_TETRA[] = {
	MODE_NORMAL, // 0 Normal (legacy)
	MODE_DISSOLVE, // 1
	MODE_BEHIND, // 2 Behind (legacy)
	MODE_MULTIPLY, // 3 legacy from here ...
	MODE_SCREEN, // 4
	MODE_SOFT_LIGHT, // 5 Old broken Overlay: GIMP reads it as Soft light (legacy).
	MODE_DIFFERENCE, // 6
	MODE_ADDITION, // 7
	MODE_SUBTRACT, // 8
	MODE_DARKEN_ONLY, // 9
	MODE_LIGHTEN_ONLY, // 10
	MODE_HSV_HUE, // 11
	MODE_HSV_SATURATION, // 12
	MODE_HSL_COLOR, // 13
	MODE_HSV_VALUE, // 14
	MODE_DIVIDE, // 15
	MODE_DODGE, // 16
	MODE_BURN, // 17
	MODE_HARD_LIGHT, // 18
	MODE_SOFT_LIGHT, // 19
	MODE_GRAIN_EXTRACT, // 20
	MODE_GRAIN_MERGE, // 21
	MODE_COLOR_ERASE, // 22 ... to here.
	MODE_OVERLAY, // 23
	MODE_LCH_HUE, // 24
	MODE_LCH_CHROMA, // 25
	MODE_LCH_COLOR, // 26
	MODE_LCH_LIGHTNESS, // 27
	MODE_NORMAL, // 28
	MODE_BEHIND, // 29
	MODE_MULTIPLY, // 30
	MODE_SCREEN, // 31
	MODE_DIFFERENCE, // 32
	MODE_ADDITION, // 33
	MODE_SUBTRACT, // 34
	MODE_DARKEN_ONLY, // 35
	MODE_LIGHTEN_ONLY, // 36
	MODE_HSV_HUE, // 37
	MODE_HSV_SATURATION, // 38
	MODE_HSL_COLOR, // 39
	MODE_HSV_VALUE, // 40
	MODE_DIVIDE, // 41
	MODE_DODGE, // 42
	MODE_BURN, // 43
	MODE_HARD_LIGHT, // 44
	MODE_SOFT_LIGHT, // 45
	MODE_GRAIN_EXTRACT, // 46
	MODE_GRAIN_MERGE, // 47
	MODE_VIVID_LIGHT, // 48
	MODE_PIN_LIGHT, // 49
	MODE_LINEAR_LIGHT, // 50
	MODE_HARD_MIX, // 51
	MODE_EXCLUSION, // 52
	MODE_LINEAR_BURN, // 53
	MODE_LUMA_DARKEN_ONLY, // 54
	MODE_LUMA_LIGHTEN_ONLY, // 55
	MODE_LUMINANCE, // 56
	MODE_COLOR_ERASE, // 57
	MODE_ERASE, // 58
	MODE_MERGE, // 59
	MODE_SPLIT, // 60
	-1, // 61 Pass through
	-1, // 62 Replace
	-1, // 63 Overwrite
	-1, // 64 Anti erase
};
const int GIMP_MODE_COUNT = int(sizeof(GIMP_MODE_TO_TETRA) / sizeof(GIMP_MODE_TO_TETRA[0]));

const char *gimp_mode_name(int p_mode) {
	switch (p_mode) {
		case 61:
			return "Pass through";
		case 62:
			return "Replace";
		case 63:
			return "Overwrite";
		case 64:
			return "Anti erase";
		default:
			return nullptr;
	}
}

bool is_valid_utf8(const uint8_t *p_data, int p_length) {
	int i = 0;
	while (i < p_length) {
		const uint8_t c = p_data[i];
		int extra;
		uint32_t code;
		if (c < 0x80) {
			i++;
			continue;
		} else if ((c & 0xe0) == 0xc0) {
			extra = 1;
			code = c & 0x1f;
		} else if ((c & 0xf0) == 0xe0) {
			extra = 2;
			code = c & 0x0f;
		} else if ((c & 0xf8) == 0xf0) {
			extra = 3;
			code = c & 0x07;
		} else {
			return false;
		}
		if (i + extra >= p_length) {
			return false;
		}
		for (int k = 1; k <= extra; k++) {
			if ((p_data[i + k] & 0xc0) != 0x80) {
				return false;
			}
			code = (code << 6) | (p_data[i + k] & 0x3f);
		}
		static const uint32_t smallest[4] = { 0, 0x80, 0x800, 0x10000 };
		if (code < smallest[extra] || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
			return false;
		}
		i += extra + 1;
	}
	return true;
}

struct XcfLayerEntry {
	String name;
	uint64_t block_offset = 0;
	int width = 0;
	int height = 0;
	int offset_x = 0;
	int offset_y = 0;
	bool visible = true;
	float opacity = 1.0f;
	int gimp_mode = GIMP_MODE_NORMAL;
	bool lock_alpha = false;
	bool apply_mask = true;
	bool active = false;
	bool is_group = false;
	bool is_text = false;
	bool is_vector = false;
	bool is_link = false;
	bool is_floating = false;
	bool is_placeholder = false; // Stands in for a layer that could not be read.
	uint64_t floating_host = 0;
	bool custom_compositing = false;
	bool has_path = false;
	LocalVector<uint32_t> path;

	Vector<float> pixels; // Leaves: canvas-sized RGBA.
	Vector<float> mask; // Groups with an applied mask: canvas-sized, 0 outside the mask.
	int parent = -1;
	LocalVector<int> children;
};

// One layer of the flattened result, top to bottom while it is being built.
struct XcfFlatLayer {
	int entry = -1;
	bool visible = true;
	float opacity = 1.0f;
	bool group_visible = true; // What the groups around it contribute.
	float group_opacity = 1.0f;
	LocalVector<int> masks; // Entries (groups) whose mask applies.
};

struct XcfReader {
	Ref<FileAccess> file;
	uint64_t length = 0;
	bool failed = false; // A read or seek left the file.
	bool out_of_memory = false;

	int version = 0;
	int bytes_per_offset = 4;
	int compression = COMPRESS_NONE;
	int base_type = BASE_RGB;
	SampleType sample_type = SAMPLE_U8;
	int sample_bytes = 1;
	bool samples_linear = false;
	bool samples_big_endian = true;
	int width = 0;
	int height = 0;

	int colormap_size = 0;
	float colormap_linear[256][3] = {};
	float srgb8_to_linear[256] = {};

	uint64_t memory_budget = 0;
	uint64_t memory_used = 0;

	LocalVector<uint8_t> packed;
	LocalVector<uint8_t> tile;

	LocalVector<XcfLayerEntry> entries;
	LocalVector<int> roots;

	// Warnings, gathered per kind so that a file with hundreds of layers
	// still gives a short list.
	bool has_icc_profile = false;
	bool has_paths = false;
	bool has_effects = false;
	bool host_endian_samples = false;
	int broken_layers = 0;
	int saved_channels = 0;
	bool selection_dropped = false;
	bool floating_dropped = false;
	LocalVector<String> clipped_layers;
	LocalVector<String> special_layers; // Text, vector, link.
	LocalVector<String> applied_masks;
	LocalVector<String> dropped_masks;
	LocalVector<String> legacy_layers;
	LocalVector<String> custom_compositing_layers;
	LocalVector<String> unavailable_modes; // "name (mode)".
	LocalVector<String> groups;
	LocalVector<String> group_modes;
	LocalVector<String> floating_layers;
	LocalVector<String> bad_paths;

	// --- Primitives. Every read is checked against the file length. ---

	uint64_t tell() const { return file->get_position(); }

	uint64_t remaining() const {
		const uint64_t position = tell();
		return position >= length ? 0 : length - position;
	}

	bool seek(uint64_t p_position) {
		if (failed || p_position > length) {
			failed = true;
			return false;
		}
		file->seek(p_position);
		return true;
	}

	// For small fixed-size fields only: the destination is zeroed on failure.
	bool read(uint8_t *r_data, uint64_t p_size) {
		if (failed || p_size > remaining() || file->get_buffer(r_data, p_size) != p_size) {
			failed = true;
			memset(r_data, 0, p_size);
			return false;
		}
		return true;
	}

	uint32_t read_u32() {
		uint8_t b[4];
		read(b, 4);
		return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
	}

	int32_t read_i32() { return int32_t(read_u32()); }

	float read_float() {
		const uint32_t bits = read_u32();
		float value;
		memcpy(&value, &bits, sizeof(value));
		return value;
	}

	uint64_t read_offset() {
		if (bytes_per_offset == 4) {
			return read_u32();
		}
		const uint64_t high = read_u32();
		return (high << 32) | read_u32();
	}

	// uint32 length including the trailing NUL, then the bytes; 0 = empty.
	String read_string() {
		const uint32_t size = read_u32();
		if (failed || size == 0) {
			return String();
		}
		if (size > XCF_MAX_STRING || size > remaining()) {
			failed = true;
			return String();
		}
		Vector<uint8_t> bytes;
		bytes.resize(size);
		if (file->get_buffer(bytes.ptrw(), size) != size) {
			failed = true;
			return String();
		}
		int used = 0;
		while (used < int(size) && bytes[used] != 0) {
			used++;
		}
		const char *text = (const char *)bytes.ptr();
		return is_valid_utf8(bytes.ptr(), used) ? String::utf8(text, used) : String::latin1(Span<char>(text, used));
	}

	// Leaves a property: p_base is where its payload starts.
	bool skip_property(uint64_t p_base, uint32_t p_size) {
		if (failed || p_base > length || p_size > length - p_base) {
			failed = true;
			return false;
		}
		return seek(p_base + p_size);
	}

	// Looks for a parasite name in a PROP_PARASITES payload.
	bool find_parasite(uint64_t p_base, uint32_t p_size, const char *p_name) {
		if (p_base > length || p_size > length - p_base) {
			return false;
		}
		const uint64_t end = p_base + p_size;
		while (!failed && tell() + 12 <= end) {
			const String name = read_string();
			read_u32(); // Flags.
			const uint32_t size = read_u32();
			if (failed || tell() > end || size > end - tell()) {
				break;
			}
			if (name == p_name) {
				return true;
			}
			seek(tell() + size);
		}
		// The caller repositions after the property whatever happened here.
		failed = false;
		return false;
	}

	// --- Samples. ---

	static bool decode_precision(int p_value, SampleType &r_type, bool &r_linear) {
		// GimpPrecision: x00 linear, x50 non-linear, x75 perceptual.
		switch (p_value) {
			case 100:
			case 150:
			case 175:
				r_type = SAMPLE_U8;
				break;
			case 200:
			case 250:
			case 275:
				r_type = SAMPLE_U16;
				break;
			case 300:
			case 350:
			case 375:
				r_type = SAMPLE_U32;
				break;
			case 500:
			case 550:
			case 575:
				r_type = SAMPLE_HALF;
				break;
			case 600:
			case 650:
			case 675:
				r_type = SAMPLE_FLOAT;
				break;
			case 700:
			case 750:
			case 775:
				r_type = SAMPLE_DOUBLE;
				break;
			default:
				return false;
		}
		r_linear = (p_value % 100) == 0;
		return true;
	}

	float read_sample(const uint8_t *p_data) const {
		uint64_t bits = 0;
		if (samples_big_endian) {
			for (int i = 0; i < sample_bytes; i++) {
				bits = (bits << 8) | p_data[i];
			}
		} else {
			for (int i = sample_bytes - 1; i >= 0; i--) {
				bits = (bits << 8) | p_data[i];
			}
		}
		float value = 0.0f;
		switch (sample_type) {
			case SAMPLE_U8:
				value = float(bits) / 255.0f;
				break;
			case SAMPLE_U16:
				value = float(bits) / 65535.0f;
				break;
			case SAMPLE_U32:
				value = float(double(bits) / 4294967295.0);
				break;
			case SAMPLE_HALF:
				value = Math::half_to_float(uint16_t(bits));
				break;
			case SAMPLE_FLOAT: {
				const uint32_t bits32 = uint32_t(bits);
				memcpy(&value, &bits32, sizeof(value));
			} break;
			case SAMPLE_DOUBLE: {
				double wide;
				memcpy(&wide, &bits, sizeof(wide));
				value = Math::is_finite(wide) ? float(wide) : 0.0f;
			} break;
		}
		// A NaN or an infinity would spread through every later edit.
		return Math::is_finite(value) ? value : 0.0f;
	}

	float to_linear(float p_value) const {
		return samples_linear ? p_value : srgb_to_linear(p_value);
	}

	// Converts one stored pixel to p_channels floats.
	void convert_pixel(const uint8_t *p_src, Layout p_layout, int p_components, float *r_dst) const {
		switch (p_layout) {
			case LAYOUT_RGB: {
				if (sample_type == SAMPLE_U8 && !samples_linear) {
					r_dst[0] = srgb8_to_linear[p_src[0]];
					r_dst[1] = srgb8_to_linear[p_src[1]];
					r_dst[2] = srgb8_to_linear[p_src[2]];
					r_dst[3] = p_components == 4 ? float(p_src[3]) / 255.0f : 1.0f;
					return;
				}
				for (int c = 0; c < 3; c++) {
					r_dst[c] = to_linear(read_sample(p_src + c * sample_bytes));
				}
				r_dst[3] = p_components == 4 ? CLAMP(read_sample(p_src + 3 * sample_bytes), 0.0f, 1.0f) : 1.0f;
			} break;
			case LAYOUT_GRAY: {
				const float gray = to_linear(read_sample(p_src));
				r_dst[0] = r_dst[1] = r_dst[2] = gray;
				r_dst[3] = p_components == 2 ? CLAMP(read_sample(p_src + sample_bytes), 0.0f, 1.0f) : 1.0f;
			} break;
			case LAYOUT_INDEXED: {
				const float *color = colormap_linear[p_src[0]];
				r_dst[0] = color[0];
				r_dst[1] = color[1];
				r_dst[2] = color[2];
				r_dst[3] = p_components == 2 ? float(p_src[1]) / 255.0f : 1.0f;
			} break;
			case LAYOUT_CHANNEL: {
				// Masks are linear whatever the image precision ("Y u8", "Y float", ...).
				r_dst[0] = CLAMP(read_sample(p_src), 0.0f, 1.0f);
			} break;
		}
	}

	// --- Tiles. ---

	// One byte plane after the other, as packets: 0..126 = a run of n + 1
	// bytes, 127 = a run with a 16-bit length, 128 = literals with a 16-bit
	// length, 129..255 = 256 - n literals.
	bool decode_rle(const uint8_t *p_src, int p_src_size, uint8_t *r_tile, int p_pixels, int p_bpp) const {
		int at = 0;
		for (int plane = 0; plane < p_bpp; plane++) {
			uint8_t *out = r_tile + plane;
			int left = p_pixels;
			while (left > 0) {
				if (at >= p_src_size) {
					return false;
				}
				int count = p_src[at++];
				if (count >= 128) {
					count = 256 - count;
					if (count == 128) {
						if (at + 2 > p_src_size) {
							return false;
						}
						count = (p_src[at] << 8) | p_src[at + 1];
						at += 2;
					}
					left -= count;
					if (left < 0 || at + count > p_src_size) {
						return false;
					}
					for (int i = 0; i < count; i++) {
						*out = p_src[at++];
						out += p_bpp;
					}
				} else {
					count += 1;
					if (count == 128) {
						if (at + 2 > p_src_size) {
							return false;
						}
						count = (p_src[at] << 8) | p_src[at + 1];
						at += 2;
					}
					left -= count;
					if (left < 0 || at >= p_src_size) {
						return false;
					}
					const uint8_t value = p_src[at++];
					for (int i = 0; i < count; i++) {
						*out = value;
						out += p_bpp;
					}
				}
			}
		}
		return true;
	}

	// One zlib stream holding at most p_tile_size bytes.
	static bool decode_zlib(const uint8_t *p_src, int p_src_size, uint8_t *r_tile, int p_tile_size) {
		z_stream stream;
		memset(&stream, 0, sizeof(stream));
		stream.zalloc = zipio_alloc;
		stream.zfree = zipio_free;
		if (inflateInit(&stream) != Z_OK) {
			return false;
		}
		stream.next_in = (Bytef *)p_src;
		stream.avail_in = p_src_size;
		stream.next_out = r_tile;
		stream.avail_out = p_tile_size;
		const int status = inflate(&stream, Z_FINISH);
		inflateEnd(&stream);
		return status == Z_STREAM_END;
	}

	// Reads the tile at the current position into `tile` (zeroed first).
	bool read_tile(int p_tile_size, int p_pixels, int p_bpp, uint64_t p_data_length) {
		tile.resize(p_tile_size);
		memset(tile.ptr(), 0, p_tile_size);
		if (compression == COMPRESS_NONE) {
			return uint64_t(p_tile_size) <= remaining() && file->get_buffer(tile.ptr(), p_tile_size) == uint64_t(p_tile_size);
		}
		if (compression != COMPRESS_RLE && compression != COMPRESS_ZLIB) {
			return false; // "Fractal" was never implemented.
		}
		// The last tile's length is not known: GIMP takes what is there.
		const uint64_t available = MIN(p_data_length, remaining());
		if (available == 0) {
			return true; // GIMP leaves such a tile empty.
		}
		packed.resize(available);
		if (file->get_buffer(packed.ptr(), available) != available) {
			return false;
		}
		if (compression == COMPRESS_RLE) {
			return decode_rle(packed.ptr(), int(available), tile.ptr(), p_pixels, p_bpp);
		}
		return decode_zlib(packed.ptr(), int(available), tile.ptr(), p_tile_size);
	}

	// Decodes the hierarchy at p_offset (a drawable of p_width x p_height
	// placed at p_origin on the canvas) into r_dst, a canvas-sized buffer of
	// p_channels floats per pixel that starts out zeroed. Only the tiles
	// that touch the canvas are read, so the cost is bounded by the canvas
	// whatever size the drawable claims to have.
	bool decode_hierarchy(uint64_t p_offset, int p_width, int p_height, const Point2i &p_origin, Layout p_layout, int p_components, float *r_dst, int p_channels) {
		if (!seek(p_offset)) {
			return false;
		}
		const int bpp = p_components * sample_bytes;
		const int32_t stored_width = read_i32();
		const int32_t stored_height = read_i32();
		const int32_t stored_bpp = read_i32();
		if (failed || stored_width != p_width || stored_height != p_height || stored_bpp != bpp) {
			return false;
		}
		const uint64_t position = tell();
		const uint64_t level_offset = read_offset();
		if (failed || level_offset < position || !seek(level_offset)) {
			return false;
		}
		const int32_t level_width = read_i32();
		const int32_t level_height = read_i32();
		if (failed || level_width != p_width || level_height != p_height) {
			return false;
		}
		const uint64_t table = tell();
		const uint64_t first = read_offset();
		if (failed) {
			return false;
		}

		// The part of the drawable that is on the canvas, in its own coordinates.
		const Rect2i visible = Rect2i(p_origin, Size2i(p_width, p_height)).intersection(Rect2i(0, 0, width, height));
		if (visible.size.x <= 0 || visible.size.y <= 0) {
			return true;
		}
		const int x0 = visible.position.x - p_origin.x;
		const int y0 = visible.position.y - p_origin.y;
		const int x1 = x0 + visible.size.x;
		const int y1 = y0 + visible.size.y;

		// What a tile that was never stored (all zero bytes) looks like:
		// opaque black without an alpha channel, colour 0 of an indexed image.
		{
			uint8_t zero[32] = {};
			float converted[4] = {};
			convert_pixel(zero, p_layout, p_components, converted);
			bool blank = true;
			for (int c = 0; c < p_channels; c++) {
				blank = blank && converted[c] == 0.0f;
			}
			if (!blank) {
				for (int y = visible.position.y; y < visible.position.y + visible.size.y; y++) {
					float *row = r_dst + (size_t(y) * width + visible.position.x) * p_channels;
					for (int x = 0; x < visible.size.x; x++) {
						memcpy(row + size_t(x) * p_channels, converted, sizeof(float) * p_channels);
					}
				}
			}
		}
		if (first == 0) {
			return true; // An empty level.
		}

		const int columns = (p_width + XCF_TILE - 1) / XCF_TILE;
		const uint64_t max_data_length = uint64_t(XCF_TILE) * XCF_TILE * bpp * 3 / 2; // XCF_TILE_MAX_DATA_LENGTH_FACTOR.
		for (int ty = y0 / XCF_TILE; ty <= (y1 - 1) / XCF_TILE; ty++) {
			for (int tx = x0 / XCF_TILE; tx <= (x1 - 1) / XCF_TILE; tx++) {
				const uint64_t index = uint64_t(ty) * columns + tx;
				if (!seek(table + index * bytes_per_offset)) {
					return false;
				}
				const uint64_t offset = read_offset();
				uint64_t next = read_offset();
				if (failed || offset == 0) {
					return false; // Not enough tiles in the level.
				}
				if (next == 0) {
					next = offset + max_data_length;
				}
				if (next < offset || next - offset > max_data_length || !seek(offset)) {
					return false;
				}
				const int tile_x = tx * XCF_TILE;
				const int tile_y = ty * XCF_TILE;
				const int tile_width = MIN(XCF_TILE, p_width - tile_x);
				const int tile_height = MIN(XCF_TILE, p_height - tile_y);
				if (!read_tile(tile_width * tile_height * bpp, tile_width * tile_height, bpp, next - offset)) {
					return false;
				}
				const int from_x = MAX(tile_x, x0);
				const int to_x = MIN(tile_x + tile_width, x1);
				const int from_y = MAX(tile_y, y0);
				const int to_y = MIN(tile_y + tile_height, y1);
				for (int y = from_y; y < to_y; y++) {
					const uint8_t *src = tile.ptr() + (size_t(y - tile_y) * tile_width + (from_x - tile_x)) * bpp;
					float *dst = r_dst + (size_t(y + p_origin.y) * width + (from_x + p_origin.x)) * p_channels;
					for (int x = from_x; x < to_x; x++) {
						convert_pixel(src, p_layout, p_components, dst);
						src += bpp;
						dst += p_channels;
					}
				}
			}
		}
		return true;
	}

	// --- Memory. ---

	bool reserve(uint64_t p_bytes) {
		if (memory_used + p_bytes > memory_budget) {
			out_of_memory = true;
			return false;
		}
		memory_used += p_bytes;
		return true;
	}

	uint64_t canvas_pixels() const { return uint64_t(width) * height; }

	// --- Blocks. ---

	// A channel block used as a mask or as the selection: decoded into a
	// canvas-sized float buffer. r_is_selection reports PROP_SELECTION.
	// With p_decode false only the header and the properties are read.
	bool read_channel(uint64_t p_offset, const Point2i &p_origin, bool p_decode, int &r_width, int &r_height, bool &r_is_selection, Vector<float> *r_values) {
		if (!seek(p_offset)) {
			return false;
		}
		r_width = read_i32();
		r_height = read_i32();
		if (failed || r_width <= 0 || r_height <= 0 || r_width > XCF_MAX_IMAGE_SIZE || r_height > XCF_MAX_IMAGE_SIZE) {
			return false;
		}
		const String name = read_string(); // Not kept.
		r_is_selection = false;
		for (;;) {
			const uint32_t type = read_u32();
			const uint32_t size = read_u32();
			if (failed) {
				return false;
			}
			if (type == PROP_END) {
				break;
			}
			if (type == PROP_SELECTION) {
				r_is_selection = true;
			}
			if (!skip_property(tell(), size)) {
				return false;
			}
		}
		const uint64_t position = tell();
		const uint64_t hierarchy = read_offset();
		if (failed || hierarchy < position) {
			return false;
		}
		if (!p_decode) {
			return true;
		}
		if (!reserve(canvas_pixels() * sizeof(float))) {
			return false;
		}
		r_values->resize_initialized(canvas_pixels());
		const bool ok = decode_hierarchy(hierarchy, r_width, r_height, p_origin, LAYOUT_CHANNEL, 1, r_values->ptrw(), 1);
		if (!ok) {
			*r_values = Vector<float>();
			memory_used -= canvas_pixels() * sizeof(float);
		}
		return ok;
	}

	bool read_layer_properties(XcfLayerEntry &r_entry) {
		bool has_float_opacity = false;
		for (;;) {
			const uint32_t type = read_u32();
			const uint32_t size = read_u32();
			if (failed) {
				return false;
			}
			const uint64_t base = tell();
			switch (type) {
				case PROP_END:
					return true;
				case PROP_ACTIVE_LAYER:
					r_entry.active = true;
					break;
				case PROP_FLOATING_SELECTION:
					r_entry.is_floating = true;
					r_entry.floating_host = read_offset();
					break;
				case PROP_OPACITY: {
					const uint32_t opacity = read_u32();
					if (!has_float_opacity) {
						r_entry.opacity = float(MIN(opacity, 255u)) / 255.0f;
					}
				} break;
				case PROP_FLOAT_OPACITY: {
					const float opacity = read_float();
					r_entry.opacity = Math::is_finite(opacity) ? CLAMP(opacity, 0.0f, 1.0f) : 1.0f;
					has_float_opacity = true;
				} break;
				case PROP_VISIBLE:
					r_entry.visible = read_u32() != 0;
					break;
				case PROP_LOCK_ALPHA:
					r_entry.lock_alpha = read_u32() != 0;
					break;
				case PROP_APPLY_MASK:
					r_entry.apply_mask = read_u32() != 0;
					break;
				case PROP_OFFSETS: {
					const int32_t x = read_i32();
					const int32_t y = read_i32();
					r_entry.offset_x = (x < -XCF_MAX_IMAGE_SIZE || x > XCF_MAX_IMAGE_SIZE) ? 0 : x;
					r_entry.offset_y = (y < -XCF_MAX_IMAGE_SIZE || y > XCF_MAX_IMAGE_SIZE) ? 0 : y;
				} break;
				case PROP_MODE: {
					r_entry.gimp_mode = read_i32();
					if (r_entry.gimp_mode == GIMP_MODE_OVERLAY_LEGACY) {
						r_entry.gimp_mode = GIMP_MODE_SOFTLIGHT_LEGACY;
					}
				} break;
				case PROP_BLEND_SPACE:
				case PROP_COMPOSITE_SPACE:
				case PROP_COMPOSITE_MODE: {
					// Negative: "auto" when the file was saved. Positive: chosen by the user.
					if (read_i32() > 0) {
						r_entry.custom_compositing = true;
					}
				} break;
				case PROP_PARASITES:
					if (find_parasite(base, size, "gimp-text-layer")) {
						r_entry.is_text = true;
					}
					break;
				case PROP_TEXT_LAYER_FLAGS:
					r_entry.is_text = true;
					break;
				case PROP_VECTOR_LAYER:
					r_entry.is_vector = true;
					break;
				case PROP_LINK_LAYER:
					r_entry.is_link = true;
					break;
				case PROP_GROUP_ITEM:
					r_entry.is_group = true;
					break;
				case PROP_ITEM_PATH: {
					r_entry.has_path = true;
					r_entry.path.clear();
					if (size > remaining()) {
						failed = true;
						return false;
					}
					for (uint32_t i = 0; i + 4 <= size; i += 4) {
						const uint32_t index = read_u32();
						if (r_entry.path.size() <= uint32_t(XCF_MAX_GROUP_DEPTH)) {
							r_entry.path.push_back(index);
						}
					}
				} break;
				default:
					break;
			}
			if (!skip_property(base, size)) {
				return false;
			}
		}
	}

	// Reads the layer block at p_offset into a new entry. False: the layer
	// is unreadable and is left out (GIMP does the same and goes on).
	bool read_layer(uint64_t p_offset) {
		if (!seek(p_offset)) {
			return false;
		}
		XcfLayerEntry entry;
		entry.block_offset = p_offset;
		entry.width = read_i32();
		entry.height = read_i32();
		const uint32_t type = read_u32();
		entry.name = read_string();
		if (failed || type > 5) {
			return false;
		}
		if (!read_layer_properties(entry)) {
			return false;
		}
		if (entry.width <= 0 || entry.height <= 0 || entry.width > XCF_MAX_IMAGE_SIZE || entry.height > XCF_MAX_IMAGE_SIZE) {
			// GIMP forgives this where the pixels can be made again.
			if (!entry.is_group && !entry.is_text && !entry.is_link) {
				return false;
			}
			entry.width = entry.height = 1;
		}

		// A layer type is the base type times two, plus one with alpha. Only
		// greyscale layers may differ from the image (a floating selection of
		// a channel); the others use the image's format.
		const bool has_alpha = (type & 1) != 0;
		const int layer_base = int(type / 2) == BASE_GRAY ? int(BASE_GRAY) : base_type;
		const Layout layout = layer_base == BASE_GRAY ? LAYOUT_GRAY : (layer_base == BASE_INDEXED ? LAYOUT_INDEXED : LAYOUT_RGB);
		const int components = (layer_base == BASE_RGB ? 3 : 1) + (has_alpha ? 1 : 0);

		uint64_t position = tell();
		const uint64_t hierarchy = read_offset();
		const uint64_t mask_offset = read_offset();
		const uint64_t effects_offset = version >= 20 ? read_offset() : 0;
		if (failed) {
			return false;
		}
		const Point2i origin(entry.offset_x, entry.offset_y);
		const uint64_t layer_bytes = canvas_pixels() * 4 * sizeof(float);

		if (!entry.is_group) {
			if (hierarchy < position || !reserve(layer_bytes)) {
				return false;
			}
			entry.pixels.resize_initialized(canvas_pixels() * 4);
			if (!decode_hierarchy(hierarchy, entry.width, entry.height, origin, layout, components, entry.pixels.ptrw(), 4)) {
				memory_used -= layer_bytes;
				return false;
			}
		}

		position += bytes_per_offset;
		if (mask_offset != 0) {
			if (mask_offset < position) {
				if (!entry.is_group) {
					memory_used -= layer_bytes;
				}
				return false;
			}
			int mask_width = 0, mask_height = 0;
			bool is_selection = false;
			Vector<float> mask;
			const bool wanted = entry.apply_mask;
			if (!read_channel(mask_offset, origin, wanted, mask_width, mask_height, is_selection, &mask)) {
				if (!entry.is_group) {
					memory_used -= layer_bytes;
				}
				return false;
			}
			if (!wanted || mask_width != entry.width || mask_height != entry.height) {
				dropped_masks.push_back(entry.name);
				if (wanted) {
					memory_used -= canvas_pixels() * sizeof(float);
				}
			} else if (entry.is_group) {
				entry.mask = mask; // Applied to the children when the groups are flattened.
				applied_masks.push_back(entry.name);
			} else {
				float *pixels = entry.pixels.ptrw();
				const float *values = mask.ptr();
				const uint64_t count = canvas_pixels();
				for (uint64_t i = 0; i < count; i++) {
					pixels[i * 4 + 3] *= values[i];
				}
				memory_used -= count * sizeof(float);
				applied_masks.push_back(entry.name);
			}
		}

		if (effects_offset != 0) {
			has_effects = true;
		}
		if (!entry.is_group) {
			if (entry.is_text || entry.is_vector || entry.is_link) {
				special_layers.push_back(entry.name);
			}
			if (origin.x < 0 || origin.y < 0 || int64_t(origin.x) + entry.width > width || int64_t(origin.y) + entry.height > height) {
				clipped_layers.push_back(entry.name);
			}
		}
		entries.push_back(entry);
		return true;
	}

	// --- The image. ---

	bool read_header(String &r_error) {
		uint8_t magic[14];
		if (!read(magic, 14) || memcmp(magic, "gimp xcf ", 9) != 0) {
			r_error = "Not an XCF file.";
			return false;
		}
		if (memcmp(magic + 9, "file", 5) == 0) {
			version = 0;
		} else if (magic[9] == 'v' && magic[13] == 0 && is_digit(char32_t(magic[10])) && is_digit(char32_t(magic[11])) && is_digit(char32_t(magic[12]))) {
			version = (magic[10] - '0') * 100 + (magic[11] - '0') * 10 + (magic[12] - '0');
		} else {
			r_error = "Not an XCF file.";
			return false;
		}
		if (version > XCF_MAX_VERSION) {
			r_error = vformat("XCF version %d is newer than the versions Tetra reads (up to %d).", version, XCF_MAX_VERSION);
			return false;
		}
		bytes_per_offset = version >= 11 ? 8 : 4;
		// Before version 12 samples wider than a byte were stored the way the
		// writing machine held them; in practice that was little-endian.
		samples_big_endian = version >= 12;

		width = read_i32();
		height = read_i32();
		base_type = read_i32();
		if (failed || base_type < BASE_RGB || base_type > BASE_INDEXED) {
			r_error = "The XCF header is damaged (invalid image type).";
			return false;
		}
		if (width <= 0 || height <= 0 || width > XCF_MAX_IMAGE_SIZE || height > XCF_MAX_IMAGE_SIZE) {
			// GIMP is lenient here too and hopes that the layers are sound.
			width = 1;
			height = 1;
		}

		int precision = 150;
		if (version >= 4) {
			const int stored = read_i32();
			if (version == 4) {
				static const int v4[] = { 150, 250, 300, 500, 600 };
				precision = (stored >= 0 && stored < 5) ? v4[stored] : -1;
			} else if (version == 5 || version == 6) {
				// The half and float values moved up by 100 in version 7.
				switch (stored) {
					case 100:
					case 150:
					case 200:
					case 250:
					case 300:
					case 350:
						precision = stored;
						break;
					case 400:
					case 450:
					case 500:
					case 550:
						precision = stored + 100;
						break;
					default:
						precision = -1;
				}
			} else {
				precision = stored;
			}
		}
		if (failed || !decode_precision(precision, sample_type, samples_linear) || (base_type == BASE_INDEXED && precision != 150)) {
			r_error = "The XCF header is damaged (invalid precision).";
			return false;
		}
		static const int sizes[] = { 1, 2, 4, 2, 4, 8 };
		sample_bytes = sizes[sample_type];
		host_endian_samples = !samples_big_endian && sample_bytes > 1;
		return true;
	}

	bool read_image_properties() {
		for (;;) {
			const uint32_t type = read_u32();
			const uint32_t size = read_u32();
			if (failed) {
				return false;
			}
			const uint64_t base = tell();
			switch (type) {
				case PROP_END:
					return true;
				case PROP_COLORMAP: {
					// The stored size of this property cannot be trusted (old
					// versions wrote a wrong one), so it is read by its content.
					const uint32_t count = read_u32();
					if (failed || count > 256) {
						return false;
					}
					uint8_t colors[768] = {};
					if (version == 0) {
						// Version 0 stored broken colormaps; GIMP substitutes greys.
						if (!seek(tell() + count)) {
							return false;
						}
						for (uint32_t i = 0; i < count; i++) {
							colors[i * 3] = colors[i * 3 + 1] = colors[i * 3 + 2] = uint8_t(i);
						}
					} else if (count > 0 && !read(colors, count * 3)) {
						return false;
					}
					if (base_type == BASE_INDEXED) {
						colormap_size = int(count);
						for (uint32_t i = 0; i < count; i++) {
							for (int c = 0; c < 3; c++) {
								colormap_linear[i][c] = srgb8_to_linear[colors[i * 3 + c]];
							}
						}
					}
					continue;
				}
				case PROP_COMPRESSION: {
					uint8_t value = 0;
					read(&value, 1);
					if (failed || value > COMPRESS_FRACTAL) {
						return false;
					}
					compression = value;
				} break;
				case PROP_PARASITES:
					if (find_parasite(base, size, "icc-profile")) {
						has_icc_profile = true;
					}
					break;
				case PROP_PATHS:
				case PROP_VECTORS:
					has_paths = true;
					break;
				default:
					break;
			}
			if (!skip_property(base, size)) {
				return false;
			}
		}
	}

	// Hangs each entry under its parent the way GIMP does: PROP_ITEM_PATH
	// names the position, every index but the last leads to the parent, and
	// the layer is appended to that parent's children.
	void build_tree() {
		bool floating_seen = false;
		for (uint32_t i = 0; i < entries.size(); i++) {
			XcfLayerEntry &entry = entries[i];
			if (entry.is_floating) {
				// Not part of the stack; it is attached to its host later.
				floating_seen = true;
				continue;
			}
			int parent = -1;
			bool valid = true;
			if (entry.has_path && entry.path.size() > 1) {
				valid = entry.path.size() <= uint32_t(XCF_MAX_GROUP_DEPTH);
				const LocalVector<int> *level = &roots;
				for (uint32_t k = 0; valid && k + 1 < entry.path.size(); k++) {
					uint32_t index = entry.path[k];
					if (k == 0 && floating_seen) {
						// The floating selection took a top-level position in the file.
						valid = index > 0;
						index--;
					}
					if (!valid || index >= level->size() || !entries[(*level)[index]].is_group) {
						valid = false;
						break;
					}
					parent = (*level)[index];
					level = &entries[parent].children;
				}
			}
			if (!valid) {
				bad_paths.push_back(entry.name);
				parent = -1;
			}
			entry.parent = parent;
			if (parent < 0) {
				roots.push_back(int(i));
			} else {
				entries[parent].children.push_back(int(i));
			}
		}
	}

	void flatten(const LocalVector<int> &p_level, bool p_visible, float p_opacity, const LocalVector<int> &p_masks, int p_depth, LocalVector<XcfFlatLayer> &r_flat) {
		for (int index : p_level) {
			const XcfLayerEntry &entry = entries[index];
			if (entry.is_placeholder) {
				// An unreadable layer; if it was a group, its layers are kept.
				if (p_depth < XCF_MAX_GROUP_DEPTH) {
					flatten(entry.children, p_visible, p_opacity, p_masks, p_depth + 1, r_flat);
				}
				continue;
			}
			if (entry.is_group) {
				groups.push_back(entry.name);
				if (entry.gimp_mode != GIMP_MODE_NORMAL && entry.gimp_mode != GIMP_MODE_NORMAL_LEGACY) {
					group_modes.push_back(entry.name);
				}
				LocalVector<int> masks(p_masks);
				if (!entry.mask.is_empty()) {
					masks.push_back(index);
				}
				if (p_depth < XCF_MAX_GROUP_DEPTH) {
					flatten(entry.children, p_visible && entry.visible, p_opacity * entry.opacity, masks, p_depth + 1, r_flat);
				}
				continue;
			}
			XcfFlatLayer flat;
			flat.entry = index;
			flat.visible = p_visible && entry.visible;
			flat.opacity = p_opacity * entry.opacity;
			flat.group_visible = p_visible;
			flat.group_opacity = p_opacity;
			flat.masks = p_masks;
			r_flat.push_back(flat);
		}
	}

	static String list_names(const LocalVector<String> &p_names) {
		String text;
		for (uint32_t i = 0; i < p_names.size() && i < uint32_t(XCF_MAX_NAMES_IN_WARNING); i++) {
			text += (i ? ", \"" : "\"") + p_names[i] + "\"";
		}
		if (p_names.size() > uint32_t(XCF_MAX_NAMES_IN_WARNING)) {
			text += vformat(RTR(" and %d more"), int(p_names.size()) - XCF_MAX_NAMES_IN_WARNING);
		}
		return text;
	}

	void collect_warnings(PackedStringArray &r_warnings) const {
		if (broken_layers > 0) {
			r_warnings.push_back(vformat(RTR("%d damaged layers could not be read and were left out."), broken_layers));
		}
		if (!clipped_layers.is_empty()) {
			r_warnings.push_back(vformat(RTR("Layers are as large as the canvas here: the parts of %s outside the canvas were cut off."), list_names(clipped_layers)));
		}
		if (!groups.is_empty()) {
			r_warnings.push_back(vformat(RTR("Layer groups were dissolved: their opacity, visibility and mask are folded into each of their layers, which now blend with everything below instead of within the group: %s."), list_names(groups)));
		}
		if (!group_modes.is_empty()) {
			r_warnings.push_back(vformat(RTR("The layer mode of these groups (pass through or another mode) was dropped: %s."), list_names(group_modes)));
		}
		if (!applied_masks.is_empty()) {
			r_warnings.push_back(vformat(RTR("Layer masks were applied to the transparency of %s."), list_names(applied_masks)));
		}
		if (!dropped_masks.is_empty()) {
			r_warnings.push_back(vformat(RTR("Disabled layer masks were left out: %s."), list_names(dropped_masks)));
		}
		if (!unavailable_modes.is_empty()) {
			r_warnings.push_back(vformat(RTR("Layer modes that Tetra does not have were replaced by Normal: %s."), list_names(unavailable_modes)));
		}
		if (!legacy_layers.is_empty()) {
			r_warnings.push_back(vformat(RTR("Legacy layer modes were replaced by their current versions, which can look slightly different: %s."), list_names(legacy_layers)));
		}
		if (!custom_compositing_layers.is_empty()) {
			r_warnings.push_back(vformat(RTR("Custom blend space, composite space or composite mode settings were dropped: %s."), list_names(custom_compositing_layers)));
		}
		if (!special_layers.is_empty()) {
			r_warnings.push_back(vformat(RTR("Text, vector and link layers were imported as pixels: %s."), list_names(special_layers)));
		}
		if (!floating_layers.is_empty()) {
			r_warnings.push_back(vformat(RTR("The floating selection became a layer: %s."), list_names(floating_layers)));
		}
		if (floating_dropped) {
			r_warnings.push_back(RTR("A floating selection of a channel or mask was left out."));
		}
		if (!bad_paths.is_empty()) {
			r_warnings.push_back(vformat(RTR("The place of these layers in their group is damaged; they were put at the top level: %s."), list_names(bad_paths)));
		}
		if (has_effects) {
			r_warnings.push_back(RTR("Layer effects (non-destructive filters) were not imported."));
		}
		if (saved_channels > 0) {
			r_warnings.push_back(vformat(RTR("%d saved channels were not imported."), saved_channels));
		}
		if (selection_dropped) {
			r_warnings.push_back(RTR("The selection could not be read and was left out."));
		}
		if (has_paths) {
			r_warnings.push_back(RTR("Paths were not imported."));
		}
		if (has_icc_profile) {
			r_warnings.push_back(RTR("The embedded colour profile was ignored; the colours are taken as sRGB."));
		}
		if (host_endian_samples) {
			r_warnings.push_back(RTR("This old high bit depth file does not say how its numbers are stored; little-endian was assumed."));
		}
	}

	Error run(TetraXcfImage &r_image) {
		length = file->get_length();
		file->seek(0);
		for (int i = 0; i < 256; i++) {
			srgb8_to_linear[i] = srgb_to_linear(float(i) / 255.0f);
		}

		if (!read_header(r_image.error)) {
			return ERR_FILE_UNRECOGNIZED;
		}
		if (!read_image_properties()) {
			r_image.error = "The image properties of the XCF file are damaged.";
			return ERR_FILE_CORRUPT;
		}
		if (!TetraImageDocument::is_size_supported(width, height)) {
			r_image.error = vformat("The image is %d x %d pixels, more than the %d megapixels a document can hold.", width, height, int(TetraImageDocument::MAX_PIXELS / 1000000));
			return ERR_OUT_OF_MEMORY;
		}
		const Dictionary memory = OS::get_singleton()->get_memory_info();
		const int64_t physical = memory.get("physical", -1);
		const int64_t available = memory.get("available", -1);
		memory_budget = MAX(uint64_t(MAX(available > 0 ? available : physical, int64_t(0))) / 2, uint64_t(2) << 30);

		// The layer table: top layer first, a group before its children.
		for (;;) {
			const uint64_t offset = read_offset();
			if (failed || offset == 0) {
				break; // GIMP also stops quietly where the table is cut off.
			}
			const uint64_t saved = tell();
			if (offset < saved) {
				r_image.error = "The layer table of the XCF file is damaged.";
				return ERR_FILE_CORRUPT;
			}
			if (entries.size() >= XCF_MAX_LAYERS) {
				r_image.error = vformat("The XCF file has more than %d layers.", XCF_MAX_LAYERS);
				return ERR_FILE_CORRUPT;
			}
			if (!read_layer(offset)) {
				if (out_of_memory) {
					r_image.error = vformat("The layers of this %d x %d image need more memory than is available.", width, height);
					return ERR_OUT_OF_MEMORY;
				}
				broken_layers++;
				// A placeholder keeps the positions that later item paths refer to.
				XcfLayerEntry placeholder;
				placeholder.is_group = true;
				placeholder.is_placeholder = true;
				entries.push_back(placeholder);
			}
			failed = false;
			if (!seek(saved)) {
				break;
			}
		}
		const bool layer_table_complete = !failed;

		// The channel table: only the selection is of use.
		bool have_selection = false;
		int path_count = 0;
		if (layer_table_complete) {
			for (;;) {
				const uint64_t offset = read_offset();
				if (failed || offset == 0) {
					break;
				}
				const uint64_t saved = tell();
				if (offset < saved) {
					break;
				}
				int channel_width = 0, channel_height = 0;
				bool is_selection = false;
				if (read_channel(offset, Point2i(), false, channel_width, channel_height, is_selection, nullptr)) {
					if (!is_selection) {
						saved_channels++;
					} else if (!have_selection) {
						Vector<float> values;
						if (channel_width == width && channel_height == height && read_channel(offset, Point2i(), true, channel_width, channel_height, is_selection, &values)) {
							r_image.selection = values;
							have_selection = true;
						} else {
							selection_dropped = true;
						}
					}
				}
				if (out_of_memory) {
					r_image.error = vformat("The layers of this %d x %d image need more memory than is available.", width, height);
					return ERR_OUT_OF_MEMORY;
				}
				failed = false;
				if (!seek(saved)) {
					break;
				}
			}
			if (!failed && version >= 18) {
				for (;;) {
					const uint64_t offset = read_offset();
					if (failed || offset == 0) {
						break;
					}
					path_count++;
				}
			}
		}
		has_paths = has_paths || path_count > 0;
		failed = false;

		// Groups to a flat list, top to bottom.
		build_tree();
		LocalVector<XcfFlatLayer> flat;
		flatten(roots, true, 1.0f, LocalVector<int>(), 0, flat);

		// The floating selection sits right above the layer it floats on.
		for (uint32_t i = 0; i < entries.size(); i++) {
			const XcfLayerEntry &entry = entries[i];
			if (!entry.is_floating || entry.is_group) {
				continue;
			}
			int host = -1;
			for (uint32_t k = 0; k < flat.size(); k++) {
				if (entries[flat[k].entry].block_offset == entry.floating_host && entry.floating_host != 0) {
					host = int(k);
					break;
				}
			}
			if (host < 0) {
				floating_dropped = true;
				continue;
			}
			XcfFlatLayer floating = flat[host];
			floating.entry = int(i);
			floating.visible = floating.group_visible && entry.visible;
			floating.opacity = floating.group_opacity * entry.opacity;
			flat.insert(host, floating);
			floating_layers.push_back(entry.name);
		}

		if (flat.is_empty()) {
			r_image.error = broken_layers > 0 ? "None of the layers of the XCF file could be read." : "The XCF file has no layers.";
			return ERR_FILE_CORRUPT;
		}

		r_image.width = width;
		r_image.height = height;
		r_image.version = version;
		r_image.layers.resize(flat.size());
		const uint64_t count = canvas_pixels();
		for (uint32_t i = 0; i < flat.size(); i++) {
			// Top first, so that the warnings name the layers in the order of GIMP's list.
			const XcfFlatLayer &source = flat[i];
			XcfLayerEntry &entry = entries[source.entry];
			const int target = int(flat.size() - 1 - i);
			TetraXcfLayer &layer = r_image.layers[target];
			layer.name = entry.name;
			layer.visible = source.visible;
			layer.opacity = CLAMP(source.opacity, 0.0f, 1.0f);
			layer.lock_alpha = entry.lock_alpha;

			const int mode = entry.gimp_mode;
			if (mode < 0 || mode >= GIMP_MODE_COUNT || GIMP_MODE_TO_TETRA[mode] < 0) {
				layer.mode = MODE_NORMAL;
				const char *known = (mode >= 0 && mode < GIMP_MODE_COUNT) ? gimp_mode_name(mode) : nullptr;
				unavailable_modes.push_back(vformat("%s (%s)", entry.name, known ? String(known) : vformat("mode %d", mode)));
			} else {
				layer.mode = GIMP_MODE_TO_TETRA[mode];
				if (mode <= GIMP_MODE_LAST_LEGACY && mode != GIMP_MODE_NORMAL_LEGACY && mode != GIMP_MODE_DISSOLVE) {
					legacy_layers.push_back(entry.name);
				}
			}
			if (entry.custom_compositing) {
				custom_compositing_layers.push_back(entry.name);
			}

			layer.pixels = entry.pixels;
			entry.pixels = Vector<float>();
			if (!source.masks.is_empty()) {
				float *pixels = layer.pixels.ptrw();
				for (int group : source.masks) {
					const float *values = entries[group].mask.ptr();
					for (uint64_t k = 0; k < count; k++) {
						pixels[k * 4 + 3] *= values[k];
					}
				}
			}
			if (entry.active && !entry.is_floating && r_image.active_layer < 0) {
				r_image.active_layer = target;
			}
		}
		// Several layers can be selected in GIMP 3: the topmost one became active.
		if (r_image.active_layer < 0) {
			r_image.active_layer = int(r_image.layers.size()) - 1;
		}

		// An all-zero selection channel is no selection.
		if (have_selection) {
			bool any = false;
			const float *values = r_image.selection.ptr();
			for (uint64_t k = 0; k < count && !any; k++) {
				any = values[k] > 0.0f;
			}
			if (!any) {
				r_image.selection = Vector<float>();
			}
		}

		collect_warnings(r_image.warnings);
		return OK;
	}
};

} // namespace

Error tetra_xcf_read(const Ref<FileAccess> &p_file, TetraXcfImage &r_image) {
	r_image = TetraXcfImage();
	ERR_FAIL_COND_V(p_file.is_null(), ERR_INVALID_PARAMETER);
	XcfReader *reader = memnew(XcfReader);
	reader->file = p_file;
	const Error err = reader->run(r_image);
	memdelete(reader);
	if (err != OK) {
		const String error = r_image.error;
		r_image = TetraXcfImage();
		r_image.error = error;
	}
	return err;
}
