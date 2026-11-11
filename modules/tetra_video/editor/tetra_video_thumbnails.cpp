#include "tetra_video_thumbnails.h"

#include "../mlt/tetra_mlt_engine.h"
#include "../tetra_video_project.h"

#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/image_texture.h"

void TetraVideoThumbnails::_thread_main(void *p_self) {
	static_cast<TetraVideoThumbnails *>(p_self)->_run();
}

void TetraVideoThumbnails::_run() {
	while (true) {
		semaphore.wait();
		if (quit.is_set()) {
			break;
		}
		Request request;
		bool has_request = false;
		Vector2i next_size;
		Vector2i next_fps;
		{
			MutexLock lock(mutex);
			next_size = pending_profile_size;
			next_fps = pending_profile_fps;
			pending_profile_size = Vector2i();
			if (!queue.is_empty()) {
				request = queue.front()->get();
				queue.pop_front();
				has_request = true;
			}
		}
		if (next_size.x > 0) {
			// The engine takes its thumbnail profile from the project it is given; an empty one carries just that.
			Ref<TetraVideoProject> carrier;
			carrier.instantiate();
			carrier->set_profile(next_size.x, next_size.y, next_fps.x, next_fps.y, 48000);
			engine->set_project(carrier);
		}
		if (has_request) {
			const Ref<Image> image = engine->get_thumbnail(request.bin_clip, request.frame, request.size);
			callable_mp(this, &TetraVideoThumbnails::_deliver).call_deferred(request.key, image);
		}
	}
}

void TetraVideoThumbnails::_deliver(const String &p_key, const Ref<Image> &p_image) {
	if (!requested.has(p_key)) {
		// Asked for before the last clear().
		return;
	}
	if (p_image.is_valid() && !p_image->is_empty()) {
		cache[p_key] = ImageTexture::create_from_image(p_image);
	} else {
		// Remembered as "no picture", so that a broken file is not asked for on every redraw.
		cache[p_key] = Ref<ImageTexture>();
	}
	emit_signal(SNAME("thumbnail_ready"));
}

String TetraVideoThumbnails::_make_key(const Dictionary &p_bin_clip, int p_frame, const Vector2i &p_size) {
	return vformat("%d|%s|%d|%d|%dx%d", int(p_bin_clip.get("kind", 0)), String(p_bin_clip.get("source", String())),
			Dictionary(p_bin_clip.get("properties", Dictionary())).hash(), p_frame, p_size.x, p_size.y);
}

Ref<Texture2D> TetraVideoThumbnails::get_thumbnail(const Dictionary &p_bin_clip, int p_frame, const Vector2i &p_size) {
	if (p_bin_clip.is_empty() || p_size.x <= 0 || p_size.y <= 0 || !bool(p_bin_clip.get("has_video", true))) {
		return Ref<Texture2D>();
	}
	const String key = _make_key(p_bin_clip, p_frame, p_size);
	const HashMap<String, Ref<ImageTexture>>::ConstIterator found = cache.find(key);
	if (found) {
		return found->value;
	}
	if (!requested.has(key) && TetraMltEngine::is_available()) {
		requested.insert(key);
		Request request;
		request.key = key;
		request.bin_clip = p_bin_clip.duplicate(true);
		request.frame = p_frame;
		request.size = p_size;
		{
			MutexLock lock(mutex);
			queue.push_back(request);
		}
		semaphore.post();
	}
	return Ref<Texture2D>();
}

bool TetraVideoThumbnails::has_thumbnail(const Dictionary &p_bin_clip, int p_frame, const Vector2i &p_size) const {
	const HashMap<String, Ref<ImageTexture>>::ConstIterator found = cache.find(_make_key(p_bin_clip, p_frame, p_size));
	return found && found->value.is_valid();
}

void TetraVideoThumbnails::set_profile(int p_width, int p_height, int p_fps_num, int p_fps_den) {
	const Vector2i size(p_width, p_height);
	const Vector2i fps(p_fps_num, p_fps_den);
	if (size == profile_size && fps == profile_fps) {
		return;
	}
	profile_size = size;
	profile_fps = fps;
	clear();
	{
		MutexLock lock(mutex);
		pending_profile_size = size;
		pending_profile_fps = fps;
	}
	semaphore.post();
}

void TetraVideoThumbnails::clear() {
	{
		MutexLock lock(mutex);
		queue.clear();
	}
	cache.clear();
	requested.clear();
}

int TetraVideoThumbnails::get_pending_count() const {
	MutexLock lock(mutex);
	return queue.size();
}

void TetraVideoThumbnails::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_thumbnail", "bin_clip", "frame", "size"), &TetraVideoThumbnails::get_thumbnail);
	ClassDB::bind_method(D_METHOD("has_thumbnail", "bin_clip", "frame", "size"), &TetraVideoThumbnails::has_thumbnail);
	ClassDB::bind_method(D_METHOD("set_profile", "width", "height", "fps_num", "fps_den"), &TetraVideoThumbnails::set_profile);
	ClassDB::bind_method(D_METHOD("clear"), &TetraVideoThumbnails::clear);
	ClassDB::bind_method(D_METHOD("get_pending_count"), &TetraVideoThumbnails::get_pending_count);

	ADD_SIGNAL(MethodInfo("thumbnail_ready"));
}

TetraVideoThumbnails::TetraVideoThumbnails() {
	engine.instantiate();
	thread.start(_thread_main, this);
}

TetraVideoThumbnails::~TetraVideoThumbnails() {
	quit.set();
	semaphore.post();
	thread.wait_to_finish();
}
