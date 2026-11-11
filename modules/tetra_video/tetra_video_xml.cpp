#include "tetra_video_xml.h"

#include "core/io/xml_parser.h"

Error tetra_parse_xml(const String &p_xml, Vector<TetraXmlNode> &r_nodes, String &r_error) {
	const CharString utf8 = p_xml.utf8();
	Ref<XMLParser> parser;
	parser.instantiate();
	if (utf8.length() == 0 || parser->_open_buffer((const uint8_t *)utf8.get_data(), utf8.length()) != OK) {
		r_error = "The document is empty.";
		return ERR_PARSE_ERROR;
	}
	Vector<int> stack;
	r_nodes.clear();
	r_nodes.push_back(TetraXmlNode());
	stack.push_back(0);
	while (parser->read() == OK) {
		switch (parser->get_node_type()) {
			case XMLParser::NODE_ELEMENT: {
				TetraXmlNode node;
				node.name = parser->get_node_name();
				for (int i = 0; i < parser->get_attribute_count(); i++) {
					node.attributes[parser->get_attribute_name(i)] = parser->get_attribute_value(i);
				}
				const int index = r_nodes.size();
				r_nodes.push_back(node);
				r_nodes.write[stack[stack.size() - 1]].children.push_back(index);
				if (!parser->is_empty()) {
					stack.push_back(index);
				}
			} break;
			case XMLParser::NODE_ELEMENT_END: {
				if (stack.size() < 2 || r_nodes[stack[stack.size() - 1]].name != parser->get_node_name()) {
					r_error = vformat("Malformed XML: unexpected closing tag </%s> at line %d.", parser->get_node_name(), parser->get_current_line() + 1);
					return ERR_PARSE_ERROR;
				}
				stack.resize(stack.size() - 1);
			} break;
			case XMLParser::NODE_TEXT: {
				r_nodes.write[stack[stack.size() - 1]].text += parser->get_node_data();
			} break;
			case XMLParser::NODE_CDATA: {
				// XMLParser hands out the content of a CDATA section as the node's name.
				r_nodes.write[stack[stack.size() - 1]].text += parser->get_node_name();
			} break;
			default:
				break;
		}
	}
	if (stack.size() > 1) {
		r_error = vformat("Malformed XML: <%s> is never closed.", r_nodes[stack[stack.size() - 1]].name);
		return ERR_PARSE_ERROR;
	}
	return OK;
}

int tetra_xml_find_child(const Vector<TetraXmlNode> &p_nodes, int p_node, const String &p_name) {
	for (int child : p_nodes[p_node].children) {
		if (p_nodes[child].name == p_name) {
			return child;
		}
	}
	return -1;
}
