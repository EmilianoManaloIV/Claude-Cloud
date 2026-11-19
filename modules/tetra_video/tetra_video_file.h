#pragma once

#include "core/io/resource.h"
#include "core/io/resource_loader.h"

// A video file of the project as the editor's file system knows it. Godot has
// no resource type for MLT project files (.mlt) or for the video formats MLT
// reads and writes (mp4, mov, mkv, webm, ...), so without these two classes
// such files would not be listed in the FileSystem dock at all. The resources
// carry nothing but their path: the Video workspace opens a project file as
// its document and adds a media file to its bin.
class TetraVideoProjectFile : public Resource {
	GDCLASS(TetraVideoProjectFile, Resource);

protected:
	static void _bind_methods() {}
};

class TetraVideoMediaFile : public Resource {
	GDCLASS(TetraVideoMediaFile, Resource);

protected:
	static void _bind_methods() {}
};

class TetraVideoFileLoader : public ResourceFormatLoader {
	GDSOFTCLASS(TetraVideoFileLoader, ResourceFormatLoader);

public:
	// Extensions (lower case, without the dot) loaded as TetraVideoMediaFile.
	static Vector<String> get_media_extensions();
	static bool is_project_extension(const String &p_extension);
	static bool is_media_extension(const String &p_extension);

	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};
