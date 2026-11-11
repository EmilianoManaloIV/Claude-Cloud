# 01 — Godot 4.7.2 editor-extension cookbook for C++ modules (the editor SHELL)

Scope: everything a Tetra Studio engineer needs to write **C++ editor code inside a custom module** against the
*internal* editor API of the pinned Godot tree (`upstream/godot`, commit `ed1daf0bf`, "4.7.2-stable").
All paths below are relative to `upstream/godot/` unless they start with `/`. Line numbers refer to that pinned tree.

Verification status of what is written here:

| Kind of claim | How it was verified |
|---|---|
| Class names, signatures, enum values, include paths | Read from the header/source cited next to the claim. |
| Build-system behaviour (module discovery, icons, docs, SCU, link order) | Read from `SConstruct`, `methods.py`, `modules/SCsub`, `scu_builders.py`, `editor/SCsub`, `editor/icons/SCsub`. The existing empty `tetra*` skeleton modules already build with `scripts/build.sh` (build log: `BUILD EXIT: 0`). |
| Section 8 recipes (Xvfb + llvmpipe screenshot, throwaway project, addon driver, `--script`, extension lists) | **Executed** against the built binary `upstream/godot/bin/godot.linuxbsd.editor.x86_64` (results quoted in §8). |
| C++ snippets | Assembled only from signatures read in the headers; **not compiled** (the research rules forbade test compiles). Treat them as header-accurate templates; the first real build is the compile check. |

Contents: [1 Module anatomy](#1-module-anatomy) · [2 EditorPlugin](#2-registering-an-editorplugin-and-the-full-editorplugin-interface) ·
[3 Main-screen workspaces](#3-main-screen-workspaces) · [4 Opening files](#4-opening-files-into-a-workspace) ·
[5 UI toolkit conventions](#5-editor-ui-toolkit-conventions) · [6 Auto-generated property panels](#6-auto-generated-property-panels-editorinspector--proxy-object) ·
[7 Pan/zoom canvas](#7-a-pannablezoomable-custom-canvas-control) · [8 Testing & automation](#8-testing-and-automation) ·
[9 Gotchas for 4.7.2](#9-gotchas-for-472-what-differs-from-40-43-era-knowledge)

---

## 1. Module anatomy

### 1.1 What makes a directory a module

`methods.py:302 is_module()` — a directory is a module iff it contains **all three** of `register_types.h`, `SCsub`, `config.py`.
The module name is the directory basename (`methods.py:257`), and it fixes these symbol names:

| Thing | Name derived from module dir `foo` | Where it is consumed |
|---|---|---|
| Init functions | `void initialize_foo_module(ModuleInitializationLevel)` / `void uninitialize_foo_module(ModuleInitializationLevel)` | generated `modules/register_module_types.gen.cpp` (`modules/modules_builders.py:17-52`) which does `#include "<module path>/register_types.h"` |
| Enable define | `MODULE_FOO_ENABLED` | generated `modules/modules_enabled.gen.h` (`modules_builders.py:8-13`); include it with `#include "modules/modules_enabled.gen.h"` before testing the define |
| SCons option | `module_foo_enabled=yes/no` | `SConstruct:485` |
| Static library | `libmodule_foo.a` | `modules/SCsub:41-43` |
| Global define | `GODOT_MODULE` (set on `env_modules`) | `modules/SCsub:13` |

Custom modules are found through `custom_modules=<dir>[,<dir>...]` (`SConstruct:280, 446-469`). `scripts/build.sh` passes
`custom_modules=/home/user/Claude-Cloud/modules`. Discovery is recursive by default (`custom_modules_recursive=yes`,
`SConstruct:281`), so nested modules inside a module directory are also picked up — do not leave stray
`register_types.h + SCsub + config.py` triples in sub-directories. A directory containing a `version.py` with
`short_name = "godot"` is skipped (`methods.py:262-270`).

For every custom search path, SConstruct does (`SConstruct:467`):

```python
env.Prepend(CPPPATH=[path, os.path.dirname(path)])   # path = /home/user/Claude-Cloud/modules
```

so **any** translation unit in the engine can write `#include "tetra/whatever.h"` (relative to `/home/user/Claude-Cloud/modules`)
or `#include "modules/tetra/whatever.h"` (relative to `/home/user/Claude-Cloud`). `"#"` (the Godot root) is prepended
later (`SConstruct:520`) and therefore wins lookups; `modules/<x>/...` resolves to the built-in module first, then ours.
Never name a custom module like a built-in one unless you intend to override it ("custom modules can override built-in
ones", `SConstruct:468`).

### 1.2 `config.py`

All hooks SConstruct looks for (`SConstruct:472-494`, `1112-1143`):

| Function | Required | Called | Purpose |
|---|---|---|---|
| `can_build(env, platform)` | yes | `SConstruct:1119` | Return `False` to disable the module. Call `env.module_add_dependencies(...)` here. |
| `configure(env)` | yes | `SConstruct:1124` | Receives the **global** `env` *before* any SCsub runs. Put global flags here only if they must apply to the whole engine. |
| `is_enabled()` | no | `SConstruct:479` | Default value of `module_<name>_enabled` (default `True`). |
| `get_opts(platform)` | no | `SConstruct:489` | Extra SCons options (`[BoolVariable(...), ...]`). |
| `get_doc_classes()` / `get_doc_path()` | no | `SConstruct:1127-1130` | List of class names documented by this module and the dir (relative to the module) with their XML files. |
| `get_icons_path()` | no | `SConstruct:1135-1139` | Icon dir relative to the module; default is `"icons"`. |

`env.module_add_dependencies(module, [deps], optional=False)` (`methods.py:324`) records dependencies;
`module_check_dependencies` (`methods.py:337`) disables a module whose required deps are disabled, and
`sort_module_list` (`methods.py:364`) orders `env.module_list` so that **dependencies come first**. Because
`modules/SCsub:43` does `env.Prepend(LIBS=[lib])` per module in that order, dependants end up *before* their
dependencies on the link line — correct for static archives. In-tree example: `modules/gltf/config.py`
(`env.module_add_dependencies("gltf", ["csg", "gridmap"], True)`).

`env.editor_build` is `env["target"] == "editor"` (`SConstruct:533`) and defines `TOOLS_ENABLED` (`SConstruct:549`).

Recommended Tetra `config.py` (the existing skeleton already follows it):

```python
# modules/tetra_paint/config.py
import os

def can_build(env, platform):
    env.module_add_dependencies("tetra_paint", ["tetra"])          # required
    return env.editor_build                                         # editor-only module

def configure(env):
    pass                                                            # keep global env untouched

def get_doc_classes():
    return ["TetraImageDocument"]                                   # only classes registered with GDREGISTER_*

def get_doc_path():
    return "doc_classes"
```

### 1.3 `SCsub` patterns

The contract (`modules/SCsub:31-43`): the module SCsub receives `env` (global) and `env_modules` (clone with
`GODOT_MODULE`), and must append **object nodes** to `env.modules_sources`. `env.add_source_files(list, "glob-or-list")`
(`methods.py:79`) globs relative to the SCsub directory and skips `*.gen.cpp`.

```python
#!/usr/bin/env python
from misc.utility.scons_hints import *     # type hints only; harmless

Import("env")
Import("env_modules")

env_tetra = env_modules.Clone()            # ALWAYS clone; never mutate env_modules

# 1) plain sources in the module root
env_tetra.add_source_files(env.modules_sources, "*.cpp")

# 2) sub-directories: one call per directory (SCons Glob has no '**')
for d in ["core", "bridge"]:
    env_tetra.add_source_files(env.modules_sources, d + "/*.cpp")

# 3) editor-only sources (in-tree idiom: modules/gridmap/SCsub, modules/csg/SCsub, modules/multiplayer/SCsub)
if env.editor_build:
    env_tetra.add_source_files(env.modules_sources, "editor/*.cpp")
```

Variants found in-tree:

| Need | Pattern | In-tree example |
|---|---|---|
| Nested SCsub per sub-dir | `Export("env_openxr")`, `SConscript("editor/SCsub")`, child does `Import("env_openxr")` | `modules/openxr/SCsub`, `modules/openxr/editor/SCsub` |
| Recursive glob | `methods.glob_recursive(pattern, node)` exists (`methods.py:565`) but **no in-tree SCsub uses it**; prefer explicit directory lists | — |
| Extra include path for the module only | `env_x.Prepend(CPPPATH=[...])` | `modules/csg/SCsub` (`env_csg.Prepend(CPPPATH=[thirdparty_dir + "include"])`) |
| Include path needed by other engine code | `env.Prepend(CPPPATH=[...])` on the **global** env | `modules/freetype/SCsub:66` |
| Module-private defines | `env_x.Append(CPPDEFINES=[("NAME", value)])` | `modules/csg/SCsub` |
| Third-party code without warnings | `env_thirdparty = env_x.Clone(); env_thirdparty.disable_warnings()` | `modules/csg/SCsub`, `modules/openxr/SCsub` |
| Force rebuild of module objects when third-party objs change | `env.Depends(module_obj, thirdparty_obj)` | `modules/csg/SCsub` |
| Unit tests .cpp | `if env["tests"]: env_x.add_source_files(env.modules_sources, "./tests/*.cpp")` | `modules/multiplayer/SCsub` |

**Linking external system libraries (GEGL, MLT, …).** The final program is linked from the *global* `env`
(`platform/linuxbsd/SCsub: prog = env.add_program("#bin/godot", common_linuxbsd)`), and SConstruct runs
`modules/SCsub` before `platform/<p>/SCsub` (`SConstruct:1236-1242`). Therefore: **library names must be appended to the
global `env`'s `LIBS`** (cloned envs never reach the link). In-tree precedents:

* `modules/camera/SCsub:20` — `env.Append(LIBS=["camera2ndk", "mediandk"])` from inside a module SCsub.
* `platform/linuxbsd/detect.py:250-420` — `env.ParseConfig("pkg-config <pkgs> --cflags --libs")`, guarded by
  `os.system("pkg-config --exists <pkg>") == 0` (e.g. lines 314, 342, 353).
* `modules/freetype/SCsub:95-108` — notes that `env["LIBS"]` holds SCons library *nodes* first and plain *strings*
  (system libs) after them; system libs must stay after the engine archives.

Recommended split for Tetra (compile flags on the module clone only, link flags on the global env — this avoids
changing the global `CPPPATH`, which would recompile the entire engine):

```python
# modules/tetra_paint/SCsub (excerpt)
import os

env_paint = env_modules.Clone()

if os.system("pkg-config --exists gegl-0.4") == 0:
    env_paint.ParseConfig("pkg-config gegl-0.4 --cflags")      # -I... only for this module's TUs
    env.ParseConfig("pkg-config gegl-0.4 --libs")              # -l... must be on the global env
    env_paint.Append(CPPDEFINES=["TETRA_HAVE_GEGL"])
```

Both `pkg-config --exists gegl-0.4` and `pkg-config --exists mlt++-7` succeed on the current build machine.
`pkg-config` itself is mandatory for linuxbsd builds (`platform/linuxbsd/detect.py:21-24`).

**Exceptions / RTTI per module.**

* Exceptions are disabled globally: `disable_exceptions` defaults to `True` (`SConstruct:263`) and adds
  `-fno-exceptions` to `CXXFLAGS` (`SConstruct:931-935`; MSVC: `_HAS_EXCEPTIONS=0`).
* A module (or a single bridge TU) can re-enable them on its **cloned** env; the in-tree pattern is
  `drivers/metal/SCsub:37-40` (also `platform/windows/SCsub:91-93`):

  ```python
  env_bridge = env_tetra.Clone()
  if "-fno-exceptions" in env_bridge["CXXFLAGS"]:
      env_bridge["CXXFLAGS"].remove("-fno-exceptions")
  env_bridge.Append(CXXFLAGS=["-fexceptions"])
  env_bridge.add_source_files(env.modules_sources, "bridge/mlt_bridge.cpp")
  ```

  An exception must never unwind into a frame compiled with `-fno-exceptions`: catch everything inside the bridge TU.
* RTTI is **not** disabled anywhere for GCC/Clang (no `-fno-rtti` in `SConstruct`, `methods.py` or any `detect.py`;
  MSVC explicitly passes `/GR`, `platform/windows/detect.py:410`). `dynamic_cast`/`typeid` work in module code, but Godot
  objects should use `Object::cast_to<T>()` (which does not use RTTI, `core/object/object.h:635-656, 855-872`).
* Language level: `-std=gnu++17` / `-std=gnu17` (`SConstruct:915-916`). Warnings default to `warnings=all`
  (`-Wall -Wshadow -Wno-misleading-indentation …`), `werror` defaults to `False` (`SConstruct:255-257, 976-1032`).

### 1.4 `register_types.{h,cpp}` and `ModuleInitializationLevel`

`modules/register_module_types.h`:

```cpp
enum ModuleInitializationLevel {
	MODULE_INITIALIZATION_LEVEL_CORE = GDEXTENSION_INITIALIZATION_CORE,
	MODULE_INITIALIZATION_LEVEL_SERVERS = GDEXTENSION_INITIALIZATION_SERVERS,
	MODULE_INITIALIZATION_LEVEL_SCENE = GDEXTENSION_INITIALIZATION_SCENE,
	MODULE_INITIALIZATION_LEVEL_EDITOR = GDEXTENSION_INITIALIZATION_EDITOR
};
```

Call order in `main/main.cpp` (`Main::setup`/`setup2`): CORE (`:2281`) → SERVERS (`:3226`) → SCENE (`:3792`, after
`register_scene_types()`) → EDITOR (`:3825`, after `register_editor_types()`, inside `#ifdef TOOLS_ENABLED`, with
`ClassDB::set_current_api(ClassDB::API_EDITOR)`). The EDITOR level runs in **every** tools-build process (editor,
project manager, `--headless --script`, running a game with the editor binary) and runs **before `EditorNode` exists**.
`uninitialize_*` is called in reverse order at shutdown (`main.cpp:903-968`).

What goes where (taken from `modules/gridmap/register_types.cpp`, `modules/csg/register_types.cpp`,
`modules/multiplayer/register_types.cpp`, `modules/gltf/register_types.cpp`, `modules/mp3/register_types.cpp`,
`modules/theora/register_types.cpp`):

| Level | Put here | Example |
|---|---|---|
| `SCENE` | `GDREGISTER_CLASS` of nodes/resources, `ResourceLoader::add_resource_format_loader`, `ResourceSaver::add_resource_format_saver` | gridmap `GDREGISTER_CLASS(GridMap)`; theora loader |
| `EDITOR` (`#ifdef TOOLS_ENABLED`) | `EditorPlugins::add_by_type<T>()`, `GDREGISTER_CLASS` of editor-API classes, `EditorNode::add_init_callback(fn)`, `GLOBAL_DEF*` for editor-related project settings | gridmap, csg, multiplayer, gltf, mp3 |

Things that need a live editor (`EditorNode::get_singleton()`, `EditorFileSystem`, `EditorSettings`,
`ResourceFormatImporter::get_singleton()->add_importer`) must be deferred: either do them in the plugin constructor /
`NOTIFICATION_ENTER_TREE`, or register a plain function with `EditorNode::add_init_callback(void (*)())`
(`editor/editor_node.h`: `static void add_init_callback(EditorNodeInitCallback p_callback)`), which is run at the very end
of `EditorNode::EditorNode()` (`editor/editor_node.cpp:9584-9585`). `EditorNode::add_plugin_init_callback` runs
earlier, right after module plugins are created (`editor_node.cpp:9430-9432`).

Minimal correct template (modelled on `modules/gridmap/register_types.cpp`):

```cpp
// modules/tetra_paint/register_types.h
#pragma once
#include "modules/register_module_types.h"

void initialize_tetra_paint_module(ModuleInitializationLevel p_level);
void uninitialize_tetra_paint_module(ModuleInitializationLevel p_level);
```

```cpp
// modules/tetra_paint/register_types.cpp
#include "register_types.h"

#include "tetra_image_document.h"            // Resource subclass + loader/saver

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"            // GDREGISTER_* live here; NOT pulled in by object.h

#ifdef TOOLS_ENABLED
#include "editor/tetra_paint_plugin.h"
#include "editor/plugins/editor_plugin.h"    // EditorPlugins::add_by_type
#endif

static Ref<ResourceFormatLoaderTetraImage> loader;
static Ref<ResourceFormatSaverTetraImage> saver;

void initialize_tetra_paint_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(TetraImageDocument);
		loader.instantiate();
		ResourceLoader::add_resource_format_loader(loader);
		saver.instantiate();
		ResourceSaver::add_resource_format_saver(saver);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::add_by_type<TetraPaintPlugin>();
	}
#endif
}

void uninitialize_tetra_paint_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		ResourceLoader::remove_resource_format_loader(loader);
		loader.unref();
		ResourceSaver::remove_resource_format_saver(saver);
		saver.unref();
	}
}
```

Class registration macros (`core/object/class_db.h:568-589`); every one is wrapped in `if constexpr (GD_IS_CLASS_ENABLED(T))`:

| Macro | Effect (`class_db.h:245-300`) |
|---|---|
| `GDREGISTER_CLASS(T)` | instantiable + exposed to scripting/ClassDB (`exposed = true`, creation func set) |
| `GDREGISTER_VIRTUAL_CLASS(T)` | exposed, instantiable, `is_virtual = true` (used for `GridMapEditorPlugin`) |
| `GDREGISTER_ABSTRACT_CLASS(T)` | exposed, **no** creation func |
| `GDREGISTER_INTERNAL_CLASS(T)` | instantiable via ClassDB but `exposed = false` |
| `GDREGISTER_RUNTIME_CLASS(T)` | exposed runtime class (placeholder in editor) |

A class that only uses `GDCLASS(T, Base)` and is **never registered** still works for `memnew`, signals, `callable_mp`,
`Object::cast_to`, `_bind_methods` (run lazily: `Object::_initialize()` → `_initialize_classv()` → `initialize_class()`,
`core/object/object.cpp:220-224`, `object.h:292-313`). In-tree editor code relies on this constantly (e.g.
`ImportDockParameters` in `editor/docks/import_dock.cpp:48`). Register a class only if it must be created by name
(Resources loaded from `.tres/.res`, nodes saved in scenes) or scripted/documented.

### 1.5 Icons

* `editor/icons/SCsub` globs `editor/icons/*.svg` **plus** `<module>/icons/*.svg` for every enabled module
  (`env.module_icons_paths`, filled at `SConstruct:1133-1139`; custom modules use absolute paths) and generates
  `editor/themes/editor_icons.gen.h` via `editor/icons/editor_icons_builders.py`.
* The icon **name is the file basename without extension** (`editor_icons_builders.py:21`). It is registered in the editor
  theme as `p_theme->set_icon(name, EditorStringName(EditorIcons), icon)` (`editor/themes/editor_icons.cpp`,
  `editor_register_icons`). Fetch with `get_editor_theme_icon(SNAME("Name"))` (§5.4).
* Module icons are appended after the editor's own icons, so a module SVG with the same basename as a built-in icon
  **silently replaces it**. 1028 built-in files exist; notably `Image.svg`, `Mesh.svg`, `Paint.svg`, `Bucket.svg`,
  `Eraser.svg`, `ColorPick.svg`, `Zoom*.svg`, `Tool*.svg`, `Play*.svg`, `Lock.svg`, `GuiVisibility*.svg`, `Checkerboard.svg`
  already exist; `Model.svg` and `Video.svg` do **not**. Prefix Tetra icons (`TetraImage.svg`, `TetraBrush.svg`, …).
* Automatic uses of an icon named like something else:
  * class icon in create dialogs / inspector / **FileSystem dock file icon**: icon named exactly like the class / resource
    type (`FileSystemDock::_get_tree_item_icon`, `editor/docks/filesystem_dock.cpp:213-224`: `has_theme_icon(p_file_type, EditorIcons)`).
    So `icons/TetraImageDocument.svg` gives every `*.timg` file its icon for free.
  * main-screen button: if `get_plugin_icon()` returns null, an icon named exactly like `get_plugin_name()` is used
    (`editor/editor_main_screen.cpp:265-268`). A plugin called "Image" would therefore pick up the built-in `Image.svg`.
  * dock tab icon: `EditorDock::set_icon_name("Name")`.
* Authoring rules (from `modules/gridmap/icons/GridMap.svg` and `editor/themes/editor_icons.cpp`): 16×16 SVG,
  `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">`, flat fills using the editor palette so the
  dark→light conversion map applies (`editor/themes/editor_color_map.cpp`): generic UI `#e0e0e0`, 2D node `#8da5f3`,
  3D node `#fc7f7f`, Control `#8eef97`, mesh-resource orange `#ffca5f`, error `#ff5f5f`, success `#5fff97`,
  warning `#ffdd65`. Names ending in `MediumThumb` / `BigThumb` are treated as thumbnails (`editor_icons_builders.py:24-27`).
  Icons are scaled by `EDSCALE` and re-generated on theme change (they are `DPITexture`s).

### 1.6 Doc classes

`get_doc_classes()` + `get_doc_path()` map class names to an XML dir (`env.doc_class_path`); `editor/SCsub` globs
`<module>/<doc_path>/*.xml` (absolute path branch for custom modules) into `editor/doc/doc_data_compressed.gen.h`.
XML skeleton (from `modules/gridmap/doc_classes/GridMapEditorPlugin.xml`):

```xml
<?xml version="1.0" encoding="UTF-8" ?>
<class name="TetraImageDocument" inherits="Resource" api_type="editor" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="../../../doc/class.xsd">
	<brief_description>…</brief_description>
	<description>…</description>
	<tutorials></tutorials>
</class>
```

Skeletons are generated with `bin/godot… --doctool <godot-root>` (`main.cpp:716`). Missing docs for an exposed class are
not a build error. Only list classes that are actually registered (exposed).

### 1.7 SCU builds and custom modules

`scripts/build.sh` uses `scu_build=yes`. `scu_builders.py:261-449 generate_scu_files()` processes a **hard-coded list
of in-tree folders** (`process_folder(["editor/docks"], ...)`, `["modules/gridmap"]`, …) relative to the Godot root and
returns the set `_scu_folders`. `methods.add_source_files` → `add_source_files_scu` (`methods.py:54-74`) only swaps the
glob for `.scu/scu_*.gen.cpp` when `self.Dir(subdir).tpath` is in that set. Custom module folders are never in the set
⇒ **Tetra modules are always compiled one TU per `.cpp`**, even in SCU builds. Consequences:

* Tetra code must include everything it uses (no accidental reliance on a unity file's earlier includes).
* Editor code *is* SCU-compiled in 8-file units (`max_includes_per_scu = 8`, `SConstruct:688`), which is why some
  in-tree headers appear to work without includes that a standalone TU needs (see §9).
* Object files of out-of-tree modules are redirected to `bin/obj/external/...` (`methods.py:87-111`).

### 1.8 Module unit tests

`tests=yes` (`SConstruct:240`) builds the doctest runner. `modules/SCsub:45-55` globs `<module>/tests/*.h` (works for
absolute custom paths) and generates `modules/modules_tests.gen.h`, included by `tests/test_main.cpp:73`. See §8.6.

---

## 2. Registering an EditorPlugin and the full EditorPlugin interface

### 2.1 Registration

`editor/plugins/editor_plugin.h:280-311`:

```cpp
typedef EditorPlugin *(*EditorPluginCreateFunc)();

class EditorPlugins {
	enum { MAX_CREATE_FUNCS = 128 };
	...
public:
	static int get_plugin_count();
	static EditorPlugin *create(int p_idx);
	template <typename T> static void add_by_type() { add_create_func(creator<T>); }   // creator<T>() == memnew(T)
	static void add_create_func(EditorPluginCreateFunc p_func);                        // fails if >= 128
};
```

Call `EditorPlugins::add_by_type<MyPlugin>()` at `MODULE_INITIALIZATION_LEVEL_EDITOR`
(`modules/gridmap/register_types.cpp`, `modules/csg/register_types.cpp`, `modules/multiplayer/register_types.cpp`,
`modules/gltf/register_types.cpp`). The plugin needs a default constructor. `MAX_CREATE_FUNCS = 128` is shared by all
modules.

Creation order in `EditorNode::EditorNode()` (`editor/editor_node.cpp:9352-9432`):

1. `DebuggerEditorPlugin` (`:9352`)
2. `CanvasItemEditorPlugin` ("2D"), `Node3DEditorPlugin` ("3D"), `ScriptEditorPlugin` ("Script") (`:9391-9393`)
3. Game view plugin ("Game") unless recovery mode (`:9395-9397`)
4. `AssetLibraryEditorPlugin` ("Asset Store") if available (`:9405-9409`)
5. AnimationPlayer / track key / marker key plugins, VersionControl, AudioBuses (`:9412-9418`)
6. **all `EditorPlugins::create(i)` in registration order** (`:9420-9422`) — modules are initialised in dependency-sorted
   order, so `tetra` plugins precede `tetra_paint` etc. Many built-in sub-editors are also registered through
   `EditorPlugins` from `register_editor_types()`, which runs *before* module EDITOR init — so module plugins are created
   after all built-in ones.
7. GDExtension editor plugins (`:9424-9428`), then `plugin_init_callbacks` (`:9430-9432`)

`EditorNode::add_editor_plugin(EditorPlugin *p_editor, bool p_config_changed = false)` (`editor_node.cpp:4365-4374`) does:
`if (has_main_screen()) editor_main_screen->add_main_plugin(p)`; `editor_data.add_editor_plugin(p)`; `add_child(p)`.
So `has_main_screen()` and `get_plugin_name()` are queried **right after construction**, and the plugin is a child of
`EditorNode` (it gets `NOTIFICATION_ENTER_TREE`/`READY`).

In the plugin constructor, singletons created earlier are valid: `EditorNode::get_singleton()`,
`EditorNode::get_editor_main_screen()`, `EditorDockManager::get_singleton()`, `Node3DEditor::get_singleton()`,
`CanvasItemEditor::get_singleton()`, `EditorSettings::get_singleton()`, `EditorFileSystem::get_singleton()`,
`EditorUndoRedoManager::get_singleton()`. Theme items are **not** safe to query in constructors (§5.4).

### 2.2 Virtual interface (`editor/plugins/editor_plugin.h:188-219`)

| Virtual (exact signature) | Default | Called by / meaning |
|---|---|---|
| `virtual String get_plugin_name() const;` | script `_get_plugin_name` | Button text on main screen, key in state dictionaries (`EditorData::get_editor_plugin_states`), name for `set_main_screen_editor`. Must be unique. In-tree main screens return `TTRC("…")`. |
| `virtual const Ref<Texture2D> get_plugin_icon() const;` | script | Main-screen button icon (queried at add time and on every theme change, `editor_main_screen.cpp:60-73`). |
| `virtual String get_plugin_version() const;` / `virtual void set_plugin_version(const String &);` | stored string | Addon version; irrelevant for modules. |
| `virtual bool has_main_screen() const;` | `false` | `true` ⇒ gets a workspace button and is considered by `get_handling_main_editor`. |
| `virtual void make_visible(bool p_visible);` | script | Main screen: show/hide the workspace root. Sub-editor: show/hide docks/toolbars when an object it handles is (un)edited. |
| `virtual void set_current() {}` | — | Sub-editor became the "current" one after `edit_item(..., p_set_current = true)`. |
| `virtual void selected_notify() {}` | — | Main-screen button was selected (`EditorMainScreen::select`). |
| `virtual void edit(Object *p_object);` | script | Start editing `p_object`. **Also called with `nullptr`** when the editor stops editing (`EditorNode::_plugin_over_edit`, `_edit_current` "Unedit previous plugin"). |
| `virtual bool handles(Object *p_object) const;` | `false` | Return true for objects this plugin edits. Never called with null by the editor core. |
| `virtual bool can_auto_hide() const;` | `true` | Sub-editors: if false, the plugin stays visible after the object is deselected ("self-owning", `editor_node.cpp:1287`). |
| `virtual Dictionary get_state() const;` / `virtual void set_state(const Dictionary &p_state);` | script | Per-**scene** editor state, saved in `res://.godot/editor/<scene>-editstate-<md5>.cfg`. |
| `virtual void clear();` | script | Reset per-scene state (new/changed scene); also called from `remove_editor_plugin`. |
| `virtual String get_unsaved_status(const String &p_for_scene = "") const;` | `""` | Non-empty text ⇒ "Save & Quit / Save & Reload" confirmation on quit, text shown as the dialog message (`editor_node.cpp:3791-3807`); with a scene path ⇒ shown when closing that scene tab. |
| `virtual void save_external_data();` | script | Save everything the plugin owns. Called on scene save (`editor_node.cpp:2526, 2603`), before running (`:991`), and after the quit dialog is confirmed (`:3730-3733`). |
| `virtual void apply_changes();` | script | Flush pending UI edits into the edited objects; called before saving/running (`EditorData::apply_changes_in_editors`). |
| `virtual void get_breakpoints(List<String> *p_breakpoints);` | script | Script editors only. |
| `virtual bool get_remove_list(List<Node *> *p_list);` | `false` | Legacy. |
| `virtual void set_window_layout(Ref<ConfigFile> p_layout);` / `virtual void get_window_layout(Ref<ConfigFile> p_layout);` | script | Global layout persistence in `res://.godot/editor/editor_layout.cfg` (`editor_node.cpp:6285-6293, 6320-6351`). Use for "which documents are open", splitter offsets, floating state. |
| `virtual void edited_scene_changed() {}` | — | Scene tab switched (`EditorData::notify_edited_scene_changed`). |
| `virtual bool build();` | `true` | Pre-run build hook; false aborts the run. |
| `virtual void run_scene(const String &p_scene, Vector<String> &r_args);` | script | Adjust command line of the launched game. |
| `virtual bool forward_canvas_gui_input(const Ref<InputEvent> &p_event);` | script, `false` | 2D viewport input while this plugin is active for the edited object. Return true to consume. |
| `virtual void forward_canvas_draw_over_viewport(Control *p_overlay);` | script | Draw on the 2D viewport overlay (`p_overlay->draw_*`). |
| `virtual void forward_canvas_force_draw_over_viewport(Control *p_overlay);` | script | Same but always, after `set_force_draw_over_forwarding_enabled()`. |
| `virtual EditorPlugin::AfterGUIInput forward_3d_gui_input(Camera3D *p_camera, const Ref<InputEvent> &p_event);` | script, `AFTER_GUI_INPUT_PASS` | 3D viewport input (see below). |
| `virtual void forward_3d_draw_over_viewport(Control *p_overlay);` | script | Draw 2D overlay on each 3D viewport. |
| `virtual void forward_3d_force_draw_over_viewport(Control *p_overlay);` | script | Always-on variant. |

Enums (`editor_plugin.h:76-111`):

```cpp
enum CustomControlContainer {
	CONTAINER_TOOLBAR,
	CONTAINER_SPATIAL_EDITOR_MENU,
	CONTAINER_SPATIAL_EDITOR_SIDE_LEFT,
	CONTAINER_SPATIAL_EDITOR_SIDE_RIGHT,
	CONTAINER_SPATIAL_EDITOR_BOTTOM,
	CONTAINER_CANVAS_EDITOR_MENU,
	CONTAINER_CANVAS_EDITOR_SIDE_LEFT,
	CONTAINER_CANVAS_EDITOR_SIDE_RIGHT,
	CONTAINER_CANVAS_EDITOR_BOTTOM,
	CONTAINER_INSPECTOR_BOTTOM,
	CONTAINER_PROJECT_SETTING_TAB_LEFT,
	CONTAINER_PROJECT_SETTING_TAB_RIGHT,
};
enum AfterGUIInput { AFTER_GUI_INPUT_PASS, AFTER_GUI_INPUT_STOP, AFTER_GUI_INPUT_CUSTOM };
```

Where each container goes (`editor/plugins/editor_plugin.cpp:140-190`):

| Value | Parent the control is added to |
|---|---|
| `CONTAINER_TOOLBAR` | `EditorNode::get_title_bar()` (top bar, next to the run buttons) |
| `CONTAINER_SPATIAL_EDITOR_MENU` | `Node3DEditor::get_singleton()->add_control_to_menu_panel()` (3D toolbar) |
| `CONTAINER_SPATIAL_EDITOR_SIDE_LEFT` / `_RIGHT` | `Node3DEditor::add_control_to_left_panel()` / `add_control_to_right_panel()` |
| `CONTAINER_SPATIAL_EDITOR_BOTTOM` | `Node3DEditor::get_singleton()->get_shader_split()` |
| `CONTAINER_CANVAS_EDITOR_MENU` | `CanvasItemEditor::get_singleton()->add_control_to_menu_panel()` |
| `CONTAINER_CANVAS_EDITOR_SIDE_LEFT` / `_RIGHT` | `CanvasItemEditor::add_control_to_left_panel()` / `add_control_to_right_panel()` |
| `CONTAINER_CANVAS_EDITOR_BOTTOM` | `CanvasItemEditor::get_singleton()->get_bottom_split()` |
| `CONTAINER_INSPECTOR_BOTTOM` | `InspectorDock::get_singleton()->get_addon_area()` |
| `CONTAINER_PROJECT_SETTING_TAB_LEFT` / `_RIGHT` | `ProjectSettingsEditor::get_singleton()->get_tabs()` (left = moved to index 0) |

### 2.3 Non-virtual helpers (all `public`, `editor_plugin.h:164-271`)

```cpp
void add_control_to_container(CustomControlContainer p_location, Control *p_control);
void remove_control_from_container(CustomControlContainer p_location, Control *p_control);

void add_dock(EditorDock *p_dock);        // == EditorDockManager::get_singleton()->add_dock(p_dock)
void remove_dock(EditorDock *p_dock);

void add_tool_menu_item(const String &p_name, const Callable &p_callable);    // Project > Tools
void add_tool_submenu_item(const String &p_name, PopupMenu *p_submenu);
void remove_tool_menu_item(const String &p_name);
PopupMenu *get_export_as_menu();                                              // Scene > Export As...

void set_input_event_forwarding_always_enabled();   // forward_*_gui_input even when not handling the selection
void set_force_draw_over_forwarding_enabled();      // enables forward_*_force_draw_over_viewport
int update_overlays() const;                        // queue redraw of 2D overlay or all visible 3D viewports

EditorInterface *get_editor_interface();
ScriptCreateDialog *get_script_create_dialog();
void add_undo_redo_inspector_hook_callback(Callable p_callable);   // void(Object *undo_redo, Object *modified, String property, Variant new_value)
void remove_undo_redo_inspector_hook_callback(Callable p_callable);
void queue_save_layout();
void make_bottom_panel_item_visible(Control *p_item);
void hide_bottom_panel();

void add_translation_parser_plugin(const Ref<EditorTranslationParserPlugin> &p_parser);          // + remove_*
void add_import_plugin(const Ref<EditorImportPlugin> &p_importer, bool p_first_priority = false); // + remove_*
void add_export_plugin(const Ref<EditorExportPlugin> &p_exporter);                               // + remove_*
void add_export_platform(const Ref<EditorExportPlatform> &p_platform);                           // + remove_*
void add_node_3d_gizmo_plugin(const Ref<EditorNode3DGizmoPlugin> &p_gizmo_plugin);               // + remove_*
void add_inspector_plugin(const Ref<EditorInspectorPlugin> &p_plugin);                           // + remove_*
void add_scene_format_importer_plugin(const Ref<EditorSceneFormatImporter> &p_importer, bool p_first_priority = false); // + remove_*
void add_scene_post_import_plugin(const Ref<EditorScenePostImportPlugin> &p_importer, bool p_first_priority = false);    // + remove_*
void add_autoload_singleton(const String &p_name, const String &p_path);                         // + remove_*
void add_debugger_plugin(const Ref<EditorDebuggerPlugin> &p_plugin);                             // + remove_*
void add_resource_conversion_plugin(const Ref<EditorResourceConversionPlugin> &p_plugin);        // + remove_*
void add_context_menu_plugin(EditorContextMenuPlugin::ContextMenuSlot p_slot, const Ref<EditorContextMenuPlugin> &p_plugin); // + remove_*
```

`protected`: `EditorUndoRedoManager *get_undo_redo();`, `add_custom_type` / `remove_custom_type`.

Signals (`editor_plugin.cpp:715-720`): `scene_changed(scene_root: Node)`, `scene_closed(filepath: String)`,
`main_screen_changed(screen_name: String)`, `resource_saved(resource: Resource)`, `scene_saved(filepath: String)`,
`project_settings_changed()` (deprecated).

**Deprecated and now `protected` (inside `#ifndef DISABLE_DEPRECATED`, `editor_plugin.h:148-159`)** — do not use from C++:
`add_control_to_dock(DockSlot, Control *, const Ref<Shortcut> & = nullptr)`, `remove_control_from_docks(Control *)`,
`set_dock_tab_icon(Control *, const Ref<Texture2D> &)`, `Button *add_control_to_bottom_panel(Control *, const String &, const Ref<Shortcut> & = nullptr)`,
`remove_control_from_bottom_panel(Control *)`. Their implementations just wrap the control in a new `EditorDock`
(`editor_plugin.cpp:88-129`). The bottom panel itself is now a dock slot (`EditorBottomPanel : public DockTabContainer`,
`editor/gui/editor_bottom_panel.h:55`); `EditorNode::get_bottom_panel()->add_item(...)` still exists but only creates an
`EditorDock` with `DOCK_SLOT_BOTTOM` and returns a dummy `Button` (`editor/gui/editor_bottom_panel.cpp:241-262`).

### 2.4 The 4.7 dock API: `EditorDock` + `EditorDockManager`

Headers: `editor/docks/editor_dock.h`, `editor/docks/editor_dock_manager.h`.

```cpp
class EditorDock : public MarginContainer {          // a dock IS a Control; add your content as its child
public:
	enum DockLayout {
		DOCK_LAYOUT_VERTICAL = 1,
		DOCK_LAYOUT_HORIZONTAL = 2,
		DOCK_LAYOUT_FLOATING = 4,
		DOCK_LAYOUT_ALL = DOCK_LAYOUT_VERTICAL | DOCK_LAYOUT_HORIZONTAL | DOCK_LAYOUT_FLOATING,
	};
	enum DockSlot {
		DOCK_SLOT_NONE = -1,
		DOCK_SLOT_LEFT_UL, DOCK_SLOT_LEFT_BL, DOCK_SLOT_LEFT_UR, DOCK_SLOT_LEFT_BR,
		DOCK_SLOT_RIGHT_UL, DOCK_SLOT_RIGHT_BL, DOCK_SLOT_RIGHT_UR, DOCK_SLOT_RIGHT_BR,
		DOCK_SLOT_BOTTOM, DOCK_SLOT_BOTTOM_L, DOCK_SLOT_BOTTOM_R,
		DOCK_SLOT_MAX
	};
	void open();              // open at its slot without stealing the current tab
	void make_visible();      // open + make it the current tab
	void make_floating();
	void close();

	void set_title(const String &p_title);              // tab title; falls back to the node name (get_display_title())
	void set_layout_key(const String &p_key);           // stable key in editor_layout.cfg (defaults to the display title)
	void set_global(bool p_global);                     // default true: listed in the "Editor > Editor Docks" menu (editor_node.cpp:8114)
	void set_transient(bool p_transient);               // default false: true => open/closed state is not persisted
	void set_closable(bool p_closable);                 // default false: show "close" in the dock context popup
	void set_icon_name(const StringName &p_name);       // EditorIcons name, re-fetched on theme change
	void set_dock_icon(const Ref<Texture2D> &p_icon);   // explicit texture (wins over icon_name)
	void set_force_show_icon(bool p_force);
	void set_title_color(const Color &p_color);
	void set_dock_shortcut(const Ref<Shortcut> &p_shortcut);
	void set_default_slot(DockSlot p_slot);             // DOCK_SLOT_NONE => starts closed/floating
	void set_available_layouts(BitField<DockLayout> p_layouts);   // default VERTICAL | FLOATING

	virtual void update_layout(DockLayout p_layout);                                   // called when moved between vertical/horizontal/floating
	virtual void save_layout_to_config(Ref<ConfigFile> &p_layout, const String &p_section) const;
	virtual void load_layout_from_config(const Ref<ConfigFile> &p_layout, const String &p_section);
	DockLayout get_current_layout() const;
};
// signals: "opened", "closed" (+ internal "_tab_style_changed")
```

```cpp
class EditorDockManager : public Object {
public:
	static EditorDockManager *get_singleton();
	void add_dock(EditorDock *p_dock);       // registers; opens it if default_slot != DOCK_SLOT_NONE
	void remove_dock(EditorDock *p_dock);    // unregisters + unparents; you still own (and must free) the dock
	void open_dock(EditorDock *p_dock, bool p_set_current = true);
	void close_dock(EditorDock *p_dock);
	void focus_dock(EditorDock *p_dock);
	void make_dock_floating(EditorDock *p_dock);
	void set_dock_enabled(EditorDock *p_dock, bool p_enabled);
	void set_docks_visible(bool p_show);  bool are_docks_visible() const;
	PopupMenu *get_docks_menu();
	...
};
```

Default slots used by built-in docks (for placing Tetra docks consistently):

| Dock | Slot | Layouts | Source |
|---|---|---|---|
| Scene | `DOCK_SLOT_LEFT_UR` | default | `editor/docks/scene_tree_dock.cpp:4998-5000` |
| Import | `DOCK_SLOT_LEFT_UR` | default | `editor/docks/import_dock.cpp:753-755` |
| FileSystem | `DOCK_SLOT_LEFT_BR` | `DOCK_LAYOUT_ALL` | `editor/docks/filesystem_dock.cpp:4431-4434` |
| History | `DOCK_SLOT_LEFT_BR` | default | `editor/docks/history_dock.cpp:240-242` |
| Inspector, Signals, Groups | `DOCK_SLOT_RIGHT_UL` | default | `inspector_dock.cpp:709-711`, `signals_dock.cpp:47-49`, `groups_dock.cpp:42-44` |
| Output, Audio, Animation, Shader Editor, … | `DOCK_SLOT_BOTTOM` | `HORIZONTAL \| FLOATING` | `editor/editor_log.cpp:489-492`, `editor/audio/editor_audio_buses.cpp:1524-1527` |

Worked example modelled on `modules/multiplayer/editor/replication_editor.cpp:157-166` +
`modules/multiplayer/editor/multiplayer_editor_plugin.cpp:118-121` and `modules/gridmap/editor/grid_map_editor_plugin.cpp:1491-1500, 1965-1979`:

```cpp
// tetra_layers_dock.h
#pragma once
#include "editor/docks/editor_dock.h"

class Tree;

class TetraLayersDock : public EditorDock {
	GDCLASS(TetraLayersDock, EditorDock);
	Tree *tree = nullptr;

public:
	TetraLayersDock();
};
```

```cpp
// tetra_layers_dock.cpp
#include "tetra_layers_dock.h"

#include "editor/settings/editor_command_palette.h"   // ED_SHORTCUT_AND_COMMAND
#include "editor/themes/editor_scale.h"                // EDSCALE
#include "scene/gui/tree.h"

TetraLayersDock::TetraLayersDock() {
	set_name(TTRC("Layers"));                           // node name == tab title (auto-translated)
	set_layout_key("TetraLayers");
	set_icon_name("TetraLayers");                       // modules/tetra_paint/icons/TetraLayers.svg
	set_dock_shortcut(ED_SHORTCUT_AND_COMMAND("docks/open_tetra_layers", TTRC("Open Layers Dock")));
	set_default_slot(EditorDock::DOCK_SLOT_RIGHT_BL);
	set_available_layouts(EditorDock::DOCK_LAYOUT_VERTICAL | EditorDock::DOCK_LAYOUT_FLOATING);
	set_global(false);                                   // not in the Editor Docks menu (context-sensitive dock)
	set_transient(true);                                 // do not persist open/closed
	set_custom_minimum_size(Size2(200, 0) * EDSCALE);

	tree = memnew(Tree);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(tree);
}
```

```cpp
// in the owning plugin
#include "editor/docks/editor_dock_manager.h"

TetraPaintPlugin::TetraPaintPlugin() {
	layers_dock = memnew(TetraLayersDock);
	EditorDockManager::get_singleton()->add_dock(layers_dock);   // opens it (default_slot != NONE)...
	layers_dock->close();                                        // ...so close until the workspace is shown
}
void TetraPaintPlugin::make_visible(bool p_visible) {
	if (p_visible) { workspace->show(); layers_dock->open(); }
	else           { workspace->hide(); layers_dock->close(); }
}
// On teardown (NOTIFICATION_EXIT_TREE): EditorDockManager::get_singleton()->remove_dock(layers_dock); memdelete(layers_dock);
```

### 2.5 3D viewport integration (for `tetra_model`'s in-viewport edit mode)

* How input reaches a plugin: `Node3DEditorViewport` input handler (`editor/scene/3d/node_3d_editor_plugin.cpp:2198-2231`)
  first asks `EditorNode::get_editor_plugins_force_input_forwarding()` (plugins that called
  `set_input_event_forwarding_always_enabled()`), then `EditorNode::get_editor_plugins_over()` (plugins currently
  editing the selection — added by `EditorNode::_plugin_over_edit` when `handles()` returned true). Result handling:
  `AFTER_GUI_INPUT_STOP` ⇒ the viewport returns immediately (event consumed); `AFTER_GUI_INPUT_CUSTOM` ⇒ built-in
  selection/gizmo handling is skipped but camera navigation still runs; `AFTER_GUI_INPUT_PASS` ⇒ normal processing.
  (`EditorPluginList::forward_3d_gui_input`, `editor/plugins/editor_plugin_list.cpp`.)
* Overlay drawing: `Node3DEditorViewport::_draw()` (`node_3d_editor_plugin.cpp:4065-4067`) calls
  `forward_3d_draw_over_viewport(surface)` for "over" plugins and `forward_3d_force_draw_over_viewport(surface)` for
  force-over plugins. Request a redraw with `update_overlays()`.
* Canonical in-tree example of a "tool mode inside the 3D viewport" owned by a module:
  `modules/gridmap/editor/grid_map_editor_plugin.{h,cpp}` — `GridMapEditorPlugin` (`handles()` = `p_object->is_class("GridMap")`,
  `forward_3d_gui_input` delegates to `GridMapEditor::forward_spatial_input_event(Camera3D *, const Ref<InputEvent> &)`,
  `forward_3d_draw_over_viewport` draws text with `p_overlay->draw_string(...)`, `make_visible` opens/closes its dock).
* Gizmos: subclass `EditorNode3DGizmoPlugin` (`editor/scene/3d/node_3d_editor_gizmos.h`; virtuals such as
  `has_gizmo(Node3D *)`, `get_gizmo_name()`, `get_priority()`, `redraw(EditorNode3DGizmo *)`, `get_handle_name/value`,
  `set_handle(const EditorNode3DGizmo *, int p_id, bool p_secondary, Camera3D *, const Point2 &)`,
  `commit_handle(const EditorNode3DGizmo *, int, bool, const Variant &p_restore, bool p_cancel)`; sub-gizmo API
  `subgizmos_intersect_ray`, `subgizmos_intersect_frustum`, `get_subgizmo_transform`, `set_subgizmo_transform`,
  `commit_subgizmos`) and register it with `Node3DEditor::get_singleton()->add_gizmo_plugin(ref)` — exactly what
  `modules/csg/editor/csg_gizmos.cpp:505-508` does in its plugin constructor. Helper: `editor/scene/3d/gizmos/gizmo_3d_helper.h`.
* `Node3DEditor` (`editor/scene/3d/node_3d_editor_plugin.h`): `static Node3DEditor *get_singleton()`,
  `static const unsigned int VIEWPORTS_COUNT = 4`, `Node3DEditorViewport *get_editor_viewport(int p_idx)`,
  `ToolMode get_tool_mode() const`, `void update_transform_gizmo()`, `add_control_to_menu_panel/left_panel/right_panel(Control *)`.

---

## 3. Main-screen workspaces

### 3.1 How `EditorMainScreen` works (`editor/editor_main_screen.{h,cpp}`)

```cpp
class EditorMainScreen : public PanelContainer {
public:
	enum EditorTable { EDITOR_2D = 0, EDITOR_3D, EDITOR_SCRIPT, EDITOR_GAME, EDITOR_ASSETLIB };
	void select_next();  void select_prev();
	void select_by_name(const String &p_name);     // matches Button::get_text() == plugin name; ERR if missing
	void select(int p_index);
	int get_selected_index() const;
	int get_plugin_index(EditorPlugin *p_editor) const;
	EditorPlugin *get_selected_plugin() const;
	EditorPlugin *get_plugin_by_name(const String &p_plugin_name) const;
	bool can_auto_switch_screens() const;
	VBoxContainer *get_control() const;            // the VBoxContainer ("MainScreen") that hosts all workspace roots
	void set_button_enabled(int p_index, bool p_enabled);  bool is_button_enabled(int p_index) const;
	void add_main_plugin(EditorPlugin *p_editor);  void remove_main_plugin(EditorPlugin *p_editor);
};
```

* Buttons appear in **plugin creation order**: 2D, 3D, Script, Game, Asset Store, then module plugins in registration
  order (verified visually: screenshot in §8 shows `2D 3D Script Game Asset Store`). The `EditorTable` indices are
  only valid for the five built-ins (and shift if Game/Asset Store are unavailable — recovery mode, no TLS).
* `add_main_plugin` (`editor_main_screen.cpp:253-280`): creates a toggle `Button` with theme variation
  `"MainScreenButton"`, `set_name`/`set_text(get_plugin_name())`, shortcut = `EditorSettings::get_shortcut("editor/editor_" + name.to_lower())`
  **if such a shortcut already exists**, icon = `get_plugin_icon()` or the `EditorIcons` icon named like the plugin.
  ⇒ to give the "Image" workspace a shortcut, call `ED_SHORTCUT("editor/editor_image", TTRC("Open Image Workspace"), KeyModifierMask::CTRL | Key::F6)`
  in the plugin **constructor** (it runs before `add_main_plugin`). Built-in ones: `editor/editor_2d` Ctrl+F1,
  `editor/editor_3d` Ctrl+F2, `editor/editor_script` Ctrl+F3 (`editor_node.cpp:8999-9001`), plus `editor/editor_game`,
  `editor/editor_assetlib`, `editor/editor_next`, `editor/editor_prev` (`editor_node.cpp:417-432`).
* `select(int)` (`editor_main_screen.cpp:166-204`): returns early while `EditorNode::is_changing_scene()`; then
  `old->make_visible(false)`, `new->make_visible(true)`, `new->selected_notify()`, and every plugin gets
  `notify_main_screen_changed(name)` (signal `main_screen_changed`).
* The selected index is persisted in `editor_layout.cfg` (`selected_main_editor_idx`,
  `editor_main_screen.cpp:79-99`) and restored deferred on startup; on first run the 3D screen is selected.
* `can_auto_switch_screens()` (`editor_main_screen.cpp:230-247`) returns true only while the selected button is **left
  of "Script"** (i.e. 2D or 3D). It is consulted when a **Node** is selected (`editor_node.cpp:3204-3209, 4653`), so
  selecting nodes never yanks the user out of Script/Game/Asset Store/**Tetra workspaces**. Resources are different: see §4.1.

Switching programmatically:

```cpp
#include "editor/editor_interface.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"

EditorInterface::get_singleton()->set_main_screen_editor("Image");          // == get_editor_main_screen()->select_by_name()
EditorNode::get_editor_main_screen()->select(
		EditorNode::get_editor_main_screen()->get_plugin_index(my_plugin)); // by plugin pointer
```

(There is no `EditorNode::editor_select()` any more; the logic lives in `EditorMainScreen::select`.)
`EditorInterface.set_main_screen_editor("Script")` was exercised from a tool script in §8 and works.

### 3.2 Worked example: a main-screen plugin

Studied templates: `AssetLibraryEditorPlugin` (`editor/asset_library/asset_library_editor_plugin.h:407-423`, `.cpp:2327-2345`)
and `GameViewPluginBase` (`editor/run/game_view_plugin.h:289-330`, `.cpp:1700-1745`). Both add their root control to
`EditorNode::get_singleton()->get_editor_main_screen()->get_control()`, give it `SIZE_EXPAND_FILL` vertically and `hide()` it.
The Game view additionally wraps its root in a `WindowWrapper` (`editor/gui/window_wrapper.h`) so the workspace can be
floated into its own window.

```cpp
// modules/tetra_paint/editor/tetra_paint_plugin.h
#pragma once

#include "editor/plugins/editor_plugin.h"

class TetraLayersDock;
class TetraPaintWorkspace;      // : public VBoxContainer (toolbar + HSplit(tool box | canvas tabs))

class TetraPaintPlugin : public EditorPlugin {
	GDCLASS(TetraPaintPlugin, EditorPlugin);

	TetraPaintWorkspace *workspace = nullptr;
	TetraLayersDock *layers_dock = nullptr;

protected:
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return TTRC("Image"); }
	virtual const Ref<Texture2D> get_plugin_icon() const override;
	virtual bool has_main_screen() const override { return true; }
	virtual void make_visible(bool p_visible) override;
	virtual bool handles(Object *p_object) const override;
	virtual void edit(Object *p_object) override;
	virtual String get_unsaved_status(const String &p_for_scene = "") const override;
	virtual void save_external_data() override;
	virtual void apply_changes() override;
	virtual void set_window_layout(Ref<ConfigFile> p_layout) override;
	virtual void get_window_layout(Ref<ConfigFile> p_layout) override;

	TetraPaintPlugin();
};
```

```cpp
// modules/tetra_paint/editor/tetra_paint_plugin.cpp
#include "tetra_paint_plugin.h"

#include "tetra_layers_dock.h"
#include "tetra_paint_workspace.h"
#include "../tetra_image_document.h"

#include "core/os/keyboard.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/box_container.h"

const Ref<Texture2D> TetraPaintPlugin::get_plugin_icon() const {
	// Same idiom as AssetLibraryEditorPlugin::get_plugin_icon(); safe at any time because it asks the editor theme directly.
	return EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("TetraImage"), EditorStringName(EditorIcons));
}

void TetraPaintPlugin::make_visible(bool p_visible) {
	if (p_visible) {
		workspace->show();
		layers_dock->open();
	} else {
		workspace->hide();
		layers_dock->close();
	}
}

bool TetraPaintPlugin::handles(Object *p_object) const {
	return Object::cast_to<TetraImageDocument>(p_object) != nullptr;
}

void TetraPaintPlugin::edit(Object *p_object) {
	TetraImageDocument *doc = Object::cast_to<TetraImageDocument>(p_object);
	if (!doc) {
		return;                       // edit(nullptr) == "stop editing": keep open documents, do nothing
	}
	workspace->open_document(Ref<TetraImageDocument>(doc));   // Ref keeps it alive beyond the inspector history
}

String TetraPaintPlugin::get_unsaved_status(const String &p_for_scene) const {
	if (!p_for_scene.is_empty()) {
		return String();              // our documents do not belong to scenes
	}
	const PackedStringArray unsaved = workspace->get_unsaved_document_names();
	if (unsaved.is_empty()) {
		return String();
	}
	return TTR("Save changes to the following image(s) before quitting?") + "\n" + String("\n").join(unsaved);
}

void TetraPaintPlugin::save_external_data() {
	if (!EditorNode::get_singleton()->is_exiting()) {   // same guard as ScriptEditorPlugin::save_external_data()
		workspace->save_all_documents();
	}
}

void TetraPaintPlugin::apply_changes() {
	workspace->commit_pending_edits();
}

void TetraPaintPlugin::set_window_layout(Ref<ConfigFile> p_layout) {
	workspace->restore_open_documents(p_layout->get_value("TetraPaint", "open_documents", PackedStringArray()));
}

void TetraPaintPlugin::get_window_layout(Ref<ConfigFile> p_layout) {
	p_layout->set_value("TetraPaint", "open_documents", workspace->get_open_document_paths());
}

void TetraPaintPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_EXIT_TREE: {
			EditorDockManager::get_singleton()->remove_dock(layers_dock);
			memdelete(layers_dock);
			layers_dock = nullptr;
		} break;
	}
}

TetraPaintPlugin::TetraPaintPlugin() {
	// Defined before EditorMainScreen::add_main_plugin() looks up "editor/editor_" + name.to_lower().
	ED_SHORTCUT("editor/editor_image", TTRC("Open Image Workspace"), KeyModifierMask::CTRL | Key::F6);

	workspace = memnew(TetraPaintWorkspace);
	workspace->set_v_size_flags(Control::SIZE_EXPAND_FILL);     // parent is a VBoxContainer
	EditorNode::get_singleton()->get_editor_main_screen()->get_control()->add_child(workspace);
	workspace->hide();                                          // shown by make_visible(true)

	layers_dock = memnew(TetraLayersDock);
	EditorDockManager::get_singleton()->add_dock(layers_dock);
	layers_dock->close();
}
```

Root-control sizing rules: the parent (`EditorMainScreen::get_control()`) is a `VBoxContainer` named `"MainScreen"` with
separation 0 (`editor_main_screen.cpp:304-309`). Horizontal fill is the Control default (`SIZE_FILL`); vertical needs
`set_v_size_flags(Control::SIZE_EXPAND_FILL)`. Exactly one workspace root is visible at a time (enforced by
`make_visible`). `AssetLibraryEditorPlugin` additionally calls `set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT)`,
which is irrelevant inside a container. `EditorNode` itself gives the main screen a minimum height of
`Size2(0, 80) * EDSCALE` (`editor_node.cpp:8862`); keep the workspace root's own minimum size small (put large content in
`ScrollContainer`s / clipped canvases) so the bottom dock and side docks can still be resized freely.

State persistence choices: per-scene `get_state`/`set_state` is wrong for document workspaces; use
`get_window_layout`/`set_window_layout` (global `editor_layout.cfg`) or
`EditorSettings::get_singleton()->set_project_metadata(section, key, value)` / `get_project_metadata(section, key, default)`
(per project, `res://.godot/editor/project_metadata.cfg`; used by `TextureRegionEditor`, `texture_region_editor_plugin.cpp:1229-1232`).

---

## 4. Opening files into a workspace

### 4.1 The path from a FileSystem double-click to `EditorPlugin::edit()`

1. `FileSystemDock::_tree_activate_file()` / `_file_list_activate_file()` → `FileSystemDock::_select_file(path)`
   (`editor/docks/filesystem_dock.cpp:1320-1385`):
   * `path.import` exists with `[remap] importer="keep"` or `"skip"` → warning "Importing has been disabled for this file…", stop.
   * `ResourceLoader::get_resource_type(path)` is `"PackedScene"`/`"AnimationLibrary"` → scene import settings dialog (if the
     extension belongs to a scene importer) or `EditorNode::load_scene_or_resource(path)`.
   * `ResourceLoader::is_imported(path)` → if the importer `has_advanced_options()`, `importer->show_advanced_options(path)`
     (audio/font import dialogs); otherwise `EditorNode::load_resource(path)`.
   * otherwise `EditorNode::load_resource(path)`.
2. `Error EditorNode::load_resource(const String &p_resource, bool p_ignore_broken_deps = false)`
   (`editor/editor_node.cpp:1680-1716`):
   * extension in `force_textfile_extensions` (only `"csv"`) → text editor path;
   * `ResourceLoader::exists(path, "")` → `res = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &err)`;
   * else extension in `textfile_extensions` → `ScriptEditor::get_singleton()->open_file(path)` (a `TextFile` resource);
   * else extension in `other_file_extensions` → `OS::get_singleton()->shell_open(globalized path)` and **return OK** (opened by the OS!);
   * finally `InspectorDock::get_singleton()->edit_resource(res)`.
3. `InspectorDock::edit_resource` → `_resource_selected(res, "")` → `EditorNode::get_singleton()->push_item(res.ptr(), "")`
   (`editor/docks/inspector_dock.cpp:381-388`).
4. `void EditorNode::push_item(Object *p_object, const String &p_property = "", bool p_inspector_only = false)`
   (`editor_node.cpp:3018-3030`): adds to `EditorSelectionHistory` and calls `_edit_current()`.
5. `EditorNode::_edit_current()` (`editor_node.cpp:3115-3320`): puts the object in the Inspector, then (unless
   `inspector_only`):
   * `EditorPlugin *main_plugin = editor_data.get_handling_main_editor(current_obj);`
     — iterates plugins **backwards** ("so that we can check user-created plugins first", `editor/editor_data.cpp`), returns the
     first with `has_main_screen() && handles(obj)`. Module plugins are created after all built-ins ⇒ **they win**.
   * For a non-Script object: if `main_plugin != currently selected screen plugin` → `old_screen->edit(nullptr)`,
     `editor_main_screen->select(plugin_index)`, `main_plugin->edit(obj)`; else `main_plugin->edit(obj)`.
     For **Resources** `skip_main_plugin` stays `false` — the workspace switch always happens (the
     `can_auto_switch_screens()` guard only applies to Nodes).
   * `edit_item(current_obj, editor_owner)` → every **sub-editor** (`!has_main_screen() && handles(obj)`,
     `EditorData::get_handling_sub_editors`) gets `edit(obj)` + `make_visible(true)` via `_plugin_over_edit`
     (`editor_node.cpp:1272-1285, 2907-3009`); previously active ones that no longer handle the object get
     `edit(nullptr)` + `make_visible(false)`.

Other entry points that end in the same `push_item`: `EditorNode::edit_resource(const Ref<Resource> &)`,
`EditorInterface::get_singleton()->edit_resource(res)`, `EditorInterface::inspect_object(obj, property, inspector_only)`,
the Inspector's "Edit"/inspect action on a resource property. Unfolding a sub-resource inside the Inspector does **not**
push (it calls `EditorNode::edit_item(res, this)` — sub-editors only; `EditorPropertyResource::_open_editor_pressed`).

Design consequences for Tetra:

* A workspace plugin whose `handles()` returns true for a type is opened on double-click of any file that loads as that
  type. If the "Image" plugin handled `Texture2D`, every double-clicked PNG (type `CompressedTexture2D`) would jump to the
  Image workspace and bypass the default texture preview flow. Decide per type; a safer default is: `handles()` only for
  Tetra's own document resources, plus an explicit **"Open in Image Editor"** context-menu entry (§4.5) for PNG/JPG/…
* `edit(nullptr)` is routine. Do not close documents in response.
* The object passed to `edit()` is only guaranteed alive while the selection history references it; take a `Ref<>`.
* Resources loaded through `ResourceLoader` are cached by path (`CACHE_MODE_REUSE`): a second double-click yields the same
  instance, so "already open" detection can compare pointers.
* An *imported* resource shows the Inspector banner "This resource was imported, so it's not editable…"
  (`editor_node.cpp:3188-3191`) and cannot be saved over (`EditorNode::save_resource` → `save_resource_as`,
  `editor_node.cpp:1807-1813`). Native Tetra documents should therefore be **non-imported** resources with their own
  loader/saver.

### 4.2 Which files show up in the FileSystem dock, and with what type

`EditorFileSystem::_update_extensions()` (`editor/file_system/editor_file_system.cpp:3743-3777`) builds:

* `valid_extensions` = every extension returned by `ResourceLoader::get_recognized_extensions_for_type("")` (all
  registered `ResourceFormatLoader`s, which includes all importers through `ResourceFormatImporter`)
  ∪ editor setting `docks/filesystem/textfile_extensions` (default `"txt,md,cfg,ini,log,json,yml,yaml,toml,xml"`)
  ∪ editor setting `docks/filesystem/other_file_extensions` (default `"ico,icns"`) (`editor/settings/editor_settings.cpp:717-718`);
* `import_extensions` = `ResourceFormatImporter::get_singleton()->get_recognized_extensions()`.

Files whose extension is not in `valid_extensions` are **invisible** (`editor_file_system.cpp:1242, 1513`). The stored
type is `ResourceLoader::get_resource_type(path)`, or `"TextFile"` / `"OtherFile"` for the two settings lists
(`editor_file_system.cpp:1356-1363`). `_update_extensions()` is re-run at the start of every scan (`:1119, :1718`).
`EditorNode` keeps its own copies of the two lists (`editor_node.cpp:8647-8656`, refreshed on settings change `:1133-1154`).

Verified at runtime (§8.4): with files `a.xcf`, `b.txt`, `c.kdenlive` in the project root, the dock initially listed only
`b.txt:TextFile`; after `EditorSettings.set_setting("docks/filesystem/other_file_extensions", "ico,icns,xcf,kdenlive")`
and `EditorFileSystem.scan()` it listed `a.xcf:OtherFile, b.txt:TextFile, c.kdenlive:OtherFile`.

Ways to make an extension visible, in order of preference:

| Method | Resulting type | Double-click does | Notes |
|---|---|---|---|
| Register a `ResourceFormatLoader` that recognises the extension (§4.3) | your Resource class name | `load_resource` → your loader → `push_item` → your plugin's `edit()` | Best for anything Tetra opens itself (`.xcf`, `.kdenlive`, `.mlt`, native docs). Also gives icon (class-named SVG), drag & drop, `ResourceLoader::load`. |
| Register a `ResourceImporter` (§4.4) | importer's `get_resource_type()` | import dialog or `load_resource` of the imported artefact | Right for assets converted for the *game* (textures, audio). Creates `.import` files; result is read-only. |
| Append to `docks/filesystem/other_file_extensions` | `"OtherFile"` | **`OS::shell_open`** (external app) | Only for files Tetra never opens itself. Note a loader registered for the same extension wins (`_update_extensions` skips extensions already valid). |
| Append to `docks/filesystem/textfile_extensions` | `"TextFile"` | built-in text editor | For text-ish sidecars. |

To append to a settings list from a module (do it from an `EditorNode::add_init_callback` function or the plugin
constructor; both lists are plain comma-separated strings):

```cpp
String exts = EDITOR_GET("docks/filesystem/other_file_extensions");
if (!exts.split(",", false).has("blend1")) {
	EditorSettings::get_singleton()->set_setting("docks/filesystem/other_file_extensions", exts + ",blend1");
}
// first scan happens after plugins are created; later: EditorFileSystem::get_singleton()->scan();
```

`EditorFileSystem` API (`editor/file_system/editor_file_system.h`): `static EditorFileSystem *get_singleton()`,
`EditorFileSystemDirectory *get_filesystem()`, `bool is_scanning() const`, `bool doing_first_scan() const`,
`void scan()`, `void scan_changes()`, `void update_file(const String &p_file)` (call after writing a file yourself),
`void update_files(const Vector<String> &)`, `String get_file_type(const String &p_file) const`,
`EditorFileSystemDirectory *get_filesystem_path(const String &p_path)`, `EditorFileSystemDirectory *find_file(const String &p_file, int *r_index) const`,
`void reimport_files(const Vector<String> &p_files)`, `HashSet<String> get_valid_extensions() const`.
Signals: `filesystem_changed`, `script_classes_updated`, `sources_changed(exist: bool)`,
`resources_reimporting(resources: PackedStringArray)`, `resources_reimported(resources)`, `resources_reload(resources)`.

`EditorFileSystemDirectory`: `String get_name()`, `String get_path() const`, `int get_subdir_count() const`,
`EditorFileSystemDirectory *get_subdir(int)`, `int get_file_count() const`, `String get_file(int) const`,
`String get_file_path(int) const`, `StringName get_file_type(int) const`, `ResourceUID::ID get_file_uid(int) const`,
`bool get_file_import_is_valid(int) const`, `uint64_t get_file_modified_time(int) const`,
`EditorFileSystemDirectory *get_parent()`, `int find_file_index(const String &) const`, `int find_dir_index(const String &) const`.

### 4.3 Custom Resource + `ResourceFormatLoader` / `ResourceFormatSaver` from a module

In-tree module examples: loader — `modules/theora/video_stream_theora.h:175-183` + `modules/theora/register_types.cpp`
(registered at SCENE level with `ResourceLoader::add_resource_format_loader(loader, true)`); loader + saver —
`modules/gdscript/gdscript_resource_format.{h,cpp}` + `modules/gdscript/register_types.cpp:145-149, 189-193`. Both declare the
loader/saver with **`GDSOFTCLASS`** (they are not ClassDB-registered).

Base-class virtuals (`core/io/resource_loader.h`, `core/io/resource_saver.h`):

```cpp
class ResourceFormatLoader : public RefCounted {
public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr,
			bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE);
	virtual bool exists(const String &p_path) const;
	virtual void get_recognized_extensions(List<String> *p_extensions) const;
	virtual void get_recognized_extensions_for_type(const String &p_type, List<String> *p_extensions) const;
	virtual bool recognize_path(const String &p_path, const String &p_for_type = String()) const;
	virtual bool handles_type(const String &p_type) const;
	virtual void get_classes_used(const String &p_path, HashSet<StringName> *r_classes);
	virtual String get_resource_type(const String &p_path) const;
	virtual String get_resource_script_class(const String &p_path) const;
	virtual ResourceUID::ID get_resource_uid(const String &p_path) const;
	virtual bool has_custom_uid_support() const;
	virtual void get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types = false);
	virtual Error rename_dependencies(const String &p_path, const HashMap<String, String> &p_map);
	virtual bool is_import_valid(const String &p_path) const { return true; }
	virtual bool is_imported(const String &p_path) const { return false; }
	virtual int get_import_order(const String &p_path) const { return 0; }
	virtual String get_import_group_file(const String &p_path) const { return ""; }
};
// ResourceLoader: MAX_LOADERS = 64
static void ResourceLoader::add_resource_format_loader(Ref<ResourceFormatLoader> p_format_loader, bool p_at_front = false);
static void ResourceLoader::remove_resource_format_loader(Ref<ResourceFormatLoader> p_format_loader);
static Ref<Resource> ResourceLoader::load(const String &p_path, const String &p_type_hint = "", CacheMode p_cache_mode = CACHE_MODE_REUSE, Error *r_error = nullptr);
static bool ResourceLoader::exists(const String &p_path, const String &p_type_hint = "");
static String ResourceLoader::get_resource_type(const String &p_path);

class ResourceFormatSaver : public RefCounted {
public:
	virtual Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0);
	virtual Error set_uid(const String &p_path, ResourceUID::ID p_uid);
	virtual bool recognize(const Ref<Resource> &p_resource) const;
	virtual void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const;
	virtual bool recognize_path(const Ref<Resource> &p_resource, const String &p_path) const;
};
// ResourceSaver: MAX_SAVERS = 64; flags FLAG_NONE=0, FLAG_RELATIVE_PATHS=1, FLAG_BUNDLE_RESOURCES=2, FLAG_CHANGE_PATH=4,
//                FLAG_OMIT_EDITOR_PROPERTIES=8, FLAG_SAVE_BIG_ENDIAN=16, FLAG_COMPRESS=32, FLAG_REPLACE_SUBRESOURCE_PATHS=64
static Error ResourceSaver::save(RequiredParam<Resource> rp_resource, const String &p_path = "", uint32_t p_flags = (uint32_t)FLAG_NONE);
static void ResourceSaver::add_resource_format_saver(Ref<ResourceFormatSaver> p_format_saver, bool p_at_front = false);
static void ResourceSaver::remove_resource_format_saver(Ref<ResourceFormatSaver> p_format_saver);
```

Template:

```cpp
// modules/tetra_paint/tetra_image_document.h
#pragma once

#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"

class TetraImageDocument : public Resource {
	GDCLASS(TetraImageDocument, Resource);

protected:
	static void _bind_methods();

public:
	// Native state lives in the engine-independent core library; the Resource is only the editor-facing handle.
	Error load_from_file(const String &p_path);
	Error save_to_file(const String &p_path) const;

	virtual bool editor_can_reload_from_file() override { return false; }   // do not let the editor hot-swap it on disk change
};

class ResourceFormatLoaderTetraImage : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderTetraImage, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};

class ResourceFormatSaverTetraImage : public ResourceFormatSaver {
	GDSOFTCLASS(ResourceFormatSaverTetraImage, ResourceFormatSaver);

public:
	virtual Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	virtual void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override;
	virtual bool recognize(const Ref<Resource> &p_resource) const override;
};
```

```cpp
// modules/tetra_paint/tetra_image_document.cpp (loader/saver part; modelled on video_stream_theora.cpp:802-837
// and gdscript_resource_format.cpp:149-182)
Ref<Resource> ResourceFormatLoaderTetraImage::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<TetraImageDocument> doc;
	doc.instantiate();
	const Error err = doc->load_from_file(p_path);
	if (r_error) {
		*r_error = err;
	}
	if (err != OK) {
		return Ref<Resource>();
	}
	return doc;          // ResourceLoader assigns the path and inserts it into the ResourceCache (resource_loader.cpp:596)
}

void ResourceFormatLoaderTetraImage::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("timg");
	p_extensions->push_back("xcf");
}

bool ResourceFormatLoaderTetraImage::handles_type(const String &p_type) const {
	return p_type == "TetraImageDocument" || p_type == "Resource";
}

String ResourceFormatLoaderTetraImage::get_resource_type(const String &p_path) const {
	return (p_path.has_extension("timg") || p_path.has_extension("xcf")) ? "TetraImageDocument" : "";
}

Error ResourceFormatSaverTetraImage::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<TetraImageDocument> doc = p_resource;
	ERR_FAIL_COND_V(doc.is_null(), ERR_INVALID_PARAMETER);
	return doc->save_to_file(p_path);
}

void ResourceFormatSaverTetraImage::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (Object::cast_to<TetraImageDocument>(*p_resource)) {
		p_extensions->push_back("timg");
	}
}

bool ResourceFormatSaverTetraImage::recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<TetraImageDocument>(*p_resource) != nullptr;
}
```

Facts to keep in mind:

* `ResourceLoader::_load` tries loaders in order and uses the first whose `recognize_path()` matches and returns a valid
  resource (`core/io/resource_loader.cpp`, `_load`). The default `recognize_path` compares the extension against
  `get_recognized_extensions[_for_type]`. **Never claim an extension that an importer already owns** (png, jpg, wav, mp3,
  glb, blend, …): with `p_at_front = true` you would hijack project-wide loading of those assets.
* `.uid` sidecars: `ResourceLoader::should_create_uid_file(path)` returns `!loader->has_custom_uid_support()`; the default
  `has_custom_uid_support()` is `GDVIRTUAL_IS_OVERRIDDEN(_get_resource_uid)`, i.e. `false` for a C++ loader. The editor
  will therefore create `foo.xcf.uid` next to each recognised file (same as `*.gd.uid`; observed for a script in §8).
  Override `has_custom_uid_support()`/`get_resource_uid()` only if the file format can store a UID itself.
* Saving through the editor: `EditorNode::save_resource(res)` / `save_resource_in_path(res, path)` call
  `ResourceSaver::save(res, path, FLAG_REPLACE_SUBRESOURCE_PATHS [| FLAG_COMPRESS])` (`editor_node.cpp:1736-1788`), emit
  `resource_saved`, and notify plugins. "Save As" asks `ResourceSaver::get_recognized_extensions(res, &list)` for filters.
* `Object::set_edited(true)` (TOOLS_ENABLED) marks a cached `res://` resource dirty; `EditorNode::_save_external_resources`
  (`editor_node.cpp:2360-2424`) then re-saves it with `ResourceSaver::save` on the next scene save. Simple documents can
  rely on this instead of implementing `save_external_data`.
* `Resource` basics (`core/io/resource.h`): `String get_path() const`, `virtual void set_path(const String &p_path, bool p_take_over = false)`,
  `void emit_changed()`, `void connect_changed(const Callable &, uint32_t p_flags = 0)`, `bool is_built_in() const`,
  `virtual void reload_from_file()`, `virtual bool editor_can_reload_from_file()`,
  macro `RES_BASE_EXTENSION("ext")` (declares the preferred `.tres`-alternative extension; e.g. `scene/resources/animation.h:40`).

### 4.4 `ResourceImporter` / `EditorImportPlugin` basics

* C++ importers subclass **`ResourceImporter`** (`core/io/resource_importer.h`); `EditorImportPlugin`
  (`editor/import/editor_import_plugin.h`) is only the script-facing wrapper around the same interface.
* Pure virtuals to implement: `String get_importer_name() const`, `String get_visible_name() const`,
  `void get_recognized_extensions(List<String> *) const`, `String get_save_extension() const`,
  `String get_resource_type() const`, `void get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset = 0) const`,
  `bool get_option_visibility(const String &p_path, const String &p_option, const HashMap<StringName, Variant> &p_options) const`,
  `Error import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants, List<String> *r_gen_files = nullptr, Variant *r_metadata = nullptr)`.
  Optional: `float get_priority() const` (default 1.0), `int get_import_order() const`, `int get_format_version() const`,
  `int get_preset_count() const`, `String get_preset_name(int) const`, `bool has_advanced_options() const`,
  `void show_advanced_options(const String &p_path)`, `bool can_import_threaded() const` (default false).
  `ImportOption` = `{ PropertyInfo option; Variant default_value; }`.
* `import()` must write `p_save_path + "." + get_save_extension()` (normally with `ResourceSaver::save`).
* Registration needs the live `ResourceFormatImporter` singleton; in-tree module pattern
  (`modules/mp3/register_types.cpp`): at EDITOR level `GDREGISTER_CLASS(ResourceImporterMP3); EditorNode::add_init_callback(_editor_init);`
  and in `_editor_init()`: `Ref<ResourceImporterMP3> imp; imp.instantiate(); ResourceFormatImporter::get_singleton()->add_importer(imp);`
  (`void add_importer(const Ref<ResourceImporter> &p_importer, bool p_first_priority = false)`). From inside a plugin use
  `add_import_plugin(ref)` which also triggers a rescan unless the first scan is running (`editor_plugin.cpp:443-451`).
* Scene formats: subclass `EditorSceneFormatImporter` and register with
  `ResourceImporterScene::add_scene_importer(ref)` (`modules/gltf/register_types.cpp`, `_editor_init`).

### 4.5 Context-menu entries: `EditorContextMenuPlugin`

Header `editor/inspector/editor_context_menu_plugin.h`:

```cpp
class EditorContextMenuPlugin : public RefCounted {
public:
	enum ContextMenuSlot {
		CONTEXT_SLOT_SCENE_TREE,
		CONTEXT_SLOT_FILESYSTEM,          // right-click on files/dirs in the FileSystem dock; paths = selection
		CONTEXT_SLOT_SCRIPT_EDITOR,
		CONTEXT_SLOT_FILESYSTEM_CREATE,   // the "Create New" submenu; callback receives { clicked directory }
		CONTEXT_SLOT_SCRIPT_EDITOR_CODE,
		CONTEXT_SLOT_SCENE_TABS,
		CONTEXT_SLOT_2D_EDITOR,
		CONTEXT_SLOT_INSPECTOR_PROPERTY,
	};
	virtual void get_options(const Vector<String> &p_paths);   // override; called every time the menu is built
	void add_menu_shortcut(const Ref<Shortcut> &p_shortcut, const Callable &p_callable);
	void add_context_menu_item(const String &p_name, const Callable &p_callable, const Ref<Texture2D> &p_texture);
	void add_context_menu_item_from_shortcut(const String &p_name, const Ref<Shortcut> &p_shortcut, const Ref<Texture2D> &p_texture);
	void add_context_submenu_item(const String &p_name, PopupMenu *p_menu, const Ref<Texture2D> &p_texture);
};
```

The callback is invoked with **one argument** (`EditorContextMenuPluginManager::invoke_callback`, `p_callback.callp(&argptr, 1, …)`):
for `CONTEXT_SLOT_FILESYSTEM` it is the selected paths (`Vector<String>` → `PackedStringArray`). Max 100 items per plugin
(`MAX_ITEMS`), `context_menu_items` is cleared before each `get_options` call.

```cpp
class TetraOpenInImageEditorMenu : public EditorContextMenuPlugin {
	GDCLASS(TetraOpenInImageEditorMenu, EditorContextMenuPlugin);

	void _open(const PackedStringArray &p_paths);     // loads each file into a TetraImageDocument and calls workspace->open_document()

public:
	virtual void get_options(const Vector<String> &p_paths) override {
		for (const String &path : p_paths) {
			const String ext = path.get_extension().to_lower();
			if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "exr" || ext == "tga") {
				add_context_menu_item(TTR("Open in Image Editor"), callable_mp(this, &TetraOpenInImageEditorMenu::_open), Ref<Texture2D>());
				return;
			}
		}
	}
};

// in the plugin constructor:
Ref<TetraOpenInImageEditorMenu> menu;
menu.instantiate();
add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, menu);
```

(`CONTEXT_SLOT_FILESYSTEM_CREATE` is the place for "New Image…" / "New Video Project…".)

### 4.6 Thumbnails for custom file types (optional)

`EditorResourcePreviewGenerator` (`editor/inspector/editor_resource_preview.h`): override
`virtual bool handles(const String &p_type) const`, `virtual Ref<Texture2D> generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const`
and/or `virtual Ref<Texture2D> generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const`;
register with `EditorResourcePreview::get_singleton()->add_preview_generator(ref)` (the editor registers its own at
`editor_node.cpp:9434-9439`). Generators run on a worker thread.
