#pragma once

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

// A small XML document tree, enough for MLT XML and Kdenlive's asset files.
struct TetraXmlNode {
	String name;
	HashMap<String, String> attributes;
	// The text directly inside the element, without that of child elements.
	String text;
	Vector<int> children;

	String get_attribute(const String &p_name, const String &p_default = String()) const {
		const String *value = attributes.getptr(p_name);
		return value ? *value : p_default;
	}
};

// Parses p_xml into r_nodes. Node 0 is a synthetic root holding the top-level
// elements. Fails with a message when tags do not match or are left open.
Error tetra_parse_xml(const String &p_xml, Vector<TetraXmlNode> &r_nodes, String &r_error);

// The first child element of p_node called p_name, or -1.
int tetra_xml_find_child(const Vector<TetraXmlNode> &p_nodes, int p_node, const String &p_name);
