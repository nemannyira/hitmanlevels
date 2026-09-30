#include "console.h"

#include <string>
#include <vector>

#include "imgui.h"

#include "../script/lua_vm.h"

namespace h2::overlay {

namespace {

bool g_visible = true;
char g_input[1024];
std::vector<std::string> g_history;
int g_historyPos = -1;  // -1: editing a new line
size_t g_lastLineCount;
bool g_focusInput;

int HistoryCallback(ImGuiInputTextCallbackData* data)
{
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
    ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Lua console", &g_visible)) {
        ImGui::End();
        return;
    }
    if (ImGui::SmallButton("Clear"))
        script::ClearConsole();
    ImGui::Separator();

    float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("output", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        const auto& lines = script::Console();
        for (const auto& l : lines) {
            ImVec4 col = l.kind == script::LineKind::Error   ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                         : l.kind == script::LineKind::Input ? ImVec4(0.6f, 0.8f, 1.0f, 1.0f)
                                                             : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
        }
        if (lines.size() != g_lastLineCount) {  // follow new output
            g_lastLineCount = lines.size();
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::Separator();

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll |
                                ImGuiInputTextFlags_CallbackHistory;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##input", g_input, sizeof(g_input), flags, HistoryCallback)) {
        std::string code = g_input;
        if (!code.empty()) {
            g_history.push_back(code);
            g_historyPos = -1;
            script::ExecConsole(code);
        }
        g_input[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (g_focusInput || ImGui::IsWindowAppearing()) {
        g_focusInput = false;
        ImGui::SetWindowFocus();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::End();
}

} // namespace h2::overlay
