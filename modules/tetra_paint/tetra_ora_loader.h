#pragma once

#include "core/io/image_loader.h"

// Lets Godot load OpenRaster (.ora) files as images, so layered files saved
// from the Image workspace import as textures. The flattened picture is the
// archive's mergedimage.png; an archive without one is composited from its
// layers.
class TetraImageLoaderORA : public ImageFormatLoader {
public:
	virtual Error load_image(Ref<Image> p_image, Ref<FileAccess> p_fileaccess, BitField<ImageFormatLoader::LoaderFlags> p_flags = FLAG_NONE, float p_scale = 1.0) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
};

// Lets Godot load GIMP's XCF files as images (the visible layers composited),
// so .xcf files in a project import as textures.
class TetraImageLoaderXCF : public ImageFormatLoader {
public:
	virtual Error load_image(Ref<Image> p_image, Ref<FileAccess> p_fileaccess, BitField<ImageFormatLoader::LoaderFlags> p_flags = FLAG_NONE, float p_scale = 1.0) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
};
