#include "tetra_image_tools.h"

#include "../tetra_image_document.h"

#include "tetra/tetra_schema.h"

#include "core/object/object.h"

static const TetraImageTools::Info tool_infos[TetraImageTools::TOOL_MAX] = {
	{ "paintbrush", TTRC("Paintbrush"), "Paint", Key::P, TTRC("Click and drag to paint with the foreground colour. Shift+click draws a straight line.") },
	{ "pencil", TTRC("Pencil"), "Edit", Key::N, TTRC("Click and drag to draw hard-edged lines. Shift+click draws a straight line.") },
	{ "eraser", TTRC("Eraser"), "Eraser", KeyModifierMask::SHIFT | Key::E, TTRC("Click and drag to erase to transparency.") },
	{ "airbrush", TTRC("Airbrush"), "TetraToolAirbrush", Key::A, TTRC("Click and drag to spray paint; it builds up while the pointer rests.") },
	{ "smudge", TTRC("Smudge"), "TetraToolSmudge", Key::S, TTRC("Click and drag to smear the colours of the layer.") },
	{ "clone", TTRC("Clone"), "TetraToolClone", Key::C, TTRC("Ctrl+click sets the source, then click and drag to copy from it.") },
	{ "bucket-fill", TTRC("Bucket Fill"), "Bucket", KeyModifierMask::SHIFT | Key::B, TTRC("Click to fill the area of similar colour.") },
	{ "gradient", TTRC("Gradient"), "TetraToolGradient", Key::G, TTRC("Drag from the start to the end of the gradient (foreground to background).") },
	{ "color-picker", TTRC("Color Picker"), "ColorPick", Key::O, TTRC("Click to set the foreground colour; Ctrl+click sets the background colour.") },
	{ "rect-select", TTRC("Rectangle Select"), "TetraToolRectSelect", Key::R, TTRC("Drag to select a rectangle. Shift adds, Ctrl subtracts, Shift+Ctrl intersects; a click selects nothing.") },
	{ "ellipse-select", TTRC("Ellipse Select"), "TetraToolEllipseSelect", Key::E, TTRC("Drag to select an ellipse. Shift adds, Ctrl subtracts, Shift+Ctrl intersects; a click selects nothing.") },
	{ "fuzzy-select", TTRC("Fuzzy Select"), "TetraToolFuzzySelect", Key::U, TTRC("Click to select the contiguous area of similar colour. Shift adds, Ctrl subtracts.") },
	{ "move", TTRC("Move"), "ToolMove", Key::M, TTRC("Drag to move the active layer.") },
	{ "zoom", TTRC("Zoom"), "Zoom", Key::Z, TTRC("Click to zoom in, Ctrl+click to zoom out. Space+drag pans.") },
};

static const TetraImageTools::Tool toolbox_order[TetraImageTools::TOOL_MAX] = {
	TetraImageTools::TOOL_RECT_SELECT,
	TetraImageTools::TOOL_ELLIPSE_SELECT,
	TetraImageTools::TOOL_FUZZY_SELECT,
	TetraImageTools::TOOL_MOVE,
	TetraImageTools::TOOL_ZOOM,
	TetraImageTools::TOOL_COLOR_PICKER,
	TetraImageTools::TOOL_BUCKET_FILL,
	TetraImageTools::TOOL_GRADIENT,
	TetraImageTools::TOOL_PENCIL,
	TetraImageTools::TOOL_PAINTBRUSH,
	TetraImageTools::TOOL_ERASER,
	TetraImageTools::TOOL_AIRBRUSH,
	TetraImageTools::TOOL_CLONE,
	TetraImageTools::TOOL_SMUDGE,
};

const TetraImageTools::Info &TetraImageTools::get_info(Tool p_tool) {
	return tool_infos[CLAMP(int(p_tool), 0, TOOL_MAX - 1)];
}

TetraImageTools::Tool TetraImageTools::find(const String &p_name) {
	for (int i = 0; i < TOOL_MAX; i++) {
		if (p_name == tool_infos[i].name) {
			return Tool(i);
		}
	}
	return TOOL_MAX;
}

const TetraImageTools::Tool *TetraImageTools::get_toolbox_order() {
	return toolbox_order;
}

static Dictionary percent(const String &p_name, const String &p_label, double p_default) {
	return TetraSchema::param_float(p_name, p_label, p_default, 0.0, 100.0, 0.1, "%");
}

static Dictionary select_mode_param() {
	Array options;
	options.push_back(TetraSchema::make_option(int(TetraImageDocument::SELECT_REPLACE), TTR("Replace the selection")));
	options.push_back(TetraSchema::make_option(int(TetraImageDocument::SELECT_ADD), TTR("Add to the selection")));
	options.push_back(TetraSchema::make_option(int(TetraImageDocument::SELECT_SUBTRACT), TTR("Subtract from the selection")));
	options.push_back(TetraSchema::make_option(int(TetraImageDocument::SELECT_INTERSECT), TTR("Intersect with the selection")));
	return TetraSchema::param_enum("mode", TTR("Mode"), int(TetraImageDocument::SELECT_REPLACE), options);
}

static Dictionary feather_param() {
	return TetraSchema::set_description(TetraSchema::param_float("feather", TTR("Feather Edges"), 0.0, 0.0, 100.0, 0.1, "px"), TTR("Radius by which the edge of the selection is blurred. 0 leaves it sharp."));
}

static Dictionary threshold_param() {
	return TetraSchema::set_description(TetraSchema::param_float("threshold", TTR("Threshold"), 15.0, 0.0, 255.0, 0.1), TTR("Largest colour difference, on the 0-255 scale, that still counts as similar."));
}

Dictionary TetraImageTools::get_options_schema(Tool p_tool) {
	ERR_FAIL_INDEX_V(int(p_tool), int(TOOL_MAX), Dictionary());
	const Info &info = tool_infos[p_tool];
	Dictionary schema = TetraSchema::make_operation(String("tool:") + info.name, TTRGET(info.label), "Tools", "native", TTRGET(info.hint));

	if (is_paint_tool(p_tool)) {
		const Dictionary defaults = TetraImageDocument::get_default_stroke_settings(TetraImageDocument::PaintTool(p_tool));
		if (p_tool != TOOL_ERASER && p_tool != TOOL_SMUDGE) {
			Array modes;
			const PackedStringArray names = TetraImageDocument::get_layer_mode_names();
			for (int i = 0; i < names.size(); i++) {
				modes.push_back(TetraSchema::make_option(i, names[i]));
			}
			TetraSchema::add_param(schema, TetraSchema::param_enum("mode", TTR("Mode"), int(TetraImageDocument::LAYER_MODE_NORMAL), modes));
		}
		TetraSchema::add_param(schema, percent("opacity", TTR("Opacity"), double(defaults.get("opacity", 1.0)) * 100.0));
		TetraSchema::add_param(schema, TetraSchema::set_ui_range(TetraSchema::param_float("size", TTR("Size"), defaults.get("size", 20.0), 1.0, 1000.0, 0.1, "px"), 1.0, 200.0));
		if (p_tool != TOOL_PENCIL) {
			TetraSchema::add_param(schema, percent("hardness", TTR("Hardness"), double(defaults.get("hardness", 0.5)) * 100.0));
		}
		TetraSchema::add_param(schema, TetraSchema::set_description(percent("flow", TTR("Flow"), double(defaults.get("flow", 1.0)) * 100.0), TTR("How much paint one dab of the brush lays down.")));
		TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_float("spacing", TTR("Spacing"), defaults.get("spacing", 10.0), 1.0, 200.0, 0.1, "%"), TTR("Distance between dabs as a percentage of the brush size.")));
		if (p_tool == TOOL_SMUDGE) {
			TetraSchema::add_param(schema, TetraSchema::set_description(percent("rate", TTR("Rate"), double(defaults.get("rate", 0.5)) * 100.0), TTR("How much colour the brush carries along.")));
		}
		if (p_tool == TOOL_CLONE) {
			TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_bool("aligned", TTR("Aligned"), false), TTR("Keep the distance between source and brush from stroke to stroke. Otherwise every stroke starts again at the source point.")));
		}
		TetraSchema::add_param(schema, TetraSchema::set_group(TetraSchema::param_bool("pressure_size", TTR("Pressure Changes Size"), false), TTR("Pen")));
		TetraSchema::add_param(schema, TetraSchema::set_group(TetraSchema::param_bool("pressure_opacity", TTR("Pressure Changes Opacity"), false), TTR("Pen")));
		return schema;
	}

	switch (p_tool) {
		case TOOL_BUCKET_FILL: {
			Array fills;
			fills.push_back(TetraSchema::make_option("fg", TTR("Foreground colour")));
			fills.push_back(TetraSchema::make_option("bg", TTR("Background colour")));
			TetraSchema::add_param(schema, TetraSchema::param_enum("fill", TTR("Fill With"), "fg", fills));
			TetraSchema::add_param(schema, threshold_param());
			TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_bool("contiguous", TTR("Contiguous Region"), true), TTR("Fill only the connected area. Otherwise every similar pixel of the layer is filled.")));
			TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_bool("sample_merged", TTR("Sample Merged"), false), TTR("Compare the colours of the visible image instead of the active layer.")));
		} break;
		case TOOL_GRADIENT: {
			Array types;
			const char *type_labels[] = { TTRC("Linear"), TTRC("Bi-linear"), TTRC("Radial"), TTRC("Square"), TTRC("Conical (symmetric)"), TTRC("Conical (asymmetric)"), TTRC("Spiral (clockwise)"), TTRC("Spiral (counter-clockwise)") };
			for (int i = 0; i < 8; i++) {
				types.push_back(TetraSchema::make_option(i, TTRGET(type_labels[i])));
			}
			TetraSchema::add_param(schema, TetraSchema::param_enum("type", TTR("Shape"), int(TetraImageDocument::GRADIENT_LINEAR), types));
			Array repeats;
			repeats.push_back(TetraSchema::make_option(int(TetraImageDocument::REPEAT_NONE), TTR("None")));
			repeats.push_back(TetraSchema::make_option(int(TetraImageDocument::REPEAT_SAWTOOTH), TTR("Sawtooth wave")));
			repeats.push_back(TetraSchema::make_option(int(TetraImageDocument::REPEAT_TRIANGULAR), TTR("Triangular wave")));
			TetraSchema::add_param(schema, TetraSchema::param_enum("repeat", TTR("Repeat"), int(TetraImageDocument::REPEAT_NONE), repeats));
			TetraSchema::add_param(schema, percent("opacity", TTR("Opacity"), 100.0));
			TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_bool("reverse", TTR("Reverse"), false), TTR("Run from the background to the foreground colour.")));
		} break;
		case TOOL_COLOR_PICKER: {
			Array targets;
			targets.push_back(TetraSchema::make_option("fg", TTR("Set the foreground colour")));
			targets.push_back(TetraSchema::make_option("bg", TTR("Set the background colour")));
			TetraSchema::add_param(schema, TetraSchema::param_enum("target", TTR("Pick Mode"), "fg", targets));
			TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_bool("sample_merged", TTR("Sample Merged"), true), TTR("Pick from the visible image instead of the active layer.")));
			TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_int("radius", TTR("Sample Average"), 0, 0, 30, 1, "px"), TTR("Average the colours within this distance of the pointer.")));
		} break;
		case TOOL_RECT_SELECT: {
			TetraSchema::add_param(schema, select_mode_param());
			TetraSchema::add_param(schema, feather_param());
		} break;
		case TOOL_ELLIPSE_SELECT: {
			TetraSchema::add_param(schema, select_mode_param());
			TetraSchema::add_param(schema, TetraSchema::param_bool("antialias", TTR("Antialiasing"), true));
			TetraSchema::add_param(schema, feather_param());
		} break;
		case TOOL_FUZZY_SELECT: {
			TetraSchema::add_param(schema, select_mode_param());
			TetraSchema::add_param(schema, threshold_param());
			TetraSchema::add_param(schema, TetraSchema::set_description(TetraSchema::param_bool("sample_merged", TTR("Sample Merged"), false), TTR("Compare the colours of the visible image instead of the active layer.")));
			TetraSchema::add_param(schema, feather_param());
		} break;
		case TOOL_ZOOM: {
			Array directions;
			directions.push_back(TetraSchema::make_option("in", TTR("Zoom in")));
			directions.push_back(TetraSchema::make_option("out", TTR("Zoom out")));
			TetraSchema::add_param(schema, TetraSchema::param_enum("direction", TTR("Direction"), "in", directions));
		} break;
		default:
			break;
	}
	return schema;
}
