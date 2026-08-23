// log_client: Dear ImGui (Win32 + DirectX 11) front-end for the log
// analysis system. All network I/O lives in NetClient's worker thread;
// this file only pumps messages, draws the UI and polls transfer state,
// so the window stays responsive throughout the 500 MB upload.
//
// RAII note: D3D COM objects are held in Microsoft::WRL::ComPtr, sockets
// in UniqueSocket, the worker in std::thread joined by ~NetClient — the
// assignment's "no manual memory management" rule holds in every layer.

#include "NetClient.h"

#include <d3d11.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

// ---- DX11 state (ComPtr = automatic Release on every path) --------------
static ComPtr<ID3D11Device>           g_device;
static ComPtr<ID3D11DeviceContext>    g_context;
static ComPtr<IDXGISwapChain>         g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_rtv;

// consumed once per frame by the main loop.
static std::wstring g_droppedFile;

static void CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> back;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) g_device->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
}

static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_0};
    if (D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level,
            &g_context) != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    switch (m) {
    case WM_SIZE:
        if (g_device && w != SIZE_MINIMIZED) {
            g_rtv.Reset();
            g_swapChain->ResizeBuffers(0, LOWORD(l), HIWORD(l),
                                       DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(w);
        std::vector<wchar_t> buf(MAX_PATH * 4, L'\0');
        if (DragQueryFileW(drop, 0, buf.data(),
                           static_cast<UINT>(buf.size())))
            g_droppedFile = buf.data();
        DragFinish(drop);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU) return 0;   // no ALT menu beep
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- helpers ------------------------------------------------------------
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring pickOpenFile(HWND owner) {
    std::vector<wchar_t> buf(MAX_PATH * 4, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrFilter = L"Log files (*.log;*.txt)\0*.log;*.txt\0"
                      L"All files (*.*)\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return {};
    return buf.data();
}

static std::wstring pickSaveFile(HWND owner) {
    std::vector<wchar_t> buf(MAX_PATH * 4, L'\0');
    lstrcpyW(buf.data(), L"result.csv");
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return {};
    return buf.data();
}

// "482.8 MiB" / "3.2 KiB" for status strings.
static std::string prettySize(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1ull << 20))
        std::snprintf(buf, sizeof(buf), "%.1f MiB",
                      static_cast<double>(bytes) / (1 << 20));
    else
        std::snprintf(buf, sizeof(buf), "%.1f KiB",
                      static_cast<double>(bytes) / (1 << 10));
    return buf;
}

// Key figures pulled from the received result.csv for the summary panel.
struct CsvSummary {
    bool ok = false;
    std::string avgSpeed, parsedLines, skippedLines;
    std::vector<std::pair<std::string, std::string>> reasons;  // name, count
};

static CsvSummary parseSummary(const std::string& csv) {
    CsvSummary s;
    size_t pos = 0;
    while (pos < csv.size()) {
        size_t eol = csv.find('\n', pos);
        if (eol == std::string::npos) eol = csv.size();
        const std::string_view line(csv.data() + pos, eol - pos);
        pos = eol + 1;

        constexpr std::string_view kPrefix = "summary,";
        if (line.substr(0, kPrefix.size()) != kPrefix) continue;
        // summary,<name>,,<value>
        const std::string_view rest = line.substr(kPrefix.size());
        const size_t c1 = rest.find(',');
        if (c1 == std::string_view::npos) continue;
        const std::string_view name = rest.substr(0, c1);
        const size_t c2 = rest.find(',', c1 + 1);
        if (c2 == std::string_view::npos) continue;
        const std::string value(rest.substr(c2 + 1));

        constexpr std::string_view kSkipPrefix = "skipped_";
        if (name == "average_speed")      { s.avgSpeed = value; s.ok = true; }
        else if (name == "parsed_lines")  s.parsedLines = value;
        else if (name == "skipped_lines") s.skippedLines = value;
        else if (name.substr(0, kSkipPrefix.size()) == kSkipPrefix)
            s.reasons.emplace_back(std::string(name.substr(kSkipPrefix.size())),
                                   value);
    }
    return s;
}

// ---- application --------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    WsaSession wsa;
    if (!wsa.ok()) return 1;

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst,
                      nullptr, nullptr, nullptr, nullptr,
                      L"LogClientClass", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Log Analysis Client",
                              WS_OVERLAPPEDWINDOW, 100, 100, 760, 600,
                              nullptr, nullptr, inst, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        UnregisterClassW(wc.lpszClassName, inst);
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    // ---- app state ------------------------------------------------------
    using Clock = std::chrono::steady_clock;
    NetClient net;
    std::wstring logPath;                    // selected file (wide, for I/O)
    std::vector<char> pathBuf(1024, '\0');   // display copy (UTF-8)
    std::string fileSizeStr;
    std::vector<std::string> logLines;
    std::vector<char> hostBuf(64, '\0');
    std::string("127.0.0.1").copy(hostBuf.data(), hostBuf.size() - 1);
    int port = 5555;
    bool autoSave = true;

    // transfer-rate / timing state (UI-side, derived from polled atomics)
    XferState prevState = XferState::Idle;
    Clock::time_point xferStart{}, lastRateT{};
    uint64_t lastRateBytes = 0;
    double rateBps = 0.0;          // smoothed
    double finalElapsedSec = 0.0;
    CsvSummary summary;

    auto uiLog = [&](std::string msg) {
        logLines.push_back(std::move(msg));
    };

    auto selectFile = [&](const std::wstring& p) {
        logPath = p;
        const std::string u8 = narrow(p);
        std::fill(pathBuf.begin(), pathBuf.end(), '\0');
        u8.copy(pathBuf.data(), pathBuf.size() - 1);
        std::error_code ec;
        const uint64_t sz = std::filesystem::file_size(p, ec);
        fileSizeStr = ec ? std::string("size unknown") : prettySize(sz);
        uiLog("selected " + u8 + " (" + fileSizeStr + ")");
    };

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        const XferState st = net.state();
        const bool busy = net.busy();

        // ---- dropped file (ignored while a transfer is running) --------
        if (!g_droppedFile.empty()) {
            if (busy) uiLog("file drop ignored while a transfer is running");
            else      selectFile(g_droppedFile);
            g_droppedFile.clear();
        }

        // ---- state transitions -----------------------------------------
        if (st != prevState) {
            if (st == XferState::Connecting) {   // (re)started
                xferStart = lastRateT = Clock::now();
                lastRateBytes = 0;
                rateBps = 0.0;
                summary = {};
                finalElapsedSec = 0.0;
            }
            if (st == XferState::Done || st == XferState::Failed ||
                st == XferState::Cancelled) {
                finalElapsedSec =
                    std::chrono::duration<double>(Clock::now() - xferStart)
                        .count();
            }
            if (st == XferState::Done) {
                summary = parseSummary(net.resultCsv());
                if (autoSave && !logPath.empty()) {
                    const std::filesystem::path dst =
                        std::filesystem::path(logPath).parent_path() /
                        L"result.csv";
                    std::ofstream out(dst,
                                      std::ios::binary | std::ios::trunc);
                    out << net.resultCsv();
                    uiLog(out ? "auto-saved " + narrow(dst.wstring())
                              : "auto-save FAILED: " + narrow(dst.wstring()));
                }
            }
            prevState = st;
        }

        // ---- transfer rate (0.25 s sampling, light smoothing) ----------
        if (busy) {
            const auto now = Clock::now();
            const double dt =
                std::chrono::duration<double>(now - lastRateT).count();
            if (dt >= 0.25) {
                const uint64_t sent = net.bytesSent();
                const double inst =
                    static_cast<double>(sent - lastRateBytes) / dt;
                rateBps = rateBps == 0.0 ? inst
                                         : rateBps * 0.6 + inst * 0.4;
                lastRateBytes = sent;
                lastRateT = now;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);

        // -- connection row
        ImGui::TextUnformatted("Server");
        ImGui::SameLine(90);
        ImGui::SetNextItemWidth(160);
        ImGui::BeginDisabled(busy);
        ImGui::InputText("##host", hostBuf.data(), hostBuf.size());
        ImGui::SameLine();
        ImGui::TextUnformatted("Port");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##port", &port, 0);
        ImGui::EndDisabled();

        // -- file row (drag & drop onto the window also works)
        ImGui::TextUnformatted("Log file");
        ImGui::SameLine(90);
        ImGui::SetNextItemWidth(-110);
        ImGui::InputText("##file", pathBuf.data(), pathBuf.size(),
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Browse...", ImVec2(100, 0))) {
            std::wstring p = pickOpenFile(hwnd);
            if (!p.empty()) selectFile(p);
        }
        ImGui::EndDisabled();
        if (!fileSizeStr.empty())
            ImGui::TextDisabled("Size: %s", fileSizeStr.c_str());
        else
            ImGui::TextDisabled("Tip: drag & drop a log file anywhere on "
                                "this window");

        ImGui::Spacing();

        ImGui::BeginDisabled(busy || logPath.empty());
        if (ImGui::Button("Upload", ImVec2(120, 0)))
            net.start(hostBuf.data(), port, logPath);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!busy);
        if (ImGui::Button("Cancel", ImVec2(120, 0))) net.cancel();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(st != XferState::Done);
        if (ImGui::Button("Save result.csv...", ImVec2(160, 0))) {
            std::wstring p = pickSaveFile(hwnd);
            if (!p.empty()) {
                std::ofstream out(p, std::ios::binary | std::ios::trunc);
                out << net.resultCsv();
                uiLog((out ? "saved " : "save FAILED: ") + narrow(p));
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-save next to log", &autoSave);

        // -- progress
        ImGui::Spacing();
        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "%.1f%%  (%llu / %llu MiB)",
                      static_cast<double>(net.progress()) * 100.0,
                      static_cast<unsigned long long>(net.bytesSent() >> 20),
                      static_cast<unsigned long long>(net.bytesTotal() >> 20));
        ImGui::ProgressBar(net.progress(), ImVec2(-1, 0),
                           net.bytesTotal() ? overlay : "");

        // -- speed / elapsed / ETA line
        {
            const double elapsed =
                busy ? std::chrono::duration<double>(Clock::now() - xferStart)
                           .count()
                     : finalElapsedSec;
            std::string line;
            char buf[64];
            if (st == XferState::Uploading && rateBps > 1.0) {
                std::snprintf(buf, sizeof(buf), "   Speed: %.1f MB/s",
                              rateBps / 1e6);
                line += buf;
                const uint64_t remain = net.bytesTotal() - net.bytesSent();
                if (remain > 0) {
                    std::snprintf(buf, sizeof(buf), "   ETA: %.1f s",
                                  static_cast<double>(remain) / rateBps);
                    line += buf;
                }
            }
            if (elapsed > 0.0) {
                std::snprintf(buf, sizeof(buf), "   Elapsed: %.1f s",
                              elapsed);
                line += buf;
            }
            const char* stateName =
                st == XferState::Idle            ? "idle"
                : st == XferState::Connecting    ? "connecting"
                : st == XferState::Uploading     ? "uploading"
                : st == XferState::WaitingResult ? "analyzing on server"
                : st == XferState::Done          ? "done"
                : st == XferState::Cancelled     ? "cancelled"
                                                 : "failed";
            ImGui::Text("State: %s%s", stateName, line.c_str());
        }
        ImGui::TextWrapped("Status: %s", net.status().c_str());

        // -- analysis result panel (after a successful run)
        if (st == XferState::Done && summary.ok) {
            ImGui::Spacing();
            ImGui::SeparatorText("Analysis result");
            ImGui::Text("Average speed: %s", summary.avgSpeed.c_str());
            ImGui::Text("Parsed lines: %s    Skipped lines: %s",
                        summary.parsedLines.c_str(),
                        summary.skippedLines.c_str());
            if (!summary.reasons.empty()) {
                std::string breakdown;
                for (const auto& [name, count] : summary.reasons) {
                    if (!breakdown.empty()) breakdown += ",  ";
                    breakdown += name + ": " + count;
                }
                ImGui::TextWrapped("Skip breakdown: %s", breakdown.c_str());
            }
        }

        // -- log pane (newest entry on top)
        ImGui::Spacing();
        ImGui::SeparatorText("Activity log");

        for (auto& line : net.drainLog()) logLines.push_back(std::move(line));
        if (logLines.size() > 1000)
            logLines.erase(logLines.begin(), logLines.end() - 1000);
        ImGui::BeginDisabled(logLines.empty());
        if (ImGui::Button("Clear log")) logLines.clear();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu entries, newest first", logLines.size());
        ImGui::BeginChild("logpane", ImVec2(0, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (auto it = logLines.rbegin(); it != logLines.rend(); ++it)
            ImGui::TextUnformatted(it->c_str());
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        const float clear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
        ID3D11RenderTargetView* rtv = g_rtv.Get();
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        g_context->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_rtv.Reset();
    g_swapChain.Reset();
    g_context.Reset();
    g_device.Reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
