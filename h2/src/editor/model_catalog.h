#pragma once
// Sorting the models of a scene into categories and families by their names
// (the scene files have no such information: every model hangs under the
// same groups). Names follow the level designers' habits, e.g.
// "Wall_01b_05", "MWall_01_Crne", "Lamp_03_Bulb_ON": words split on "_",
// digits and CamelCase are matched against keyword lists.
#include <string>

namespace h2::editor {

enum class Category {
    Pickup,   // working items made from the scene's item templates
    Architecture,
    Furniture,
    Lighting,
    Nature,
    Vehicles,
    Items,    // weapons, ammo, tools, keys
    Props,    // containers, kitchenware, signs, small things
    Effects,  // particles, blood, debris, destruction stages
    Other,
    Interface,  // menus, HUD, map screen: objects outside the rooms of the level
    Helpers,    // invisible technical objects: portals, bounds, collision, AI points
    Count
};

const char* CategoryName(Category c);
const char* CategoryKey(Category c);  // short stable name (saved in files)

// The user's corrections (right click in the Models list), by model name or
// family, kept in Mods\model_categories.json. Category::Count clears one.
bool CategoryOverride(const std::string& key, Category* out);
void SetCategoryOverride(const std::string& key, Category c);
Category Categorize(const std::string& name, const std::string& className);

// "Wall_01b" -> "Wall", "MWall_01_Crne" -> "MWall", "Lamp03Bulb" -> "Lamp"
std::string FamilyOf(const std::string& name);

} // namespace h2::editor
