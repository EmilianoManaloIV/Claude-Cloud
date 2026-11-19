#include "tetra_video_file.h"

#include "core/io/file_access.h"

Vector<String> TetraVideoFileLoader::get_media_extensions() {
	// Formats Godot itself does not load. Ogg Theora (.ogv) and the audio formats stay with Godot's own loaders.
	return { "mp4", "m4v", "mov", "mkv", "webm", "avi", "mpg", "mpeg" };
}

bool TetraVideoFileLoader::is_project_extension(const String &p_extension) {
	return p_extension.to_lower() == "mlt";
}

bool TetraVideoFileLoader::is_media_extension(const String &p_extension) {
	return get_media_extensions().has(p_extension.to_lower());
}

Ref<Resource> TetraVideoFileLoader::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	if (!FileAccess::exists(p_path)) {
		if (r_error) {
			*r_error = ERR_FILE_NOT_FOUND;
		}
		return Ref<Resource>();
	}
	Ref<Resource> resource;
	if (is_project_extension(p_path.get_extension())) {
		resource = Ref<TetraVideoProjectFile>(memnew(TetraVideoProjectFile));
	} else {
		resource = Ref<TetraVideoMediaFile>(memnew(TetraVideoMediaFile));
	}
	if (r_error) {
		*r_error = OK;
	}
	return resource;
}

void TetraVideoFileLoader::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("mlt");
	for (const String &extension : get_media_extensions()) {
		p_extensions->push_back(extension);
	}
}

bool TetraVideoFileLoader::handles_type(const String &p_type) const {
	return p_type == "TetraVideoProjectFile" || p_type == "TetraVideoMediaFile";
}

String TetraVideoFileLoader::get_resource_type(const String &p_path) const {
	const String extension = p_path.get_extension();
	if (is_project_extension(extension)) {
		return "TetraVideoProjectFile";
	}
	if (is_media_extension(extension)) {
		return "TetraVideoMediaFile";
	}
	return String();
}
