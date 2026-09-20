#define NOMINMAX

#include "resource.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

#include <d3d11.h>
#include <windows.h>
#include <commdlg.h>
#include <vector>
#include <string>
#include <fstream>
#include <cstdint>
#include <algorithm>
#include <cstdio>

enum class FieldType { Hex, Str, I8, U8, I16, U16, I32, U32, I64, U64 };

struct FieldDef {
    std::string name;
    std::string description;
    uint64_t offset = 0;
    FieldType type = FieldType::Hex;
    int length = 0;
    std::string group;
    std::vector<char> editBuffer;
};

struct TableItem {
    bool isGroupStart = false;
    bool isGroupEnd = false;
    int fieldIndex = -1;
    std::string groupName;
};

static std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

struct ParseResult {
    std::vector<FieldDef> fields;
    std::vector<TableItem> items;
    std::string errors;
};

static ParseResult ParseDefinitionFile(const std::string& path) {
    ParseResult result;
    std::ifstream file(path);
    if (!file.is_open()) {
        result.errors = "Could not open file:\n" + path;
        return result;
    }

    std::string line;
    int lineNum = 0;
    while (std::getline(file, line)) {
        lineNum++;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line.substr(0, 2) == "//") continue;

        if (line.find("@group") == 0) {
            std::string rest = Trim(line.substr(6));
            if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"')
                rest = rest.substr(1, rest.size() - 2);

            TableItem item;
            item.isGroupStart = true;
            item.groupName = rest;
            result.items.push_back(item);
            continue;
        }

        if (line.find("@endgroup") == 0) {
            TableItem item;
            item.isGroupEnd = true;
            result.items.push_back(item);
            continue;
        }

        std::vector<std::string> parts;
        std::string current;
        bool inQuotes = false;
        for (char c : line) {
            if (c == '"') inQuotes = !inQuotes;
            else if (c == ',' && !inQuotes) { parts.push_back(Trim(current)); current.clear(); }
            else current += c;
        }
        parts.push_back(Trim(current));

        if (parts.size() < 4) {
            result.errors += "Line " + std::to_string(lineNum) + ": Expected 4 columns (name, desc, offset, type).\n";
            continue;
        }

        FieldDef f;
        f.name = parts[0];
        f.description = parts[1];

        std::string offStr = parts[2];
        try {
            size_t pos = 0;
            if (offStr.find("0x") == 0 || offStr.find("0X") == 0)
                f.offset = std::stoull(offStr, &pos, 16);
            else
                f.offset = std::stoull(offStr, &pos, 10);

            if (pos != offStr.length()) throw std::invalid_argument("Trailing characters");
        }
        catch (...) {
            result.errors += "Line " + std::to_string(lineNum) + ": Invalid offset '" + offStr + "'.\n";
            continue;
        }

        std::string typeStr = parts[3];
        std::transform(typeStr.begin(), typeStr.end(), typeStr.begin(), ::tolower);

        try {
            if (typeStr.find("hex(") == 0 && typeStr.back() == ')') {
                f.type = FieldType::Hex;
                f.length = std::stoi(typeStr.substr(4, typeStr.length() - 5));
            }
            else if (typeStr.find("str(") == 0 && typeStr.back() == ')') {
                f.type = FieldType::Str;
                f.length = std::stoi(typeStr.substr(4, typeStr.length() - 5));
            }
            else if (typeStr == "i8") { f.type = FieldType::I8;  f.length = 1; }
            else if (typeStr == "u8") { f.type = FieldType::U8;  f.length = 1; }
            else if (typeStr == "i16") { f.type = FieldType::I16; f.length = 2; }
            else if (typeStr == "u16") { f.type = FieldType::U16; f.length = 2; }
            else if (typeStr == "i32") { f.type = FieldType::I32; f.length = 4; }
            else if (typeStr == "u32") { f.type = FieldType::U32; f.length = 4; }
            else if (typeStr == "i64") { f.type = FieldType::I64; f.length = 8; }
            else if (typeStr == "u64") { f.type = FieldType::U64; f.length = 8; }
            else {
                result.errors += "Line " + std::to_string(lineNum) + ": Unknown type '" + typeStr + "'.\n";
                continue;
            }
        }
        catch (...) {
            result.errors += "Line " + std::to_string(lineNum) + ": Invalid type/length format '" + typeStr + "'.\n";
            continue;
        }

        int bufSize = (f.type == FieldType::Hex) ? (f.length * 2 + 1) : (f.length + 1);
        f.editBuffer.resize(bufSize, '\0');

        TableItem item;
        item.fieldIndex = (int)result.fields.size();
        result.items.push_back(item);
        result.fields.push_back(f);
    }
    return result;
}

struct AppState {
    std::vector<uint8_t> fileData;
    std::vector<FieldDef> fields;
    std::vector<TableItem> tableItems;
    std::string binaryPath;
    int selectedFieldIndex = -1;
    int editingFieldIndex = -1;
    bool dataModified = false;

    void LoadBinary(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        fileData.resize((size_t)size);
        file.read(reinterpret_cast<char*>(fileData.data()), size);
        binaryPath = path;
        dataModified = false;
        SyncFieldsFromData();
    }

    void SaveBinary() {
        if (binaryPath.empty()) return;
        std::ofstream file(binaryPath, std::ios::binary);
        file.write(reinterpret_cast<const char*>(fileData.data()), fileData.size());
        dataModified = false;
    }

    void SyncFieldsFromData() {
        for (auto& f : fields) {
            if (f.offset + f.length > fileData.size()) continue;
            uint8_t* ptr = fileData.data() + f.offset;

            if (f.type == FieldType::Hex) {
                std::string hexStr;
                for (int i = 0; i < f.length; i++) {
                    char buf[3];
                    sprintf_s(buf, "%02X", ptr[i]);
                    hexStr += buf;
                }
                strncpy_s(f.editBuffer.data(), f.editBuffer.size(), hexStr.c_str(), _TRUNCATE);
            }
            else if (f.type == FieldType::Str) {
                std::string str(reinterpret_cast<char*>(ptr), f.length);
                str.erase(std::find(str.begin(), str.end(), '\0'), str.end());
                strncpy_s(f.editBuffer.data(), f.editBuffer.size(), str.c_str(), _TRUNCATE);
            }
        }
    }
};

static AppState g_AppState;

static void DrawHexView() {
    ImGui::BeginChild("HexView", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

    if (g_AppState.fileData.empty()) {
        ImGui::Text("No binary file loaded.");
        ImGui::EndChild();
        return;
    }

    const int bytesPerLine = 16;
    int totalLines = (int)((g_AppState.fileData.size() + bytesPerLine - 1) / bytesPerLine);

    static int lastSelectedIndex = -1;
    if (g_AppState.selectedFieldIndex != lastSelectedIndex) {
        lastSelectedIndex = g_AppState.selectedFieldIndex;
        if (g_AppState.selectedFieldIndex >= 0 && g_AppState.selectedFieldIndex < (int)g_AppState.fields.size()) {
            uint64_t offset = g_AppState.fields[g_AppState.selectedFieldIndex].offset;
            int targetLine = (int)(offset / bytesPerLine);

            float lineHeight = ImGui::GetTextLineHeightWithSpacing();
            float viewHeight = ImGui::GetWindowHeight();
            float scrollY = targetLine * lineHeight - viewHeight * 0.5f + lineHeight * 0.5f;
            if (scrollY < 0.0f) scrollY = 0.0f;

            ImGui::SetScrollY(scrollY);
        }
    }

    ImGuiListClipper clipper;
    clipper.Begin(totalLines);
    while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; line++) {
            uint64_t lineOffset = (uint64_t)line * bytesPerLine;

            ImGui::TextColored(ImVec4(0.5f, 0.5f, 1.0f, 1.0f), "%08llX: ", lineOffset);
            ImGui::SameLine(0, 0);

            for (int i = 0; i < bytesPerLine; i++) {
                uint64_t byteOffset = lineOffset + i;
                if (byteOffset >= g_AppState.fileData.size()) {
                    ImGui::TextUnformatted("   ");
                }
                else {
                    uint8_t byte = g_AppState.fileData[(size_t)byteOffset];
                    bool isHighlighted = false;

                    if (g_AppState.selectedFieldIndex >= 0 && g_AppState.selectedFieldIndex < (int)g_AppState.fields.size()) {
                        const auto& f = g_AppState.fields[g_AppState.selectedFieldIndex];
                        if (byteOffset >= f.offset && byteOffset < f.offset + f.length) {
                            isHighlighted = true;
                        }
                    }

                    if (isHighlighted) {
                        ImVec2 p_min = ImGui::GetCursorScreenPos();
                        float charWidth = ImGui::CalcTextSize("0").x;
                        ImVec2 p_max(p_min.x + charWidth * 3.0f, p_min.y + ImGui::GetTextLineHeight());
                        ImGui::GetWindowDrawList()->AddRectFilled(p_min, p_max, IM_COL32(255, 255, 0, 100));
                    }
                    ImGui::Text("%02X ", byte);
                }
                ImGui::SameLine(0, 0);
                if (i == 7) { ImGui::TextUnformatted(" "); ImGui::SameLine(0, 0); }
            }

            ImGui::TextUnformatted(" |");
            ImGui::SameLine(0, 0);

            for (int i = 0; i < bytesPerLine; i++) {
                uint64_t byteOffset = lineOffset + i;
                if (byteOffset >= g_AppState.fileData.size()) {
                    ImGui::TextUnformatted(" ");
                }
                else {
                    uint8_t byte = g_AppState.fileData[(size_t)byteOffset];
                    char c = (byte >= 32 && byte < 127) ? (char)byte : '.';

                    bool isHighlighted = false;
                    if (g_AppState.selectedFieldIndex >= 0 && g_AppState.selectedFieldIndex < (int)g_AppState.fields.size()) {
                        const auto& f = g_AppState.fields[g_AppState.selectedFieldIndex];
                        if (byteOffset >= f.offset && byteOffset < f.offset + f.length) {
                            isHighlighted = true;
                        }
                    }

                    if (isHighlighted) {
                        ImVec2 p_min = ImGui::GetCursorScreenPos();
                        float charWidth = ImGui::CalcTextSize("0").x;
                        ImVec2 p_max(p_min.x + charWidth, p_min.y + ImGui::GetTextLineHeight());
                        ImGui::GetWindowDrawList()->AddRectFilled(p_min, p_max, IM_COL32(100, 200, 255, 100));
                    }
                    ImGui::Text("%c", c);
                }
                ImGui::SameLine(0, 0);
            }
            ImGui::TextUnformatted("|");
        }
    }
    clipper.End();
    ImGui::EndChild();
}

static void DrawTable() {
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("FieldsTable", 5, flags, ImVec2(0, 300))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthFixed, 250);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Value / Edit", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableHeadersRow();

        auto DrawFieldRow = [&](int i) {
            ImGui::PushID(i);

            auto& f = g_AppState.fields[i];

            ImGui::TableNextRow();
            bool isSelected = (g_AppState.selectedFieldIndex == i);

            ImGui::TableSetColumnIndex(0);

            if (ImGui::Selectable(f.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                if (g_AppState.selectedFieldIndex != i) {
                    g_AppState.editingFieldIndex = -1;
                }
                g_AppState.selectedFieldIndex = i;
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.description.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("0x%llX", f.offset);

            ImGui::TableNextColumn();
            if (f.type == FieldType::Hex) ImGui::Text("hex(%d)", f.length);
            else if (f.type == FieldType::Str) ImGui::Text("str(%d)", f.length);
            else {
                char prefix = ((int)f.type % 2 == 0) ? 'i' : 'u';
                ImGui::Text("%c%d", prefix, f.length * 8);
            }

            ImGui::TableSetColumnIndex(4);
            bool isEditing = (g_AppState.editingFieldIndex == i);

            if (f.offset + f.length > g_AppState.fileData.size()) {
                ImGui::TextDisabled("<Out of bounds>");
            }
            else if (isEditing) {
                uint8_t* ptr = g_AppState.fileData.data() + f.offset;
                ImGui::SetNextItemWidth(-FLT_MIN);

                if (f.type == FieldType::Hex) {
                    ImGui::InputText("##hex", f.editBuffer.data(), f.editBuffer.size(), ImGuiInputTextFlags_CharsHexadecimal);
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        std::string hexStr(f.editBuffer.data());
                        if (hexStr.length() == (size_t)(f.length * 2)) {
                            bool valid = true;
                            for (int b = 0; b < f.length; b++) {
                                try { ptr[b] = (uint8_t)std::stoi(hexStr.substr(b * 2, 2), nullptr, 16); }
                                catch (...) { valid = false; break; }
                            }
                            if (valid) g_AppState.dataModified = true;
                        }
                        g_AppState.editingFieldIndex = -1;
                    }
                }
                else if (f.type == FieldType::Str) {
                    ImGui::InputText("##str", f.editBuffer.data(), f.editBuffer.size());
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        std::string str(f.editBuffer.data());
                        memset(ptr, 0, f.length);
                        memcpy(ptr, str.c_str(), std::min((int)str.length(), f.length));
                        g_AppState.dataModified = true;
                        g_AppState.editingFieldIndex = -1;
                    }
                }
                else {
                    ImGuiDataType imguiType;
                    switch (f.type) {
                    case FieldType::I8:  imguiType = ImGuiDataType_S8;  break;
                    case FieldType::U8:  imguiType = ImGuiDataType_U8;  break;
                    case FieldType::I16: imguiType = ImGuiDataType_S16; break;
                    case FieldType::U16: imguiType = ImGuiDataType_U16; break;
                    case FieldType::I32: imguiType = ImGuiDataType_S32; break;
                    case FieldType::U32: imguiType = ImGuiDataType_U32; break;
                    case FieldType::I64: imguiType = ImGuiDataType_S64; break;
                    case FieldType::U64: imguiType = ImGuiDataType_U64; break;
                    default:             imguiType = ImGuiDataType_S32; break;
                    }
                    ImGui::InputScalar("##int", imguiType, (void*)ptr);
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        g_AppState.dataModified = true;
                        g_AppState.editingFieldIndex = -1;
                    }
                }
            }
            else {
                if (f.type == FieldType::Hex || f.type == FieldType::Str) {
                    ImGui::TextUnformatted(f.editBuffer.data());
                }
                else {
                    uint8_t* ptr = g_AppState.fileData.data() + f.offset;
                    char intStr[32] = {};
                    switch (f.type) {
                    case FieldType::I8:  snprintf(intStr, sizeof(intStr), "%d", (int)*(int8_t*)ptr); break;
                    case FieldType::U8:  snprintf(intStr, sizeof(intStr), "%u", (unsigned)*(uint8_t*)ptr); break;
                    case FieldType::I16: snprintf(intStr, sizeof(intStr), "%d", (int)*(int16_t*)ptr); break;
                    case FieldType::U16: snprintf(intStr, sizeof(intStr), "%u", (unsigned)*(uint16_t*)ptr); break;
                    case FieldType::I32: snprintf(intStr, sizeof(intStr), "%d", *(int32_t*)ptr); break;
                    case FieldType::U32: snprintf(intStr, sizeof(intStr), "%u", *(uint32_t*)ptr); break;
                    case FieldType::I64: snprintf(intStr, sizeof(intStr), "%lld", (long long)*(int64_t*)ptr); break;
                    case FieldType::U64: snprintf(intStr, sizeof(intStr), "%llu", (unsigned long long) * (uint64_t*)ptr); break;
                    default: break;
                    }
                    ImGui::TextUnformatted(intStr);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Edit")) {
                    g_AppState.selectedFieldIndex = i;
                    g_AppState.editingFieldIndex = i;
                    g_AppState.SyncFieldsFromData();
                }
            }

            ImGui::PopID();

        };

        int currentGroupDepth = 0;
        int openGroupDepth = 0;

        for (const auto& item : g_AppState.tableItems) {
            if (item.isGroupStart) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                bool open = ImGui::TreeNodeEx(item.groupName.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
                if (open) openGroupDepth++;
                currentGroupDepth++;
            }
            else if (item.isGroupEnd) {
                if (currentGroupDepth > 0) {
                    currentGroupDepth--;
                    if (openGroupDepth > currentGroupDepth) {
                        ImGui::TreePop();
                        openGroupDepth--;
                    }
                }
            }
            else {
                if (openGroupDepth == currentGroupDepth) {
                    DrawFieldRow(item.fieldIndex);
                }
            }
        }

        ImGui::EndTable();
    }
}

static std::string OpenFileDialog(HWND hwnd, const char* filter) {
    char filename[MAX_PATH] = { 0 };
    OPENFILENAMEA ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) return std::string(filename);
    return "";
}

static std::string SaveFileDialog(HWND hwnd, const char* filter) {
    char filename[MAX_PATH] = { 0 };
    OPENFILENAMEA ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameA(&ofn)) return std::string(filename);
    return "";
}

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        GetModuleHandle(nullptr),
        LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)),
        nullptr,
        nullptr,
        nullptr,
        L"ImGui Example",
        LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1))
    };

    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName,
        L"rapid-struct",
        WS_OVERLAPPEDWINDOW,
        100, 100,
        1280, 800,
        nullptr, nullptr,
        wc.hInstance,
        nullptr
    );

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(ImVec2(1200, 750), ImGuiCond_FirstUseEver);
        ImGui::Begin("rapid-struct");

        if (ImGui::Button("Open Binary")) {
            std::string path = OpenFileDialog(hwnd, "Binary Files\0*.*\0");
            if (!path.empty()) g_AppState.LoadBinary(path);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Definition")) {
            std::string path = OpenFileDialog(hwnd, "Rapid Struct Definition (*.rs)\0*.rs\0");
            if (!path.empty()) {
                auto res = ParseDefinitionFile(path);
                g_AppState.fields = std::move(res.fields);
                g_AppState.tableItems = std::move(res.items);
                g_AppState.SyncFieldsFromData();

                if (!res.errors.empty()) {
                    MessageBoxA(hwnd, res.errors.c_str(), "Definition Parse Errors", MB_OK | MB_ICONWARNING);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save Binary")) {
            if (g_AppState.binaryPath.empty()) {
                std::string path = SaveFileDialog(hwnd, "Binary Files\0*.*\0");
                if (!path.empty()) g_AppState.binaryPath = path;
            }
            if (!g_AppState.binaryPath.empty()) g_AppState.SaveBinary();
        }

        if (g_AppState.dataModified) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "[Modified]");
        }

        ImGui::Separator();

        ImVec2 content_avail = ImGui::GetContentRegionAvail();

        ImGui::BeginChild("TableArea", ImVec2(content_avail.x, content_avail.y * 0.45f), true);
        DrawTable();
        ImGui::EndChild();

        ImGui::BeginChild("HexArea", ImVec2(content_avail.x, 0), true);
        DrawHexView();
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.15f, 0.15f, 0.15f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}