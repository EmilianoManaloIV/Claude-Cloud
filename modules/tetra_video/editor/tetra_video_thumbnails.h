#pragma once

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

class Image;
class ImageTexture;
class TetraMltEngine;
class Texture2D;

// Pictures of bin clips for the bin list and the timeline. A thumbnail is
// asked for with get_thumbnail(), which answers from the cache or returns null
// and has the picture made on a worker thread (an MLT engine of its own, so
// playback is never held up); thumbnail_ready then tells the views to redraw.
class TetraVideoThumbnails : public RefCounted {
	GDCLASS(TetraVideoThumbnails, RefCounted);

	struct Request {
		String key;
		Dictionary bin_clip;
		int frame = 0;
		Vector2i size;
	};

	Ref<TetraMltEngine> engine;
	Thread thread;
	Mutex mutex;
	Semaphore semaphore;
	SafeFlag quit;
	List<Request> queue;
	// Profile the worker's engine has to adopt before its next picture; zero width when there is none.
	Vector2i pending_profile_size;
	Vector2i pending_profile_fps;

	HashMap<String, Ref<ImageTexture>> cache;
	HashSet<String> requested;
	Vector2i profile_size = Vector2i(1920, 1080);
	Vector2i profile_fps = Vector2i(25, 1);

	static void _thread_main(void *p_self);
	void _run();
	void _deliver(const String &p_key, const Ref<Image> &p_image);
	static String _make_key(const Dictionary &p_bin_clip, int p_frame, const Vector2i &p_size);

protected:
	static void _bind_methods();

public:
	// p_bin_clip is a TetraVideoProject.get_bin_clip() dictionary, p_frame a frame of that clip.
	Ref<Texture2D> get_thumbnail(const Dictionary &p_bin_clip, int p_frame, const Vector2i &p_size);
	bool has_thumbnail(const Dictionary &p_bin_clip, int p_frame, const Vector2i &p_size) const;
	// Frame numbers are counted in this profile. Changing it drops every picture.
	void set_profile(int p_width, int p_height, int p_fps_num, int p_fps_den);
	void clear();
	int get_pending_count() const;

	TetraVideoThumbnails();
	~TetraVideoThumbnails();
};
