#include "console.h"

#include <cctype>
#include <string>
#include <vector>

#include "imgui.h"

#include "../script/lua_vm.h"

namespace h2::overlay {

namespace {

bool g_visible = false;
char g_input[1024];
std::vector<std::string> g_history;
int g_historyPos = -1;  // -1: editing a new line
size_t g_lastLineCount;  // ConsoleSerial when the output was last followed
bool g_focusInput;

// Tab: the name before the cursor completed ("h2.pl" -> "h2.player(", "obj:po" -> "obj:pos("); several: as far as
// they agree, and the choices printed
void Complete(ImGuiInputTextCallbackData* data)
{
    int end = data->CursorPos, start = end;
    auto nameChar = [](char ch) { return isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.'; };
    while (start > 0 && nameChar(data->Buf[start - 1]))
        --start;
    std::string word(data->Buf + start, data->Buf + end);
    bool method = start > 0 && data->Buf[start - 1] == ':';
    size_t dot = word.rfind('.');
    if (method && dot != std::string::npos)
        return;
    std::string path = dot == std::string::npos ? "" : word.substr(0, dot);
    std::string partial = dot == std::string::npos ? word : word.substr(dot + 1);
    if (!path.empty() && !isalpha(static_cast<unsigned char>(path[0])) && path[0] != '_')
        return;  // a number ("1.5")
    std::vector<std::string> names = script::Complete(path, partial, method);
    if (names.empty())
        return;
    std::string common = names[0];
    for (const std::string& n : names) {
        size_t i = 0;
        while (i < common.size() && i < n.size() && common[i] == n[i])
            ++i;
        common.resize(i);
    }
    if (common.size() > partial.size()) {
        int at = end - int(partial.size());
        data->DeleteChars(at, int(partial.size()));
        data->InsertChars(at, common.c_str());
    }
    if (names.size() > 1) {
        std::string list;
        for (size_t i = 0; i < names.size() && i < 60; ++i)
            list += (i ? "  " : "") + names[i];
        if (names.size() > 60)
            list += "  ... (" + std::to_string(names.size()) + ")";
        script::ConsolePrint(script::LineKind::Output, list);
    }
}

int InputCallback(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        Complete(data);
        return 0;
    }
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || g_history.empty())
        return 0;
    int prev = g_historyPos;
    if (data->EventKey == ImGuiKey_UpArrow)
        g_historyPos = g_historyPos < 0 ? int(g_history.size()) - 1 : (g_historyPos > 0 ? g_historyPos - 1 : 0);
    else if (data->EventKey == ImGuiKey_DownArrow && g_historyPos >= 0)
        g_historyPos = g_historyPos + 1 < int(g_history.size()) ? g_historyPos + 1 : -1;
    if (prev != g_historyPos) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, g_historyPos >= 0 ? g_history[g_historyPos].c_str() : "");
    }
    return 0;
}

} // namespace

bool* ConsoleVisible() { return &g_visible; }

void FocusConsoleInput() { g_focusInput = true; }

void DrawConsole()
{
    ImGui::TextDisabled("Lua console");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        script::ClearConsole();
    ImGui::SameLine();
    if (ImGui::SmallButton("Close (`)"))
        g_visible = false;
    ImGui::Separator();

    float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("output", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        const auto& lines = script::Console();
        if (lines.empty())
            ImGui::TextDisabled("Lua: h2.* calls (h2/docs/lua.md). Tab completes a name, Up / Down the earlier lines.");
        for (const auto& l : lines) {
            ImVec4 col = l.kind == script::LineKind::Error   ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                         : l.kind == script::LineKind::Input ? ImVec4(0.6f, 0.8f, 1.0f, 1.0f)
                                                             : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
        }
        if (script::ConsoleSerial() != g_lastLineCount) {  // follow new output
            g_lastLineCount = script::ConsoleSerial();
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::Separator();

    if (script::Untrusted()) {  // (the state holds a downloaded level's globals: what is typed would run among them)
        ImGui::TextDisabled("The console is off while a downloaded level runs (its scripts share the Lua state).");
        return;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll |
                                ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##input", g_input, sizeof(g_input), flags, InputCallback)) {
        std::string code = g_input;
        if (!code.empty()) {
            g_history.push_back(code);
            g_historyPos = -1;
            script::ExecConsole(code);
        }
        g_input[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (g_focusInput) {
        g_focusInput = false;
        ImGui::SetWindowFocus();
        ImGui::SetKeyboardFocusHere(-1);
    }
}

} // namespace h2::overlay
