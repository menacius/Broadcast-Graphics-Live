#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::string read_file(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

bool require(const std::string &text, const std::string &needle, const char *label)
{
    if (text.find(needle) != std::string::npos)
        return true;
    std::cerr << "Missing group-container contract: " << label << " (" << needle << ")\n";
    return false;
}
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        std::cerr << "usage: group_container_contract_test <model> <data> <editor> "
                     "<layers> <canvas> <source>\n";
        return 2;
    }
    const std::string model = read_file(argv[1]);
    const std::string data = read_file(argv[2]);
    const std::string editor = read_file(argv[3]);
    const std::string layers = read_file(argv[4]);
    const std::string canvas = read_file(argv[5]);
    const std::string source = read_file(argv[6]);

    bool ok = true;
    ok &= require(model, "Group = 8", "persisted group layer type");
    ok &= require(model, "group_collapsed", "group collapse state");
    ok &= require(data, "j[\"group_collapsed\"]", "group collapse serialization");
    ok &= require(data, "LayerType::Group", "group deserialization range");

    ok &= require(editor, "void TitleEditor::group_selected_layers()", "group command");
    ok &= require(editor, "void TitleEditor::ungroup_selected_layers()", "ungroup command");
    ok &= require(editor, "add_selected_layers_to_group", "add-to-group command");
    ok &= require(editor, "editor_top_level_selected_layers", "nested selection roots");
    ok &= require(editor, "layers_->set_selected_layer(group->id)", "new group remains selected");
    ok &= require(editor, "editor_selection_with_group_descendants", "group copy/delete integrity");

    ok &= require(layers, "visible_layer_hierarchy_rows", "hierarchical layer rows");
    ok &= require(layers, "group_collapsed_changed", "group caret behavior");
    ok &= require(layers, "add_to_group_requested", "add-to-group UI");
    ok &= require(layers, "remove_from_group_requested", "remove-from-group UI");

    ok &= require(canvas, "layer.type == LayerType::Group", "dynamic group canvas bounds");
    ok &= require(canvas, "editor_outermost_group_ancestor", "canvas selects group container");
    ok &= require(canvas, "editor_layer_has_ancestor(title_, *candidate, layer.id)",
                  "group bounds follow descendants");

    ok &= require(source, "if (layer.type == LayerType::Group)",
                  "group has no empty compositor raster");
    ok &= require(source, "children remain independent GPU layers and text never",
                  "text child renderer contract");

    return ok ? 0 : 1;
}
