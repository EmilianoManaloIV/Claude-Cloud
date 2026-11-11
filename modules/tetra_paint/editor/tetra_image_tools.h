#pragma once

#include "core/os/keyboard.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

// The tools of the Image workspace: their names for scripts, labels, icons,
// GIMP's default shortcuts and the option forms shown in the Tool Options
// panel. The first six are the document's paint tools, in the order of
// TetraImageDocument::PaintTool.
class TetraImageTools {
public:
	enum Tool {
		TOOL_PAINTBRUSH,
		TOOL_PENCIL,
		TOOL_ERASER,
		TOOL_AIRBRUSH,
		TOOL_SMUDGE,
		TOOL_CLONE,
		TOOL_BUCKET_FILL,
		TOOL_GRADIENT,
		TOOL_COLOR_PICKER,
		TOOL_RECT_SELECT,
		TOOL_ELLIPSE_SELECT,
		TOOL_FUZZY_SELECT,
		TOOL_MOVE,
		TOOL_ZOOM,
		TOOL_MAX,
	};

	struct Info {
		const char *name; // For scripts: "paintbrush", "rect-select", ...
		const char *label;
		const char *icon; // Editor theme icon.
		Key shortcut;
		const char *hint; // Shown in the status bar while the tool is active.
	};

	static const Info &get_info(Tool p_tool);
	// TOOL_MAX when there is no tool of that name.
	static Tool find(const String &p_name);
	static bool is_paint_tool(Tool p_tool) { return p_tool <= TOOL_CLONE; }
	// The tools in the order of the toolbox.
	static const Tool *get_toolbox_order();

	// The tool's options as an operation of the shared schema ("tool:<name>"),
	// ready for TetraParamPanel. Percentages are 0..100 and thresholds 0..255,
	// as GIMP shows them.
	static Dictionary get_options_schema(Tool p_tool);
};
