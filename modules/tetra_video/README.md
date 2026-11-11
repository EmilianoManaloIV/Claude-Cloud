# tetra_video — the Video engine

Tetra's Kdenlive: a non-linear video editor whose playback and rendering
engine is MLT. This module is the engine layer; the workspace UI is built on
the four classes below. All of them are registered in ClassDB, so everything
here can be driven from GDScript (the tests in
`tests/godot/project/tetra_video/` do exactly that).

| Class | Role |
| --- | --- |
| `TetraVideoProject` (RefCounted) | The timeline document: profile, bin, tracks, clips, effects, markers, undo, MLT XML. No MLT needed. |
| `TetraMltEngine` (RefCounted) | MLT itself: service lists and metadata, media probing, frames of a project, real-time playback with sound. |
| `TetraVideoAssets` (static) | Kdenlive's effect/transition catalog as operation schemas; conversion of panel values to MLT properties. |
| `TetraVideoRenderer` (RefCounted) | Rendering to a file through `melt`, with Kdenlive's encoding presets. |

Source layout: `tetra_video_project.{h,cpp}` (model), `tetra_video_project_xml.cpp`
(MLT XML writer and loader), `tetra_video_xml.{h,cpp}` (small XML tree),
`tetra_video_assets.{h,cpp}`, `tetra_video_renderer.{h,cpp}`,
`mlt/tetra_mlt_engine.{h,cpp}`.

## Build

`SCsub` asks `pkg-config` for `mlt++-7`. When it is found the module is
compiled with `TETRA_MLT_ENABLED` and linked against `libmlt++-7`/`libmlt-7`;
otherwise the same classes are built, `TetraMltEngine.is_available()` is
false and every engine call fails cleanly (`ERR_UNAVAILABLE`, `null`, empty).
The model, the XML and the catalog parsing work without MLT.

Run-time lookups:

| What | Where it is looked for |
| --- | --- |
| Kdenlive data (effects, transitions, `profiles.xml`) | project setting `tetra/video/kdenlive_data_dir`, env `TETRA_KDENLIVE_DATA`, `$TETRA_REPO_ROOT/upstream/kdenlive/data`, `<editor binary>/../../kdenlive/data`, `/usr/share/kdenlive` |
| `melt` | env `TETRA_MELT`, then `melt-7` and `melt` on `PATH` |

## Conventions

- Frames are integers in the project's frame rate. A timeline clip has a
  `position` (first timeline frame) and `in`/`out` source frames, both
  inclusive, as in MLT. Its length is `out - in + 1`; its end edge is
  `position + length` (exclusive).
- Tracks are addressed by index, ordered bottom to top. `add_track()` puts new
  video tracks on top and new audio tracks at the bottom, which gives the
  Kdenlive stack `A2 A1 V1 V2`.
- An operation that cannot be done is refused as a whole: it returns `false`
  (or `-1` where an id/index is returned), changes nothing, and
  `get_last_error()` says why.
- Dictionaries handed in or out are copies.

## TetraVideoProject

Enums: `ClipKind { CLIP_VIDEO, CLIP_AUDIO, CLIP_IMAGE, CLIP_COLOR, CLIP_TEXT }`,
`TrackKind { TRACK_VIDEO, TRACK_AUDIO }`.

Signals: `changed()` after every change (including undo/redo and load);
`structure_changed()` additionally when the profile, the bin or the track
list changed. `get_revision() -> int` grows with every change.

### Profile

```
set_profile(width, height, fps_num, fps_den, sample_rate) -> bool      # default 1920x1080, 25/1, 48000
set_profile_preset(name) -> bool          static get_profile_presets() -> PackedStringArray
get_width() get_height() get_fps_num() get_fps_den() get_fps() -> float get_sample_rate()
```

Presets: `atsc_1080p_{24,2398,25,2997,30,50,5994,60}`, `atsc_720p_{25,30,50,60}`,
`uhd_2160p_{25,30,50,60}`, `vertical_hd_{30,60}`, `square_1080p_{30,60}`.
Changing the frame rate does not rescale clip positions.

### Bin

```
add_bin_clip(kind, name, source, length, has_video = true, has_audio = false, properties = {}) -> int   # id, or -1
add_bin_color(name, color, length = 125) -> int
add_bin_text(name, text, length = 125, properties = {}) -> int
remove_bin_clip(id, cascade = false) -> bool     # refused while used, unless cascade (then its clips go too)
set_bin_clip_name(id, name) / set_bin_clip_source(id, source) / set_bin_clip_properties(id, properties) -> bool
has_bin_clip(id) -> bool   get_bin_clip_ids() -> PackedInt32Array   get_bin_clip_use_count(id) -> int
get_bin_clip(id) -> { id, kind, name, source, length, has_video, has_audio, properties }
```

`source` is a file path (absolute, `res://` or `user://`) for media and
images, `"#rrggbbaa"` for colours and the text for titles. `properties` are
extra MLT producer properties (for a title: `family`, `size`, `weight`,
`fgcolour`, `bgcolour`, `align`, `pad`, ... of MLT's `pango` producer). Media
clips cannot be trimmed beyond `length`; images, colours and titles extend
freely. Use `TetraMltEngine.import_media(project, path)` to add a file with
probed length and streams.

### Tracks

```
add_track(kind, name = "", index = -1) -> int      # index of the new track
remove_track(index) -> bool                         # with its clips; refused when locked
move_track(from, to) -> bool
set_track_name(index, name) / set_track_hidden(index, bool) / set_track_muted(index, bool) / set_track_locked(index, bool) -> bool
get_track_count() -> int     get_track(index) -> { kind, name, hidden, muted, locked }
get_track_kind(index) get_track_name(index) is_track_hidden(index) is_track_muted(index) is_track_locked(index)
get_paired_track(index) -> int      # Kdenlive's mirror track: n-th video from the bottom <-> n-th audio from the top; -1 if none
```

A video track only takes clips with video, an audio track only clips with
audio. As in Kdenlive, video tracks never play sound and audio tracks never
show pictures; `hidden` blanks a video track, `muted` silences an audio
track. A locked track rejects every edit of its clips.

### Clips

```
insert_clip(bin_id, track, position, in = -1, out = -1, with_linked = true) -> int    # clip id, or -1
move_clip(id, track, position) -> bool
trim_clip_start(id, frame) -> bool      # frame = new first timeline frame
trim_clip_end(id, frame) -> bool        # frame = new end edge (exclusive)
split_clip(id, frame) -> int            # id of the new right part, or -1
split_all_at(frame) -> PackedInt32Array # ids of the new right parts (unlocked tracks)
delete_clip(id) -> bool                 delete_clips(ids) -> bool
ripple_delete(id) -> bool               # delete and close the gap on that track (and on the partner's track)
lift_range(from, to, tracks = []) -> bool       # remove [from, to) leaving a gap; [] = every unlocked track
extract_range(from, to, tracks = []) -> bool    # remove [from, to) and close up
insert_space(position, length, track = -1) -> bool   # push clips starting at/after position; -1 = every unlocked track
remove_space(position, track = -1) -> int            # close the gap around position; returns frames removed (0 = refused)
duplicate_clip(id, track = -1, position = -1) -> int # default: same track, right after the original
set_clip_fades(id, fade_in, fade_out) -> bool        # frames, clamped to the clip
link_clips(a, b) -> bool    unlink_clip(id) -> bool    get_linked_clip(id) -> int   # -1 if unlinked
```

Rules, as in Kdenlive: clips on one track never overlap; anything that would
overlap is refused. Trims are clamped to the neighbours and, for media, to
the source; they return `false` when nothing changed. A split keeps the id,
effects and fade-in on the left part and gives the right part a new id, the
effects and the fade-out.

AV clips: inserting a bin clip with both video and audio also inserts the
other half on the paired track and links the two (one undo step). When the
paired track exists but is locked or occupied the insert is refused; when
there is no paired track only one half is inserted. `with_linked = false`
inserts one half only. Linked clips move (to the mirror track when the track
changes), trim, split and are deleted together; if the partner cannot follow
the operation is refused. Range and space operations act per track and leave
links in place.

### Effects, markers

```
add_clip_effect(clip_id, operation_id, service, values = {}, index = -1) -> int   # index, or -1
remove_clip_effect(clip_id, index) / move_clip_effect(clip_id, from, to) -> bool
set_clip_effect_enabled(clip_id, index, enabled) -> bool
set_clip_effect_values(clip_id, index, values) / set_clip_effect_value(clip_id, index, name, value) -> bool
get_clip_effect_count(clip_id) -> int
get_clip_effects(clip_id) -> Array of { operation, service, values, enabled }

add_marker(frame, text = "") -> bool      # one per frame; adding again retitles
remove_marker(frame) / move_marker(from, to) -> bool      has_marker(frame) -> bool
get_markers() -> Array of { frame, text }  # sorted
```

`values` is exactly what a `TetraParamPanel` produces for the operation: the
*display* values of the schema. The XML writer converts them with
`TetraVideoAssets` (factor, colour notation, switches). An empty
`operation_id` means `"mlt:<service>"`.

### Queries

```
has_clip(id) -> bool    get_clip_ids() -> PackedInt32Array (sorted)
get_clip(id) -> { id, bin_id, track, position, in, out, length, link, fade_in, fade_out, effects }
get_clip_at(track, frame) -> int (-1)        get_clips_in_track(track) -> PackedInt32Array (by position)
get_duration() -> int                         # last frame + 1
get_snap_points(ignore_ids = []) -> PackedInt32Array   # 0, clip edges, markers; sorted, unique
find_snap(frame, tolerance, ignore_ids = []) -> int    # nearest snap point within tolerance, or -1
is_range_free(track, from, to, ignore_ids = []) -> bool   # [from, to)
check_consistency() -> String                 # "" when the document is sound
get_state() -> Dictionary                     # the whole document, for comparisons
get_last_error() -> String
```

### Undo

Every operation is one undo step with a name (`"Insert Clip"`, `"Trim Clip
Start"`, ...). The stack stores the document before and after each step, so
undo and redo are exact.

```
undo() -> bool   redo() -> bool   can_undo()   can_redo()   get_undo_name()   get_redo_name()   get_undo_count()
begin_undo_group(name) ... end_undo_group()     # everything in between is one step (a drag calling move_clip repeatedly)
clear_undo_history()   set_max_undo_steps(n) (default 200)   get_max_undo_steps()
```

### MLT XML and the project file

```
to_mlt_xml(options = {}) -> String
save(path) -> Error      load(path) -> Error      load_from_xml(xml) -> Error      clear()
static value_to_mlt(value) -> String
```

`to_mlt_xml` writes a complete, deterministic MLT document: profile, a black
background track as long as the project, one producer per timeline clip
(`avformat`, `color`, `pixbuf`/`qimage`, `pango`/`qtext`), one playlist per
track with blanks for gaps, and a tractor with an always-active `mix`
transition for every audio track and an always-active compositing transition
for every video track, all onto the background track (Kdenlive's layout).
Clip effects are filters on the playlist entry, so their animations are
clip-relative; fades are `brightness` (level on the lowest video track, alpha
above it) and `volume` filters with MLT animation strings.

Options: `compositing` (default: first available of `frei0r.cairoblend`,
`qtblend`, `composite`), `image_producer` (`pixbuf`, `qimage`),
`text_producer` (`pango`, `qtext`), `width`/`height` (override the profile
size, used for scaled renders), `tetra_properties` (default true).

The project file (`.mlt`) is this XML. `tetra:*` properties, which MLT
ignores, carry what is needed to reload the exact model: ids, bin, links,
fades, markers, track flags, effect operation ids and display values. melt,
Kdenlive and Shotcut can open the file. `load()` also accepts MLT XML written
by other programs on a best-effort basis (playlists become tracks, entries
become clips, entry filters become `mlt:` effects, Kdenlive's two-playlist
track tractors are merged) and returns `ERR_PARSE_ERROR` (malformed XML),
`ERR_FILE_UNRECOGNIZED` (not MLT) or `ERR_INVALID_DATA` (inconsistent
timeline) with a message in `get_last_error()`. A failed load leaves the
project untouched; a successful one clears the undo history.

## TetraMltEngine

Static:

```
is_available() -> bool                   get_version() -> String            # "7.22.0"
list_services(kind) -> PackedStringArray # kind: "producer" | "filter" | "transition" | "consumer"
has_service(kind, name) -> bool
get_preferred_service(kind, "a,b,c") -> String      # first installed candidate
get_service_operation(kind, name) -> Dictionary     # MLT's metadata as an operation schema, id "mlt:<name>"
probe(path, project = null) -> { kind, service, length, duration, fps, width, height, has_video, has_audio, sample_rate, channels }
import_media(project, path) -> int                  # probe + add_bin_clip; bin id or -1
get_audio_peaks(path, samples_per_second = 50) -> PackedFloat32Array   # 0..1 peak per bucket, whole file
```

`probe` counts `length` in the project's frames when a project is given, in
the file's own frames otherwise; `{}` when the file cannot be opened.
`get_service_operation` adds `service`, `kind`, `has_metadata`, `hidden`;
parameters without a default in the metadata carry `no_default: true` and are
only sent once changed.

Frames:

```
set_project(project) -> Error     set_xml(xml) -> Error      # (re)build the graph; call again after every edit
has_graph() -> bool               get_length() -> int
render_frame(frame, max_size = Vector2i()) -> Image          # RGBA8, on the calling thread
get_thumbnail(source, frame, size) -> Image                  # source: a file path, or a get_bin_clip() dictionary
```

`render_frame` pulls the frame at the profile size, or scaled down to fit
`max_size` with the aspect kept. A refused `set_xml` (`ERR_INVALID_DATA`)
keeps the previous graph. `set_project` is the usual call: connect it to the
project's `changed` signal (coalesce with `call_deferred` when edits come in
bursts).

Playback (engine thread):

```
play()   pause()   seek(frame)   set_speed(speed)   get_speed()   get_position() -> int   is_playing() -> bool
set_loop(bool) get_loop()    set_preview_size(Vector2i) get_preview_size()    set_volume(linear) get_volume()
set_audio_bus(name) get_audio_bus()      # used from the next play(); default "Master"
get_current_frame_image() -> Image       get_current_frame() -> int
get_audio_frames_pushed() -> int         get_dropped_frames() -> int
signal frame_ready(frame)                signal playback_finished()
```

- `frame_ready` is emitted on the main thread (deferred, coalesced) whenever
  a new picture is available; fetch it with `get_current_frame_image()` and
  put it in an `ImageTexture` (`update()` when the size is unchanged).
- `seek()` while paused renders that frame in the background and emits
  `frame_ready`; only the latest of a burst of seeks is rendered, so
  scrubbing never blocks the UI. After `set_project()` the current frame is
  refreshed the same way once the playback thread exists (after the first
  `seek()` or `play()`).
- At speed 1 frames are decoded in order slightly ahead of the clock; the
  sound goes to the AudioServer through an `AudioStreamGenerator` playback
  the engine owns (no `AudioStreamPlayer` node is needed), and pictures are
  shown when their sound is heard. When decoding is too slow pictures are
  dropped, never sound; if that is not enough playback jumps ahead, so
  `get_position()` stays real-time.
- Any other speed (negative = reverse, for J/K/L) shows the frame under the
  clock, silently. `set_speed(0)` pauses.
- Playback stops at the last frame (first frame in reverse) with
  `playback_finished`, unless looping. `play()` at the end starts over.

Threading: every call into MLT, from any instance or thread, is serialised by
one mutex, so all methods may be called from any thread; long ones
(`set_project` on a timeline with many media files, `get_thumbnail`,
`get_audio_peaks`, `probe`) belong on a `WorkerThreadPool` task. Destroying
the engine stops its thread and closes its producers.

## TetraVideoAssets

```
get_data_dir() -> String
get_effects(available_only = true, width = 1920, height = 1080) -> Array[Dictionary]
get_transitions(available_only = true, width = 1920, height = 1080) -> Array[Dictionary]
get_operation(id, width = 1920, height = 1080) -> Dictionary
get_default_values(operation) -> Dictionary
values_to_mlt_properties(operation, values) -> Dictionary          # name -> String, as MLT expects
effect_to_mlt_properties(operation_id, values, width, height, length) -> Dictionary
clear_cache()
```

Each operation follows the shared schema (`id`, `label`, `description`,
`category`, `source`, `params`) and adds `service` (the MLT service), `kind`
(`"video_effect"`, `"audio_effect"`, `"transition"`), `available` and `fixed`
(properties always sent, never shown). Ids are `kdenlive:<id>` for effects
read from Kdenlive's `effects/**/*.xml` and `transitions/*.xml`; MLT filters
that Kdenlive does not describe but that have MLT metadata are appended as
`mlt:<name>`. Categories come from `kdenliveeffectscategory.rc` (fallback
`Audio` / `Misc`; transitions are `Transitions`). `width`/`height` resolve
Kdenlive's `%width`, `%maxWidth`... placeholders; pass the project's size.

Parameter mapping (Kdenlive type → schema type, kept in `kdenlive_type`):

| Kdenlive | Schema | Notes |
| --- | --- | --- |
| constant, double, animated, keyframe, simplekeyframe, integer, float, colorwheel | `int` / `float` | display units: `min`/`max` as written, `default` × `factor`; `factor`, `step`, `unit`; `animated: true` for keyframable ones (edited as a constant for now) |
| bool | `bool` | sent as `1`/`0` |
| switch | `bool` | `switch_on` / `switch_off` hold the strings sent |
| list, listdependency | `enum` | `paramlist` + `paramlistdisplay` |
| color, fixedcolor | `color` | `color_format` (`#` or `0x`) and `alpha` decide the notation sent |
| position | `int` | frames |
| url, urllist | `file` | empty = not sent |
| text | `text` | |
| fixed, hidden | — | in `fixed`, always sent |
| animatedrect, geometry, curve, bezier_spline, roto-spline, wipe, keywords, fontfamily, ... | `string` | `raw: true`, the MLT value as text |
| multiswitch, filterjob, readonly | — | not shown, the service's defaults apply |

## TetraVideoRenderer

```
static get_presets() -> Array of { id, group, name, extension, args, type, builtin, default_quality?, default_bitrate?,
                                   default_audio_bitrate?, default_audio_quality?, speeds, default_speed_index }
static get_preset(id) -> Dictionary        static find_melt() -> String
static expand_preset_arguments(preset, options) -> PackedStringArray

start(project, output_path, preset_id, options = {}) -> Error
cancel()   is_running() -> bool   get_frame() -> int   get_percent() -> float
get_command_line() -> String   get_log() -> String   get_last_error() -> String
signal progress(frame, percent)        signal finished(ok, message)
```

Presets: three built in — `tetra/mp4` (H.264/AAC), `tetra/webm` (VP9/Opus),
`tetra/ogv` (Theora/Vorbis, which Godot's `VideoStreamPlayer` plays) — and
every profile of Kdenlive's `profiles.xml` as `<group id>/<name>`
(`generic/MP4-H264/AAC`, ...).

Options: `in`, `out` (frames, inclusive; default the whole timeline),
`width` + `height` (scaled render), `quality` (or `crf`), `bitrate`,
`audio_bitrate` (kbit/s), `audio_quality`, `speed_index`, `threads`,
`extra_args` (more `name=value` consumer properties).

`start` writes the project's XML to a scratch file and runs
`melt <xml> in=.. out=.. -progress -consumer avformat:<output> <args> real_time=-N`.
It returns `ERR_DOES_NOT_EXIST` (unknown preset), `ERR_FILE_NOT_FOUND` (no
melt), `ERR_INVALID_DATA` (empty timeline), `ERR_BUSY` (already running).
`finished(true, <output path>)` or `finished(false, <reason>)` arrives on the
main thread; a cancelled render removes its partial file. Keep a reference to
the renderer while it runs: dropping it cancels the render. After rendering
into the project folder call `EditorFileSystem.scan()` so the file shows up.

## Typical call sequences

Build a project and show frame N:

```gdscript
var project := TetraVideoProject.new()
project.add_track(TetraVideoProject.TRACK_VIDEO)
project.add_track(TetraVideoProject.TRACK_AUDIO)
var bin_id := TetraMltEngine.import_media(project, "res://clips/take1.mp4")
var clip := project.insert_clip(bin_id, 1, 0)          # video on V1, linked audio on A1
var engine := TetraMltEngine.new()
engine.set_project(project)
texture_rect.texture = ImageTexture.create_from_image(engine.render_frame(120, Vector2i(960, 540)))
```

Monitor with playback and sound:

```gdscript
project.changed.connect(func(): engine.set_project(project))
engine.set_preview_size(monitor.size)
engine.frame_ready.connect(func(frame):
    texture.update(engine.get_current_frame_image())   # or set_image() when the size changed
    timeline.playhead = frame)
engine.playback_finished.connect(_on_playback_finished)
engine.seek(0)        # shows the frame
engine.play()         # sound goes to the "Master" bus
# J / K / L:
engine.set_speed(-1.0); engine.play()     # J
engine.pause()                            # K
engine.set_speed(1.0); engine.play()      # L
```

Apply an effect with a parameter panel:

```gdscript
var effects := TetraVideoAssets.get_effects(true, project.get_width(), project.get_height())   # for TetraOperationBrowser
var operation: Dictionary = effects[i]
var index := project.add_clip_effect(clip, operation.id, operation.service, TetraVideoAssets.get_default_values(operation))
# the panel edits a copy of the values and writes them back:
project.set_clip_effect_values(clip, index, panel_values)     # one undo step; "changed" rebuilds the engine graph
```

Render with progress:

```gdscript
var renderer := TetraVideoRenderer.new()       # keep it in a member
renderer.progress.connect(func(frame, percent): bar.value = percent)
renderer.finished.connect(func(ok, message): print("done: " if ok else "failed: ", message))
renderer.start(project, "res://renders/out.ogv", "tetra/ogv", {"width": 1280, "height": 720, "quality": 7})
```

## Tests

`scripts/test.sh tetra_video` runs `test_project.gd` (editing rules, linked
clips, undo exactness over random operation sequences), `test_xml_engine.gd`
(XML, project file round trip, foreign and malformed files, frames from the
engine, effects, fades, stills, titles, thumbnails, `melt` accepting the
file), `test_playback.gd` (real-time playback, reverse, loop, sound),
`test_assets.gd` (catalog and value conversion) and `test_render.gd` (presets
and renders checked with `ffprobe`). Frames they render are saved as
`build/screens/video-*.png`.
