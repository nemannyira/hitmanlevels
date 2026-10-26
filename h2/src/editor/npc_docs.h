#pragma once
// Descriptions of NPC behaviours (SDL script classes) and their parameters,
// for tooltips and the info box of the NPCs tab. The scripts are compiled, so
// the meaning comes from how the levels use them; descriptions derived only
// from a name are marked as such.
#include <string>

namespace h2::editor {

struct ParamDoc {
    std::string text;
    bool inferred = false;  // made from the name, not checked against the levels
};

ParamDoc DescribeParam(const std::string& name, const std::string& type);
std::string DescribeBehaviour(const std::string& shortName);
// A setting's name for people ("Hit points" for m_iHitpoints); its own name when it has none.
std::string ParamLabel(const std::string& name);

} // namespace h2::editor
