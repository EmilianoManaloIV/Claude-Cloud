#include "tetra_image_document.h"

#include "tetra_ora_loader.h"
#include "tetra_paint_utils.h"

#include "core/io/file_access.h"
#include "core/io/xml_parser.h"
#include "core/io/zip_io.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

#include <cstring>

// Reads one member of an open zip archive. Returns false if it is missing.
static bool zip_read_member(unzFile p_zip, const String &p_name, Vector<uint8_t> &r_data) {
	if (unzLocateFile(p_zip, p_name.utf8().get_data(), 1) != UNZ_OK) {
		return false;
	}
	unz_file_info info;
	if (unzGetCurrentFileInfo(p_zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK || unzOpenCurrentFile(p_zip) != UNZ_OK) {
		return false;
	}
	r_data.resize(info.uncompressed_size);
	const int read = info.uncompressed_size == 0 ? 0 : unzReadCurrentFile(p_zip, r_data.ptrw(), r_data.size());
	unzCloseCurrentFile(p_zip);
	return read == int(info.uncompressed_size);
}

static bool zip_write_member(zipFile p_zip, const String &p_name, const Vector<uint8_t> &p_data, bool p_compress) {
	const OS::DateTime now = OS::get_singleton()->get_datetime();
	zip_fileinfo info;
	memset(&info, 0, sizeof(info));
	info.tmz_date.tm_year = now.year;
	info.tmz_date.tm_mon = int(now.month) - 1;
	info.tmz_date.tm_mday = now.day;
	info.tmz_date.tm_hour = now.hour;
	info.tmz_date.tm_min = now.minute;
	info.tmz_date.tm_sec = now.second;
	if (zipOpenNewFileInZip(p_zip, p_name.utf8().get_data(), &info, nullptr, 0, nullptr, 0, nullptr, p_compress ? Z_DEFLATED : 0, p_compress ? Z_DEFAULT_COMPRESSION : 0) != ZIP_OK) {
		return false;
	}
	const bool written = p_data.is_empty() || zipWriteInFileInZip(p_zip, p_data.ptr(), p_data.size()) == ZIP_OK;
	return zipCloseFileInZip(p_zip) == ZIP_OK && written;
}

struct OraLayer {
	String name;
	String source;
	int x = 0;
	int y = 0;
	float opacity = 1.0f;
	bool visible = true;
	bool selected = false;
	bool lock_alpha = false;
	String composite_op;
};

// Parses stack.xml. Layers come back top to bottom, as the file lists them.
// Nested stacks (groups) are flattened: their visibility and opacity are
// folded into the layers they contain.
static Error parse_ora_stack(const Vector<uint8_t> &p_xml, int &r_width, int &r_height, LocalVector<OraLayer> &r_layers) {
	Ref<XMLParser> parser;
	parser.instantiate();
	Error err = parser->open_buffer(p_xml);
	if (err != OK) {
		return err;
	}
	struct Group {
		bool visible = true;
		float opacity = 1.0f;
	};
	LocalVector<Group> groups;
	r_width = 0;
	r_height = 0;
	while (parser->read() == OK) {
		const XMLParser::NodeType type = parser->get_node_type();
		const String node = parser->get_node_name();
		if (type == XMLParser::NODE_ELEMENT_END && node == "stack") {
			if (!groups.is_empty()) {
				groups.resize(groups.size() - 1);
			}
			continue;
		}
		if (type != XMLParser::NODE_ELEMENT) {
			continue;
		}
		const bool visible = parser->get_named_attribute_value_safe("visibility") != "hidden";
		const float opacity = parser->has_attribute("opacity") ? CLAMP(float(parser->get_named_attribute_value("opacity").to_float()), 0.0f, 1.0f) : 1.0f;
		const Group parent = groups.is_empty() ? Group() : groups[groups.size() - 1];
		if (node == "image") {
			r_width = parser->get_named_attribute_value_safe("w").to_int();
			r_height = parser->get_named_attribute_value_safe("h").to_int();
		} else if (node == "stack") {
			if (!parser->is_empty()) {
				Group group;
				group.visible = parent.visible && visible;
				group.opacity = parent.opacity * opacity;
				groups.push_back(group);
			}
		} else if (node == "layer") {
			OraLayer layer;
			layer.name = parser->get_named_attribute_value_safe("name").xml_unescape();
			layer.source = parser->get_named_attribute_value_safe("src");
			layer.x = parser->get_named_attribute_value_safe("x").to_int();
			layer.y = parser->get_named_attribute_value_safe("y").to_int();
			layer.opacity = parent.opacity * opacity;
			layer.visible = parent.visible && visible;
			layer.selected = parser->get_named_attribute_value_safe("selected") == "true";
			layer.lock_alpha = parser->get_named_attribute_value_safe("tetra-lock-alpha") == "true";
			layer.composite_op = parser->get_named_attribute_value_safe("composite-op");
			r_layers.push_back(layer);
		}
	}
	return (r_width > 0 && r_height > 0) ? OK : ERR_FILE_CORRUPT;
}

Error TetraImageDocument::_load_ora(const String &p_path) {
	Ref<FileAccess> file;
	zlib_filefunc_def io = zipio_create_io(&file);
	unzFile zip = unzOpen2(p_path.utf8().get_data(), &io);
	ERR_FAIL_NULL_V_MSG(zip, ERR_FILE_CANT_OPEN, vformat("Cannot open \"%s\" as an OpenRaster archive.", p_path));

	Vector<uint8_t> xml;
	int new_width = 0, new_height = 0;
	LocalVector<OraLayer> entries;
	Error err = zip_read_member(zip, "stack.xml", xml) ? parse_ora_stack(xml, new_width, new_height, entries) : ERR_FILE_CORRUPT;
	if (err == OK && (new_width > 16384 || new_height > 16384)) {
		err = ERR_FILE_CORRUPT;
	}

	// Everything is decoded before the document is touched, so a broken
	// archive leaves it as it was.
	LocalVector<Layer> loaded;
	int selected = -1;
	for (int i = int(entries.size()) - 1; err == OK && i >= 0; i--) {
		const OraLayer &entry = entries[i];
		Vector<uint8_t> png;
		Ref<Image> image;
		image.instantiate();
		if (!zip_read_member(zip, entry.source, png) || image->load_png_from_buffer(png) != OK) {
			err = ERR_FILE_CORRUPT;
			break;
		}
		// PNG data is sRGB-encoded whatever its bit depth.
		const Vector<float> source = _image_to_linear(image, true);
		const int source_width = image->get_width();
		Layer layer;
		layer.name = entry.name;
		layer.visible = entry.visible;
		layer.opacity = entry.opacity;
		layer.lock_alpha = entry.lock_alpha;
		layer.mode = LayerMode(TetraLayerModes::mode_from_ora_op(entry.composite_op.utf8().get_data()));
		layer.pixels.resize_initialized(int64_t(new_width) * new_height * 4);
		const Rect2i placed = Rect2i(entry.x, entry.y, source_width, image->get_height()).intersection(Rect2i(0, 0, new_width, new_height));
		float *dst = layer.pixels.ptrw();
		for (int y = placed.position.y; y < placed.position.y + placed.size.y; y++) {
			memcpy(dst + (size_t(y) * new_width + placed.position.x) * 4,
					source.ptr() + (size_t(y - entry.y) * source_width + (placed.position.x - entry.x)) * 4,
					sizeof(float) * 4 * placed.size.x);
		}
		if (entry.selected) {
			selected = int(loaded.size());
		}
		loaded.push_back(layer);
	}
	unzClose(zip);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("\"%s\" is not a readable OpenRaster file.", p_path));

	_reset(new_width, new_height);
	for (Layer &layer : loaded) {
		layer.id = next_layer_id++;
		layers.push_back(layer);
	}
	if (!layers.is_empty()) {
		active_layer = layers[selected >= 0 ? selected : int(layers.size()) - 1].id;
	}
	return OK;
}

Error TetraImageDocument::load(const String &p_path) {
	TETRA_REQUIRE_IDLE_V(ERR_BUSY);
	if (p_path.get_extension().to_lower() == "ora") {
		const Error err = _load_ora(p_path);
		if (err != OK) {
			return err;
		}
	} else {
		Ref<Image> image;
		image.instantiate();
		const Error err = ImageLoader::load_image(p_path, image);
		if (err != OK) {
			return err;
		}
		const Vector<float> pixels = _image_to_linear(image);
		ERR_FAIL_COND_V(pixels.is_empty(), ERR_FILE_CORRUPT);
		_reset(image->get_width(), image->get_height());
		active_layer = _append_layer(p_path.get_file().get_basename(), pixels);
	}
	_clear_history();
	emit_signal(SNAME("size_changed"));
	emit_signal(SNAME("layers_changed"));
	emit_signal(SNAME("selection_changed"));
	_mark_dirty(_canvas_rect());
	return OK;
}

// Encodes a rectangle of linear float pixels as an 8-bit sRGB PNG.
static Vector<uint8_t> encode_png(const float *p_pixels, int p_stride_width, const Rect2i &p_rect) {
	Vector<uint8_t> bytes;
	bytes.resize(int64_t(p_rect.size.x) * p_rect.size.y * 4);
	uint8_t *dst = bytes.ptrw();
	tetra_parallel_rows(p_rect.size.y, p_rect.size.x, [&](int p_first, int p_end) {
		for (int y = p_first; y < p_end; y++) {
			const float *src = p_pixels + (size_t(p_rect.position.y + y) * p_stride_width + p_rect.position.x) * 4;
			uint8_t *out = dst + size_t(y) * p_rect.size.x * 4;
			for (int x = 0; x < p_rect.size.x; x++) {
				out[x * 4 + 0] = tetra_linear_to_srgb8(src[x * 4 + 0]);
				out[x * 4 + 1] = tetra_linear_to_srgb8(src[x * 4 + 1]);
				out[x * 4 + 2] = tetra_linear_to_srgb8(src[x * 4 + 2]);
				out[x * 4 + 3] = uint8_t(CLAMP(src[x * 4 + 3], 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		}
	});
	return Image::create_from_data(p_rect.size.x, p_rect.size.y, false, Image::FORMAT_RGBA8, bytes)->save_png_to_buffer();
}

Error TetraImageDocument::save_ora(const String &p_path) const {
	ERR_FAIL_COND_V(width == 0, ERR_UNCONFIGURED);
	Ref<FileAccess> file;
	zlib_filefunc_def io = zipio_create_io(&file);
	zipFile zip = zipOpen2(p_path.utf8().get_data(), APPEND_STATUS_CREATE, nullptr, &io);
	ERR_FAIL_NULL_V_MSG(zip, ERR_FILE_CANT_WRITE, vformat("Cannot create \"%s\".", p_path));

	// The OpenRaster specification wants "mimetype" first and uncompressed.
	bool ok = zip_write_member(zip, "mimetype", String("image/openraster").to_utf8_buffer(), false);

	String xml = "<?xml version='1.0' encoding='UTF-8'?>\n";
	xml += vformat("<image version=\"0.0.3\" w=\"%d\" h=\"%d\" xres=\"72\" yres=\"72\">\n  <stack>\n", width, height);
	for (int i = int(layers.size()) - 1; i >= 0; i--) {
		const Layer &layer = layers[i];
		// Each layer is stored cropped to its content, with its position.
		const float *pixels = layer.pixels.ptr();
		int left = width, right = -1, top = height, bottom = -1;
		for (int y = 0; y < height; y++) {
			const float *row = pixels + size_t(y) * width * 4;
			for (int x = 0; x < width; x++) {
				if (row[x * 4 + 3] > 0.0f) {
					left = MIN(left, x);
					right = MAX(right, x);
					top = MIN(top, y);
					bottom = y;
				}
			}
		}
		const Rect2i bounds = right < 0 ? Rect2i(0, 0, 1, 1) : Rect2i(left, top, right - left + 1, bottom - top + 1);
		const String source = vformat("data/layer%03d.png", i);
		ok = ok && zip_write_member(zip, source, encode_png(pixels, width, bounds), false);
		xml += vformat("    <layer name=\"%s\" src=\"%s\" x=\"%d\" y=\"%d\" opacity=\"%s\" visibility=\"%s\" composite-op=\"%s\"%s%s/>\n",
				layer.name.xml_escape(true), source, bounds.position.x, bounds.position.y, String::num(layer.opacity, 6),
				layer.visible ? "visible" : "hidden", TetraLayerModes::get_info(layer.mode).ora_op,
				layer.id == active_layer ? " selected=\"true\"" : "", layer.lock_alpha ? " tetra-lock-alpha=\"true\"" : "");
	}
	xml += "  </stack>\n</image>\n";
	ok = ok && zip_write_member(zip, "stack.xml", xml.to_utf8_buffer(), true);

	const Ref<Image> merged = composite();
	ok = ok && zip_write_member(zip, "mergedimage.png", merged->save_png_to_buffer(), false);
	Ref<Image> thumbnail = merged->duplicate();
	const int longest = MAX(width, height);
	if (longest > 256) {
		thumbnail->resize(MAX(width * 256 / longest, 1), MAX(height * 256 / longest, 1), Image::INTERPOLATE_BILINEAR);
	}
	ok = ok && zip_write_member(zip, "Thumbnails/thumbnail.png", thumbnail->save_png_to_buffer(), false);

	ok = zipClose(zip, nullptr) == ZIP_OK && ok;
	return ok ? OK : ERR_FILE_CANT_WRITE;
}

Error TetraImageDocument::export_image(const String &p_path, float p_quality) const {
	ERR_FAIL_COND_V(width == 0, ERR_UNCONFIGURED);
	const String extension = p_path.get_extension().to_lower();
	const float quality = CLAMP(p_quality, 0.0f, 1.0f);
	if (extension == "png") {
		return composite()->save_png(p_path);
	}
	if (extension == "webp") {
		return composite()->save_webp(p_path, quality < 1.0f, quality);
	}
	if (extension == "exr") {
		return composite_float()->save_exr(p_path, false);
	}
	if (extension == "jpg" || extension == "jpeg") {
		// JPEG has no alpha: the image is flattened onto white.
		Vector<float> merged;
		merged.resize(int64_t(width) * height * 4);
		_composite_into(_canvas_rect(), merged.ptrw());
		Vector<uint8_t> bytes;
		bytes.resize(int64_t(width) * height * 3);
		uint8_t *dst = bytes.ptrw();
		const float *src = merged.ptr();
		for (int64_t i = 0; i < int64_t(width) * height; i++) {
			const float alpha = CLAMP(src[i * 4 + 3], 0.0f, 1.0f);
			for (int c = 0; c < 3; c++) {
				dst[i * 3 + c] = tetra_linear_to_srgb8(src[i * 4 + c] * alpha + (1.0f - alpha));
			}
		}
		return Image::create_from_data(width, height, false, Image::FORMAT_RGB8, bytes)->save_jpg(p_path, quality);
	}
	ERR_FAIL_V_MSG(ERR_FILE_UNRECOGNIZED, vformat("Cannot export to \".%s\"; use png, jpg, webp or exr.", extension));
}

Error TetraImageLoaderORA::load_image(Ref<Image> p_image, Ref<FileAccess> p_fileaccess, BitField<ImageFormatLoader::LoaderFlags> p_flags, float p_scale) {
	ERR_FAIL_COND_V(p_image.is_null() || p_fileaccess.is_null(), ERR_INVALID_PARAMETER);
	const String path = p_fileaccess->get_path();
	Ref<FileAccess> file;
	zlib_filefunc_def io = zipio_create_io(&file);
	unzFile zip = unzOpen2(path.utf8().get_data(), &io);
	ERR_FAIL_NULL_V(zip, ERR_FILE_CANT_OPEN);
	Vector<uint8_t> png;
	const bool has_merged = zip_read_member(zip, "mergedimage.png", png);
	unzClose(zip);
	if (has_merged && p_image->load_png_from_buffer(png) == OK) {
		return OK;
	}
	Ref<TetraImageDocument> document;
	document.instantiate();
	const Error err = document->load(path);
	if (err != OK) {
		return err;
	}
	p_image->copy_internals_from(document->composite());
	return OK;
}

void TetraImageLoaderORA::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("ora");
}

void TetraImageDocument::_bind_file_methods() {
	ClassDB::bind_method(D_METHOD("load", "path"), &TetraImageDocument::load);
	ClassDB::bind_method(D_METHOD("save_ora", "path"), &TetraImageDocument::save_ora);
	ClassDB::bind_method(D_METHOD("export_image", "path", "quality"), &TetraImageDocument::export_image, DEFVAL(0.9f));
}
