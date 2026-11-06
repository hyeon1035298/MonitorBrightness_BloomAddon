// ==WindhawkMod==
// @id              brightness
// @name            Monitor Brightness
// @description     Monitor brightness next to the taskbar clock: per-monitor sliders, hotkeys, dimming when idle
// @version         0.1.0
// @author          Hyeon
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -ldwmapi -ldxva2 -lgdi32 -lole32 -loleaut32 -lpowrprof -lruntimeobject -lshell32 -lshlwapi -luuid -lwbemuuid
// ==/WindhawkMod==

// SPDX-License-Identifier: GPL-3.0-only
// Source code is published under The GNU General Public License v3.0.
// Third-party sources and licenses: THIRD_PARTY.md in the repository root.

// ==WindhawkModReadme==
/*
# Monitor Brightness

A monitor next to the taskbar clock shows the brightness. The wheel over it changes
every monitor at once; clicking it opens a window with a slider per monitor, contrast
and power for monitors that speak DDC/CI, and the settings: the range each monitor's
0 to 100 maps to, hotkeys, and dimming while the computer is idle.

External monitors are driven over DDC/CI, a built-in panel through WMI. All hardware
calls run on one worker thread, and only the last value per monitor is written, so a
slow monitor never holds up the taskbar.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- pill:
  - theme: system
    $name: 테마
    $description: 위젯과 창의 라이트·다크. Bloom 애드온으로 쓰면 Bloom의 테마를 따릅니다
    $options:
    - system: Windows 작업표시줄 테마
    - dark: 다크
    - light: 라이트
  - glass: true
    $name: 유리 효과
    $description: 끄면 창이 블러 없이 테마 색으로 칠해집니다
  $name: 모양
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <dwmapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <powerbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wbemidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/base.h>

#include <windhawk_utils.h>

namespace wf = winrt::Windows::Foundation;
namespace wdj = winrt::Windows::Data::Json;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxs = winrt::Windows::UI::Xaml::Shapes;
using winrt::box_value;

namespace {

// ============================================================================
// Basics: unloading, language, paths, files
// ============================================================================

std::atomic<bool> g_unloading;
std::atomic<int> g_pending;  // queued taskbar work still to run

bool g_korean = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
PCWSTR T(PCWSTR ko, PCWSTR en) {
    return g_korean ? ko : en;
}

std::wstring Format(PCWSTR format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

// The add-on's data folder, which Bloom also writes values.json into.
std::wstring DataDir() {
    return KnownFolder(FOLDERID_LocalAppData) + L"\\Bloom\\addons\\brightness";
}

HINSTANCE ModuleInstance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ModuleInstance, &module);
    return module;
}

std::optional<std::wstring> ReadText(const std::wstring& path, DWORD maxBytes = 256 * 1024) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::string bytes;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart <= maxBytes;
    if (ok) {
        bytes.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size());
    }
    CloseHandle(file);
    if (!ok) return std::nullopt;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), n);
    return text;
}

// Whole or not at all: a temporary file, then a rename over the old one.
bool WriteText(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), n, nullptr, nullptr);
    std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::optional<wdj::JsonObject> ParseObject(const std::optional<std::wstring>& text) {
    wdj::JsonObject object{nullptr};
    if (!text || !wdj::JsonObject::TryParse(*text, object)) return std::nullopt;
    return object;
}

// ============================================================================
// Preferences (prefs.json in the data folder, written by this mod only)
// ============================================================================

struct HotKey {
    UINT mods = 0;  // MOD_CONTROL, MOD_ALT, MOD_SHIFT, MOD_WIN
    UINT vk = 0;    // 0: none
    bool operator==(const HotKey&) const = default;
};

// Each monitor's range: the hardware percents the window's 0 and 100 stand for.
struct MonPref {
    int lo = 0, hi = 100;
    bool hidden = false;
    bool Plain() const { return lo == 0 && hi == 100 && !hidden; }
};

struct Prefs {
    int pillNumber = 0;  // 0: the first monitor, 1: the average
    int wheelStep = 5;
    bool link = false;
    HotKey up{MOD_CONTROL | MOD_ALT, VK_UP}, down{MOD_CONTROL | MOD_ALT, VK_DOWN};
    int keyStep = 10;
    bool idleOn = false;
    int idleMinutes = 5;
    int idleLevel = 20;
    std::map<std::wstring, MonPref> monitors;  // by monitor device path
};
Prefs g_prefs;  // taskbar thread

constexpr size_t kMaxMonitorPrefs = 32;

HotKey ReadKey(const wdj::JsonObject& obj, PCWSTR name, HotKey fallback) {
    auto value = obj.TryLookup(name);
    auto key = value ? value.try_as<wdj::JsonObject>() : nullptr;
    if (!key) return fallback;
    HotKey k;
    k.mods = UINT(key.GetNamedNumber(L"mods", 0)) & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    k.vk = UINT(key.GetNamedNumber(L"vk", 0));
    if (k.vk > 0xFE) k.vk = 0;
    return k;
}

wdj::JsonObject KeyJson(const HotKey& k) {
    wdj::JsonObject o;
    o.SetNamedValue(L"mods", wdj::JsonValue::CreateNumberValue(k.mods));
    o.SetNamedValue(L"vk", wdj::JsonValue::CreateNumberValue(k.vk));
    return o;
}

std::wstring PrefsPath() {
    return DataDir() + L"\\prefs.json";
}

void LoadPrefs() {
    Prefs p;
    if (auto o = ParseObject(ReadText(PrefsPath()))) {
        auto& obj = *o;
        p.pillNumber = std::clamp(int(obj.GetNamedNumber(L"pillNumber", 0)), 0, 1);
        p.wheelStep = std::clamp(int(obj.GetNamedNumber(L"wheelStep", 5)), 1, 25);
        p.link = obj.GetNamedBoolean(L"link", false);
        p.up = ReadKey(obj, L"keyUp", p.up);
        p.down = ReadKey(obj, L"keyDown", p.down);
        p.keyStep = std::clamp(int(obj.GetNamedNumber(L"keyStep", 10)), 1, 50);
        p.idleOn = obj.GetNamedBoolean(L"idleOn", false);
        p.idleMinutes = std::clamp(int(obj.GetNamedNumber(L"idleMinutes", 5)), 1, 120);
        p.idleLevel = std::clamp(int(obj.GetNamedNumber(L"idleLevel", 20)), 0, 100);
        auto value = obj.TryLookup(L"monitors");
        if (auto mons = value ? value.try_as<wdj::JsonObject>() : nullptr) {
            for (auto const& entry : mons) {
                auto m = entry.Value().try_as<wdj::JsonObject>();
                if (!m || p.monitors.size() >= kMaxMonitorPrefs) continue;
                MonPref mp;
                mp.lo = std::clamp(int(m.GetNamedNumber(L"lo", 0)), 0, 99);
                mp.hi = std::clamp(int(m.GetNamedNumber(L"hi", 100)), mp.lo + 1, 100);
                mp.hidden = m.GetNamedBoolean(L"hidden", false);
                p.monitors[std::wstring(entry.Key())] = mp;
            }
        }
    }
    g_prefs = p;
}

void SavePrefs() {
    auto& p = g_prefs;
    wdj::JsonObject obj;
    obj.SetNamedValue(L"v", wdj::JsonValue::CreateNumberValue(1));
    obj.SetNamedValue(L"pillNumber", wdj::JsonValue::CreateNumberValue(p.pillNumber));
    obj.SetNamedValue(L"wheelStep", wdj::JsonValue::CreateNumberValue(p.wheelStep));
    obj.SetNamedValue(L"link", wdj::JsonValue::CreateBooleanValue(p.link));
    obj.SetNamedValue(L"keyUp", KeyJson(p.up));
    obj.SetNamedValue(L"keyDown", KeyJson(p.down));
    obj.SetNamedValue(L"keyStep", wdj::JsonValue::CreateNumberValue(p.keyStep));
    obj.SetNamedValue(L"idleOn", wdj::JsonValue::CreateBooleanValue(p.idleOn));
    obj.SetNamedValue(L"idleMinutes", wdj::JsonValue::CreateNumberValue(p.idleMinutes));
    obj.SetNamedValue(L"idleLevel", wdj::JsonValue::CreateNumberValue(p.idleLevel));
    wdj::JsonObject mons;
    for (auto& [id, mp] : p.monitors) {
        if (mp.Plain()) continue;
        wdj::JsonObject m;
        m.SetNamedValue(L"lo", wdj::JsonValue::CreateNumberValue(mp.lo));
        m.SetNamedValue(L"hi", wdj::JsonValue::CreateNumberValue(mp.hi));
        m.SetNamedValue(L"hidden", wdj::JsonValue::CreateBooleanValue(mp.hidden));
        mons.SetNamedValue(id, m);
    }
    obj.SetNamedValue(L"monitors", mons);
    SHCreateDirectoryExW(nullptr, DataDir().c_str(), nullptr);
    if (!WriteText(PrefsPath(), std::wstring(obj.Stringify()))) Wh_Log(L"prefs not written: %u", GetLastError());
}

MonPref PrefOf(const std::wstring& id) {
    auto it = g_prefs.monitors.find(id);
    return it == g_prefs.monitors.end() ? MonPref{} : it->second;
}

MonPref& EditPref(const std::wstring& id) {
    return g_prefs.monitors[id];
}

// ============================================================================
// Look: light or dark and glass, as Bloom hands them to its add-ons
// ============================================================================

// Bloom's design tokens: zinc greys, one ink whose alpha makes every overlay,
// and Bloom's rose as the accent (dark #FF7A9E, light #E04E78).
struct Palette {
    uint32_t win, pop, btn, btnHover, fg, muted, faint, ink, accent;
    double tintOpacity, tintLuminosity;
};
constexpr Palette kDark{0xFF18181B, 0xFF27272A, 0xFF3F3F46, 0xFF52525B, 0xFFECEDEE, 0xFFA1A1AA,
                        0x99ECEDEE, 0x00FFFFFF, 0xFFFF7A9E, 0.75, 0.85};
constexpr Palette kLight{0xFFF4F4F5, 0xFFFAFAFA, 0xFFFFFFFF, 0xFFF4F4F5, 0xFF11181C, 0xFF3F3F46,
                         0xFF52525B, 0x00111111, 0xFFE04E78, 0.75, 0.9};

struct Look {
    bool known = false;  // from Bloom; else the taskbar's own theme
    bool dark = true;
    bool glass = true;
};
Look g_look;

// The add-on's own settings (pill.theme, pill.glass), which Bloom sets to its theme.
// With "system" the widget follows the taskbar it sits on.
Look ReadLook() {
    Look look;
    PCWSTR theme = Wh_GetStringSetting(L"pill.theme");
    if (theme && (!wcscmp(theme, L"dark") || !wcscmp(theme, L"light"))) {
        look.known = true;
        look.dark = !wcscmp(theme, L"dark");
    }
    Wh_FreeStringSetting(theme);
    look.glass = Wh_GetIntSetting(L"pill.glass") != 0;
    return look;
}

const Palette& PaletteFor(const wux::FrameworkElement& onTaskbar) {
    bool dark = g_look.known ? g_look.dark : !onTaskbar || onTaskbar.ActualTheme() != wux::ElementTheme::Light;
    return dark ? kDark : kLight;
}

wuxm::SolidColorBrush Brush(uint32_t argb) {
    return wuxm::SolidColorBrush(winrt::Windows::UI::Color{BYTE(argb >> 24), BYTE(argb >> 16), BYTE(argb >> 8), BYTE(argb)});
}

uint32_t WithAlpha(uint32_t argb, BYTE alpha) {
    return (argb & 0x00FFFFFF) | (uint32_t(alpha) << 24);
}

// The ink at a percent of alpha: the w5, w8, w10, w15 and w25 overlays.
uint32_t Ink(const Palette& c, int percent) {
    return WithAlpha(c.ink, BYTE(percent * 255 / 100));
}

wuxm::Brush GlassBrush(const Palette& c) {
    if (!g_look.glass) return Brush(c.pop);
    auto color = Brush(c.pop).Color();
    wuxm::AcrylicBrush glass;
    glass.BackgroundSource(wuxm::AcrylicBackgroundSource::HostBackdrop);
    glass.TintColor(color);
    glass.TintOpacity(c.tintOpacity);
    glass.TintLuminosityOpacity(c.tintLuminosity);
    glass.FallbackColor(color);
    return glass;
}

// ============================================================================
// Taskbar windows and their XAML (CTaskBand::GetTaskbarHost)
// ============================================================================

void* CTaskBand_ITaskListWndSite_vftable;
void* CSecondaryTaskBand_ITaskListWndSite_vftable;
using GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;
GetTaskbarHost_t CSecondaryTaskBand_GetTaskbarHost_Original;
void* TaskbarHost_FrameHeight_Original;
using Decref_t = void(WINAPI*)(void* pThis);
Decref_t std__Ref_count_base__Decref_Original;

bool HookTaskbarDll() {
    HMODULE module = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"}, &CTaskBand_ITaskListWndSite_vftable},
        {{LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})"}, &CSecondaryTaskBand_ITaskListWndSite_vftable},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
         &CTaskBand_GetTaskbarHost_Original},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )"},
         &CSecondaryTaskBand_GetTaskbarHost_Original},
        {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"}, &TaskbarHost_FrameHeight_Original},
        {{LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"}, &std__Ref_count_base__Decref_Original},
    };
    return WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks));
}

bool IsSecondary(HWND hwnd) {
    wchar_t cls[32];
    return GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

std::vector<HWND> Taskbars() {
    std::vector<HWND> list;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            wchar_t cls[32];
            DWORD pid = 0;
            if (GetWindowThreadProcessId(hwnd, &pid) && pid == GetCurrentProcessId() &&
                GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) &&
                (_wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0))
                ((std::vector<HWND>*)param)->push_back(hwnd);
            return TRUE;
        },
        (LPARAM)&list);
    std::ranges::stable_partition(list, [](HWND h) { return !IsSecondary(h); });
    return list;
}

HWND PrimaryTaskbar() {
    auto list = Taskbars();
    return list.empty() || IsSecondary(list[0]) ? nullptr : list[0];
}

// Taskbar thread.
wux::FrameworkElement TaskbarElement(HWND hwnd) {
    bool secondary = IsSecondary(hwnd);
    void* vftable = secondary ? CSecondaryTaskBand_ITaskListWndSite_vftable : CTaskBand_ITaskListWndSite_vftable;
    auto getHost = secondary ? CSecondaryTaskBand_GetTaskbarHost_Original : CTaskBand_GetTaskbarHost_Original;
    if (!vftable || !getHost || !TaskbarHost_FrameHeight_Original || !std__Ref_count_base__Decref_Original)
        return nullptr;
    HWND band = secondary ? FindWindowExW(hwnd, nullptr, L"WorkerW", nullptr) : (HWND)GetPropW(hwnd, L"TaskbandHWND");
    void* site = band ? (void*)GetWindowLongPtrW(band, 0) : nullptr;
    for (int i = 0; site && *(void**)site != vftable; i++) {
        if (i == 20) return nullptr;
        site = (void**)site + 1;
    }
    if (!site) return nullptr;
    void* host[2]{};  // std::shared_ptr<TaskbarHost>
    getHost(site, host);
    if (!host[0]) {
        if (host[1]) std__Ref_count_base__Decref_Original(host[1]);
        return nullptr;
    }
    // 48:83EC 28 | sub rsp,28 ; 48:83C1 48 | add rcx,48 <- the element's offset in TaskbarHost
    size_t offset = 0x10;
    const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
    if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 && b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F)
        offset = b[7];
    wux::FrameworkElement element{nullptr};
    winrt::copy_from_abi(element, *(void**)((BYTE*)host[0] + offset));
    std__Ref_count_base__Decref_Original(host[1]);
    return element;
}

wux::FrameworkElement Parent(const wux::FrameworkElement& e) {
    return wuxm::VisualTreeHelper::GetParent(e).try_as<wux::FrameworkElement>();
}

template <class F>
wux::FrameworkElement FindDescendant(const wux::FrameworkElement& e, F&& visit, int depth) {
    if (!e || depth <= 0) return nullptr;
    int n = wuxm::VisualTreeHelper::GetChildrenCount(e);
    for (int i = 0; i < n; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(e, i).try_as<wux::FrameworkElement>();
        if (!child) continue;
        if (visit(child)) return child;
        if (auto found = FindDescendant(child, visit, depth - 1)) return found;
    }
    return nullptr;
}

// The tray's horizontal panel: icons, then the clock (NotificationCenterButton).
wuxc::StackPanel TrayPanel(HWND hwnd) {
    auto element = TaskbarElement(hwnd);
    auto root = element ? element.XamlRoot() : nullptr;
    auto content = root ? root.Content().try_as<wux::FrameworkElement>() : nullptr;
    auto frame = FindDescendant(
        content, [](auto& e) { return winrt::get_class_name(e) == L"SystemTray.SystemTrayFrame"; }, 8);
    auto grid = FindDescendant(frame, [](auto& e) { return e.Name() == L"SystemTrayFrameGrid"; }, 2);
    return grid ? grid.try_as<wuxc::StackPanel>() : nullptr;
}

// Other Bloom add-ons find this widget by name; the memory-clean one sits right of it.
constexpr PCWSTR kWidgetName = L"BloomAddon.brightness";
constexpr PCWSTR kMemoryWidgetName = L"BloomAddon.memory-clean";

// Right before the memory-clean widget, else right before the clock.
uint32_t InsertIndex(const wuxc::StackPanel& tray) {
    auto children = tray.Children();
    uint32_t clock = children.Size();
    for (uint32_t i = 0; i < children.Size(); i++) {
        auto child = children.GetAt(i).try_as<wux::FrameworkElement>();
        if (!child) continue;
        if (child.Name() == kMemoryWidgetName) return i;
        if (clock == children.Size() &&
            (child.Name() == L"NotificationCenterButton" ||
             FindDescendant(child, [](auto& e) { return e.Name() == L"NotificationCenterButton"; }, 3)))
            clock = i;
    }
    return clock;
}

bool TaskbarOnTop(HWND taskbar) {
    RECT rc{};
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(taskbar, &rc) || !GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    return (rc.top + rc.bottom) / 2 < (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
}

// ---- running work on the taskbar thread ---------------------------------------

UINT RunMsg() {
    static UINT msg = RegisterWindowMessageW(L"MonitorBrightness_Run");
    return msg;
}

LRESULT CALLBACK TaskbarSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR) {
    if (msg == RunMsg() && lParam) {
        try {
            (*(std::function<void()>*)lParam)();
        } catch (...) {
            Wh_Log(L"taskbar work failed: %08X", winrt::to_hresult());
        }
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

HWND g_subclassed;

// Runs `work` on the window's thread and waits for it.
bool RunOn(HWND hwnd, std::function<void()> work) {
    if (!hwnd) return false;
    if (g_subclassed != hwnd) {
        if (g_subclassed) WindhawkUtils::RemoveWindowSubclassFromAnyThread(g_subclassed, TaskbarSubclass);
        g_subclassed = nullptr;
        if (!WindhawkUtils::SetWindowSubclassFromAnyThread(hwnd, TaskbarSubclass, 0)) return false;
        g_subclassed = hwnd;
    }
    SendMessageW(hwnd, RunMsg(), 0, (LPARAM)&work);
    return true;
}

winrt::Windows::System::DispatcherQueue g_queue{nullptr};  // the taskbar thread's, set there

// Runs `work` on the taskbar thread later, from any thread.
bool Post(std::function<void()> work) {
    auto queue = g_queue;
    if (g_unloading || !queue) return false;
    g_pending++;
    bool queued = false;
    try {
        queued = queue.TryEnqueue([work = std::move(work)] {
            if (!g_unloading) {
                try {
                    work();
                } catch (...) {
                    Wh_Log(L"posted work failed: %08X", winrt::to_hresult());
                }
            }
            g_pending--;
        });
    } catch (...) {
    }
    if (!queued) g_pending--;
    return queued;
}

// ============================================================================
// Hardware: one worker thread owns every monitor handle, DDC/CI call and WMI call
// ============================================================================

// What the worker hands the taskbar thread after a scan.
struct MonView {
    std::wstring id, name;
    bool internal = false, ok = false, contrastOk = false;
    int hw = -1, contrast = -1;  // hardware percents
};

enum class What { kBrightness, kContrast, kPower };

constexpr UINT kMsgScan = WM_APP + 1, kMsgWrite = WM_APP + 2, kMsgScreenOff = WM_APP + 3;
constexpr UINT_PTR kTimerRescan = 1;
constexpr PCWSTR kWorkerClass = L"Bloom.Addon.brightness.Worker";

std::atomic<HWND> g_workerWnd;
HANDLE g_workerThread;

// Writes waiting for the worker: only the newest value per monitor and kind survives.
std::mutex g_writeLock;
std::map<std::pair<std::wstring, int>, int> g_writes;
std::atomic<bool> g_writePosted;

void QueueWrite(const std::wstring& id, What what, int value) {
    {
        std::lock_guard lock(g_writeLock);
        g_writes[{id, int(what)}] = value;
    }
    if (g_writePosted.exchange(true)) return;
    HWND w = g_workerWnd;
    if (!w || !PostMessageW(w, kMsgWrite, 0, 0)) g_writePosted = false;
}

void RequestScan() {
    if (HWND w = g_workerWnd) PostMessageW(w, kMsgScan, 0, 0);
}

void OnMonitors(std::vector<MonView> list, uint64_t readAt);  // taskbar thread

struct HwMon {
    std::wstring id, name;
    bool internal = false, primary = false;
    RECT rc{};
    std::vector<PHYSICAL_MONITOR> phys;
    bool ok = false, contrastOk = false;
    DWORD max = 100, cmax = 100;
    int pct = -1, contrast = -1;
};
std::vector<HwMon> w_mons;  // worker thread only

// ---- WMI, only for a built-in panel ---------------------------------------------

winrt::com_ptr<IWbemServices> w_wmi;

bool WmiReady() {
    if (w_wmi) return true;
    winrt::com_ptr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, locator.put_void())))
        return false;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, w_wmi.put());
    SysFreeString(ns);
    if (FAILED(hr)) {
        w_wmi = nullptr;
        return false;
    }
    CoSetProxyBlanket(w_wmi.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

winrt::com_ptr<IWbemClassObject> WmiFirst(PCWSTR query) {
    winrt::com_ptr<IEnumWbemClassObject> found;
    BSTR language = SysAllocString(L"WQL"), text = SysAllocString(query);
    HRESULT hr = w_wmi->ExecQuery(language, text, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
                                  found.put());
    SysFreeString(language);
    SysFreeString(text);
    if (FAILED(hr) || !found) return nullptr;
    winrt::com_ptr<IWbemClassObject> object;
    ULONG count = 0;
    if (found->Next(3000, 1, object.put(), &count) != WBEM_S_NO_ERROR || !count) return nullptr;
    return object;
}

int WmiGet() {
    if (!WmiReady()) return -1;
    auto object = WmiFirst(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");
    if (!object) return -1;
    VARIANT value, number;
    VariantInit(&value);
    VariantInit(&number);
    int out = -1;
    if (SUCCEEDED(object->Get(L"CurrentBrightness", 0, &value, nullptr, nullptr)) &&
        SUCCEEDED(VariantChangeType(&number, &value, 0, VT_I4)))
        out = std::clamp(int(number.lVal), 0, 100);
    VariantClear(&value);
    VariantClear(&number);
    return out;
}

bool WmiSet(int percent) {
    if (!WmiReady()) return false;
    auto object = WmiFirst(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");
    if (!object) return false;
    VARIANT path;
    VariantInit(&path);
    if (FAILED(object->Get(L"__PATH", 0, &path, nullptr, nullptr)) || path.vt != VT_BSTR) {
        VariantClear(&path);
        return false;
    }
    winrt::com_ptr<IWbemClassObject> cls, signature, in;
    BSTR className = SysAllocString(L"WmiMonitorBrightnessMethods");
    BSTR method = SysAllocString(L"WmiSetBrightness");
    bool ok = SUCCEEDED(w_wmi->GetObject(className, 0, nullptr, cls.put(), nullptr)) &&
              SUCCEEDED(cls->GetMethod(L"WmiSetBrightness", 0, signature.put(), nullptr)) &&
              SUCCEEDED(signature->SpawnInstance(0, in.put()));
    if (ok) {
        VARIANT timeout, level;
        VariantInit(&timeout);
        VariantInit(&level);
        timeout.vt = VT_I4;
        timeout.lVal = 0;
        level.vt = VT_UI1;
        level.bVal = BYTE(std::clamp(percent, 0, 100));
        ok = SUCCEEDED(in->Put(L"Timeout", 0, &timeout, 0)) && SUCCEEDED(in->Put(L"Brightness", 0, &level, 0)) &&
             SUCCEEDED(w_wmi->ExecMethod(path.bstrVal, method, 0, nullptr, in.get(), nullptr, nullptr));
    }
    SysFreeString(className);
    SysFreeString(method);
    VariantClear(&path);
    return ok;
}

// ---- DDC/CI ---------------------------------------------------------------------

constexpr BYTE kVcpBrightness = 0x10, kVcpContrast = 0x12, kVcpPower = 0xD6;

int Percent(DWORD value, DWORD max) {
    return max ? std::clamp(int((value * 100 + max / 2) / max), 0, 100) : 0;
}

bool ReadVcp(HANDLE monitor, BYTE code, DWORD& value, DWORD& max) {
    for (int i = 0; i < 2; i++) {
        if (GetVCPFeatureAndVCPFeatureReply(monitor, code, nullptr, &value, &max) && max) return true;
        Sleep(40);
    }
    return false;
}

void ReadDdc(HwMon& m) {
    m.ok = m.contrastOk = false;
    if (m.phys.empty()) return;
    HANDLE h = m.phys[0].hPhysicalMonitor;
    DWORD value = 0, max = 0;
    if (ReadVcp(h, kVcpBrightness, value, max)) {
        m.ok = true;
        m.max = max;
        m.pct = Percent(value, max);
    }
    if (m.ok && ReadVcp(h, kVcpContrast, value, max)) {
        m.contrastOk = true;
        m.cmax = max;
        m.contrast = Percent(value, max);
    }
}

bool SetVcp(HwMon& m, BYTE code, DWORD value) {
    bool ok = false;
    for (auto& p : m.phys) ok |= SetVCPFeature(p.hPhysicalMonitor, code, value) != FALSE;
    if (!ok) {
        Sleep(50);
        for (auto& p : m.phys) ok |= SetVCPFeature(p.hPhysicalMonitor, code, value) != FALSE;
    }
    return ok;
}

void CloseMonitors() {
    for (auto& m : w_mons)
        if (!m.phys.empty()) DestroyPhysicalMonitors(DWORD(m.phys.size()), m.phys.data());
    w_mons.clear();
}

// ---- the monitor list -----------------------------------------------------------

struct Target {
    std::wstring gdi, path, name;
    bool internal = false;
};

// Friendly names, stable ids (the monitor's device path) and which panel is built in.
std::vector<Target> DisplayTargets() {
    std::vector<Target> out;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return out;
    paths.resize(pathCount);
    for (auto& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = p.sourceInfo.adapterId;
        source.header.id = p.sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = p.targetInfo.adapterId;
        target.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
            continue;
        auto tech = p.targetInfo.outputTechnology;
        out.push_back({source.viewGdiDeviceName, target.monitorDevicePath, target.monitorFriendlyDeviceName,
                       tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                           tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                           tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED});
    }
    return out;
}

void Scan() {
    CloseMonitors();
    struct Found {
        HMONITOR handle;
        std::wstring gdi;
        RECT rc;
        bool primary;
    };
    std::vector<Found> found;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR handle, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(handle, &mi))
                ((std::vector<Found>*)param)
                    ->push_back({handle, mi.szDevice, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
            return TRUE;
        },
        (LPARAM)&found);
    // The primary monitor first, then left to right.
    std::ranges::sort(found, [](const Found& a, const Found& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rc.left != b.rc.left) return a.rc.left < b.rc.left;
        return a.rc.top < b.rc.top;
    });
    auto targets = DisplayTargets();
    int number = 0;
    for (auto& f : found) {
        if (g_unloading) break;
        HwMon m;
        m.rc = f.rc;
        m.primary = f.primary;
        for (auto& t : targets) {
            if (_wcsicmp(t.gdi.c_str(), f.gdi.c_str()) == 0) {
                m.id = t.path;
                m.name = t.name;
                m.internal = t.internal;
                break;
            }
        }
        number++;
        if (m.id.empty()) m.id = f.gdi;
        if (m.internal) {
            m.name = T(L"내장 화면", L"Built-in display");
            m.pct = WmiGet();
            m.ok = m.pct >= 0;
        } else {
            DWORD count = 0;
            if (GetNumberOfPhysicalMonitorsFromHMONITOR(f.handle, &count) && count && count <= 8) {
                m.phys.resize(count);
                if (!GetPhysicalMonitorsFromHMONITOR(f.handle, count, m.phys.data())) m.phys.clear();
            }
            if (m.name.empty() && !m.phys.empty()) m.name = m.phys[0].szPhysicalMonitorDescription;
            ReadDdc(m);
        }
        if (m.name.empty()) m.name = Format(T(L"모니터 %d", L"Monitor %d"), number);
        w_mons.push_back(std::move(m));
    }
}

void Publish(uint64_t readAt) {
    std::vector<MonView> list;
    for (auto& m : w_mons) list.push_back({m.id, m.name, m.internal, m.ok, m.contrastOk, m.pct, m.contrast});
    Post([list = std::move(list), readAt]() mutable { OnMonitors(std::move(list), readAt); });
}

void ScanAndPublish() {
    uint64_t readAt = GetTickCount64();
    Scan();
    Publish(readAt);
}

void DoWrites() {
    g_writePosted = false;
    std::map<std::pair<std::wstring, int>, int> writes;
    {
        std::lock_guard lock(g_writeLock);
        writes.swap(g_writes);
    }
    for (auto& [key, value] : writes) {
        if (g_unloading) return;
        auto it = std::ranges::find_if(w_mons, [&](auto& m) { return m.id == key.first; });
        if (it == w_mons.end() || !it->ok) continue;
        HwMon& m = *it;
        switch (What(key.second)) {
            case What::kBrightness:
                if (m.internal ? WmiSet(value) : SetVcp(m, kVcpBrightness, DWORD((value * m.max + 50) / 100)))
                    m.pct = value;
                else
                    Wh_Log(L"brightness not set on %s", m.name.c_str());
                break;
            case What::kContrast:
                if (m.contrastOk && SetVcp(m, kVcpContrast, DWORD((value * m.cmax + 50) / 100))) m.contrast = value;
                break;
            case What::kPower:
                // 5: off until the monitor's own power button; monitors without it take 4 (standby).
                if (!m.internal && !SetVcp(m, kVcpPower, 5)) SetVcp(m, kVcpPower, 4);
                break;
        }
    }
}

LRESULT CALLBACK WorkerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case kMsgScan:
            ScanAndPublish();
            return 0;
        case kMsgWrite:
            DoWrites();
            return 0;
        case kMsgScreenOff:
            Sleep(500);  // the click that asked for it must not wake the screens again
            DefWindowProcW(hwnd, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
            return 0;
        case WM_DISPLAYCHANGE:
            SetTimer(hwnd, kTimerRescan, 2000, nullptr);  // several arrive while monitors come and go
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC) SetTimer(hwnd, kTimerRescan, 3000, nullptr);
            return TRUE;
        case WM_TIMER:
            if (wParam == kTimerRescan) {
                KillTimer(hwnd, kTimerRescan);
                ScanAndPublish();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_workerWnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI WorkerMain(void*) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HINSTANCE instance = ModuleInstance();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WorkerProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWorkerClass;
    RegisterClassExW(&wc);
    // A hidden top-level window: display changes are broadcast to those only.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWorkerClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                                nullptr, instance, nullptr);
    if (hwnd) {
        g_workerWnd = hwnd;
        if (!g_unloading) ScanAndPublish();
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    } else {
        Wh_Log(L"worker window not created: %u", GetLastError());
    }
    g_workerWnd = nullptr;
    CloseMonitors();
    w_wmi = nullptr;
    UnregisterClassW(kWorkerClass, instance);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

void StartWorker() {
    if (!g_workerThread) g_workerThread = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
}

void StopWorker() {
    if (!g_workerThread) return;
    for (int i = 0; i < 100 && !g_workerWnd && WaitForSingleObject(g_workerThread, 0) == WAIT_TIMEOUT; i++) Sleep(10);
    if (HWND w = g_workerWnd) PostMessageW(w, WM_CLOSE, 0, 0);
    // A DDC/CI call in flight finishes first; a monitor that never answers leaves the thread
    // here, and the runtime then keeps the mod loaded rather than freeing code in use.
    if (WaitForSingleObject(g_workerThread, 6000) == WAIT_TIMEOUT) Wh_Log(L"worker still busy");
    CloseHandle(g_workerThread);
    g_workerThread = nullptr;
}

// ============================================================================
// Monitors as the taskbar thread sees them
// ============================================================================

struct Mon {
    std::wstring id, name;
    bool internal = false, ok = false, contrastOk = false;
    int hw = -1, value = -1, contrast = -1;  // value: the window's 0..100 within the range
    uint64_t changedAt = 0;                  // GetTickCount64 of the last change made here
};
std::vector<Mon> g_mons;
bool g_scanned = false;

int ToHw(const Mon& m, int value) {
    MonPref p = PrefOf(m.id);
    return std::clamp(int(std::lround(p.lo + (p.hi - p.lo) * value / 100.0)), 0, 100);
}

int FromHw(const Mon& m, int hw) {
    if (hw < 0) return -1;
    MonPref p = PrefOf(m.id);
    return std::clamp(int(std::lround((hw - p.lo) * 100.0 / std::max(1, p.hi - p.lo))), 0, 100);
}

bool Shown(const Mon& m) {
    return m.ok && !PrefOf(m.id).hidden;
}

// The number on the taskbar: the first shown monitor's, or the average; -1 when none.
int PillNumber() {
    int sum = 0, count = 0;
    for (auto& m : g_mons) {
        if (!Shown(m) || m.value < 0) continue;
        if (g_prefs.pillNumber == 0) return m.value;
        sum += m.value;
        count++;
    }
    return count ? (sum + count / 2) / count : -1;
}

void SetValue(Mon& m, int value) {
    m.value = std::clamp(value, 0, 100);
    m.hw = ToHw(m, m.value);
    m.changedAt = GetTickCount64();
    QueueWrite(m.id, What::kBrightness, m.hw);
}

void PaintWidgets(bool force = false);
void RefreshMain();
void Undim();

// Wheel notches from a delta: a fine wheel or a touchpad sends parts of one.
int Notches(int& carry, int delta) {
    carry += delta;
    int notches = carry / WHEEL_DELTA;
    carry -= notches * WHEEL_DELTA;
    return notches;
}

// A slider moved: that monitor, and with "all together" every shown one, to the same value.
void ChangeOne(const std::wstring& id, int value) {
    Undim();
    for (auto& m : g_mons) {
        if (m.id == id || (g_prefs.link && Shown(m))) SetValue(m, value);
    }
    PaintWidgets();
    RefreshMain();
}

// The widget's wheel and the hotkeys: every shown monitor by the same step.
void StepAll(int delta) {
    Undim();
    for (auto& m : g_mons)
        if (Shown(m) && m.value >= 0) SetValue(m, m.value + delta);
    PaintWidgets();
    RefreshMain();
}

void ChangeContrast(const std::wstring& id, int value) {
    for (auto& m : g_mons) {
        if (m.id != id || !m.contrastOk) continue;
        m.contrast = std::clamp(value, 0, 100);
        m.changedAt = GetTickCount64();
        QueueWrite(m.id, What::kContrast, m.contrast);
    }
}

// ============================================================================
// The widget: a monitor with the brightness in it, left of the memory-clean widget
// ============================================================================

// 25:18, the same box as the memory-clean add-on's RAM module next to it.
constexpr double kW = 25, kH = 18;
constexpr double kBodyX = 1.15, kBodyY = 0.95, kBodyW = kW - 2.3, kBodyH = 12.6, kRadius = 3.2, kStroke = 1.3;
constexpr double kNeckW = 2.8, kBaseW = 10, kBaseH = 1.3, kBaseY = kH - 1.45;
constexpr PCWSTR kFont = L"Segoe UI, Malgun Gothic";  // Bloom's own pair

winrt::Windows::UI::Text::FontWeight Weight(int weight) {
    return {uint16_t(weight)};
}

// One drawing of the icon.
struct MonIcon {
    wuxc::Canvas canvas{nullptr};
    wuxs::Rectangle body{nullptr}, fill{nullptr}, neck{nullptr}, base{nullptr};
    wuxc::TextBlock text{nullptr};

    void Build() {
        canvas = wuxc::Canvas();
        canvas.Width(kW);
        canvas.Height(kH);
        fill = wuxs::Rectangle();
        body = wuxs::Rectangle();
        for (auto* r : {&fill, &body}) {
            r->Width(kBodyW);
            r->Height(kBodyH);
            r->RadiusX(kRadius);
            r->RadiusY(kRadius);
            wuxc::Canvas::SetLeft(*r, kBodyX);
            wuxc::Canvas::SetTop(*r, kBodyY);
        }
        body.StrokeThickness(kStroke);
        double neckTop = kBodyY + kBodyH - kStroke / 2;
        neck = wuxs::Rectangle();
        neck.Width(kNeckW);
        neck.Height(kBaseY - neckTop);
        wuxc::Canvas::SetLeft(neck, kW / 2 - kNeckW / 2);
        wuxc::Canvas::SetTop(neck, neckTop);
        base = wuxs::Rectangle();
        base.Width(kBaseW);
        base.Height(kBaseH);
        base.RadiusX(kBaseH / 2);
        base.RadiusY(kBaseH / 2);
        wuxc::Canvas::SetLeft(base, kW / 2 - kBaseW / 2);
        wuxc::Canvas::SetTop(base, kBaseY - kBaseH / 2);
        canvas.Children().Append(fill);
        canvas.Children().Append(neck);
        canvas.Children().Append(base);
        canvas.Children().Append(body);
        wuxc::Grid textBox;
        textBox.Width(kBodyW);
        textBox.Height(kBodyH);
        wuxc::Canvas::SetLeft(textBox, kBodyX);
        wuxc::Canvas::SetTop(textBox, kBodyY);
        text = wuxc::TextBlock();
        text.FontFamily(wuxm::FontFamily(kFont));
        text.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
        text.HorizontalAlignment(wux::HorizontalAlignment::Center);
        text.VerticalAlignment(wux::VerticalAlignment::Center);
        text.TextLineBounds(wux::TextLineBounds::Tight);
        textBox.Children().Append(text);
        canvas.Children().Append(textBox);
    }

    // The screen fills as bright as the monitor is.
    void Paint(int value, const Palette& c) {
        auto brush = Brush(c.fg);
        body.Stroke(brush);
        neck.Fill(brush);
        base.Fill(brush);
        fill.Fill(Brush(WithAlpha(c.fg, 0x40)));
        wuxm::RectangleGeometry clip;
        clip.Rect({0, 0, float(kBodyW * std::clamp(value, 0, 100) / 100.0), float(kBodyH)});
        fill.Clip(clip);
        text.Text(value < 0 ? std::wstring(L"–") : std::to_wstring(value));
        text.FontSize(value >= 100 ? 7.8 : 10);
        text.Foreground(brush);
    }
};

struct Widget {
    HWND taskbar = nullptr;
    wuxc::Grid root{nullptr};
    MonIcon icon;
    int shown = -2;
    const Palette* palette = nullptr;
    bool hover = false, fitted = false;
    int wheel = 0;  // part of a notch not yet used
};
std::vector<std::unique_ptr<Widget>> g_widgets;  // taskbar thread
void TogglePanel(Widget& w);

// ---- fitting the taskbar ----------------------------------------------------------
// Everything about how the widget and its window sit on the taskbar is here: the
// widget's size and hover, and where the window opens. The memory-clean add-on behaves
// the same way, so the two are kept alike.

// Height and corners of the tray's own icons, so the hover box lines up with theirs.
bool FitToTray(Widget& w, const wuxc::StackPanel& tray) {
    auto icon = FindDescendant(
        tray,
        [](auto& e) {
            auto cls = winrt::get_class_name(e);
            return (cls == L"SystemTray.ChevronIconView" || cls == L"SystemTray.NotifyIconView") &&
                   e.ActualHeight() > 0;
        },
        8);
    if (!icon) return false;
    auto grid = FindDescendant(icon, [](auto& e) { return e.Name() == L"ContainerGrid"; }, 2).try_as<wuxc::Grid>();
    double height = grid && grid.ActualHeight() > 0 ? grid.ActualHeight() : icon.ActualHeight();
    wux::CornerRadius radius = grid ? grid.CornerRadius() : wux::CornerRadius{};
    if (radius.TopLeft <= 0)
        if (auto control = icon.try_as<wuxc::Control>()) radius = control.CornerRadius();
    if (height < 16 || height > 64) return false;
    w.root.Height(height);
    if (radius.TopLeft > 0) w.root.CornerRadius(radius);
    return true;
}

// The tray icons' hover: Windows' subtle fill, by the taskbar's own theme.
uint32_t TrayHover(const wux::FrameworkElement& e) {
    return e.ActualTheme() == wux::ElementTheme::Light ? 0x09000000 : 0x0FFFFFFF;
}

// Opens the window over the widget, or under it when the taskbar is at the top.
void ShowFlyout(Widget& w, const wuxc::Flyout& flyout) {
    wuxc::Primitives::FlyoutShowOptions options;
    options.Placement(TaskbarOnTop(w.taskbar) ? wuxc::Primitives::FlyoutPlacementMode::Bottom
                                              : wuxc::Primitives::FlyoutPlacementMode::Top);
    flyout.ShowAt(w.root, options);
}

std::wstring Tip() {
    std::wstring tip;
    for (auto& m : g_mons) {
        if (!Shown(m) || m.value < 0) continue;
        if (!tip.empty()) tip += L"\n";
        tip += Format(L"%s  %d %%", m.name.c_str(), m.value);
    }
    if (tip.empty()) tip = g_scanned ? T(L"밝기를 바꿀 수 있는 모니터가 없습니다", L"No monitor whose brightness can be set")
                                     : T(L"모니터를 찾는 중…", L"Looking for monitors…");
    return std::wstring(T(L"밝기", L"Brightness")) + L"\n" + tip;
}

void PaintWidget(Widget& w, bool force) {
    const Palette& c = PaletteFor(w.root);
    int number = PillNumber();
    if (!force && w.shown == number && w.palette == &c) return;
    w.shown = number;
    w.palette = &c;
    w.icon.Paint(number, c);
    w.root.Background(Brush(w.hover ? TrayHover(w.root) : 0));
    std::wstring tip = Tip();
    wuxc::ToolTipService::SetToolTip(w.root, box_value(tip));
    wux::Automation::AutomationProperties::SetName(w.root, tip);
}

void PaintWidgets(bool force) {
    for (auto& w : g_widgets) PaintWidget(*w, force);
}

std::unique_ptr<Widget> BuildWidget(HWND taskbar) {
    auto w = std::make_unique<Widget>();
    w->taskbar = taskbar;
    w->root = wuxc::Grid();
    w->root.Name(kWidgetName);
    w->root.Height(32);
    w->root.Padding({6, 0, 6, 0});
    w->root.Margin({2, 0, 2, 0});
    w->root.CornerRadius({4, 4, 4, 4});
    w->root.VerticalAlignment(wux::VerticalAlignment::Center);
    w->root.Background(Brush(0));
    w->icon.Build();
    w->icon.canvas.VerticalAlignment(wux::VerticalAlignment::Center);
    w->root.Children().Append(w->icon.canvas);
    Widget* raw = w.get();
    w->root.PointerEntered([raw](auto&&, auto&&) {
        raw->hover = true;
        PaintWidget(*raw, true);
    });
    w->root.PointerExited([raw](auto&&, auto&&) {
        raw->hover = false;
        PaintWidget(*raw, true);
    });
    w->root.Tapped([raw](auto&&, wux::Input::TappedRoutedEventArgs const& args) {
        args.Handled(true);
        TogglePanel(*raw);
    });
    w->root.PointerWheelChanged([raw](auto&&, wux::Input::PointerRoutedEventArgs const& args) {
        args.Handled(true);
        int notches = Notches(raw->wheel, args.GetCurrentPoint(raw->root).Properties().MouseWheelDelta());
        if (notches) StepAll(notches * g_prefs.wheelStep);
    });
    return w;
}

void RemoveWidget(Widget& w) {
    if (!w.root) return;
    if (auto panel = Parent(w.root).try_as<wuxc::Panel>()) {
        uint32_t index;
        if (panel.Children().IndexOf(w.root, index)) panel.Children().RemoveAt(index);
    }
}

// Every taskbar gets one; ones whose tray was rebuilt go in again.
void PlaceWidgets() {
    auto taskbars = Taskbars();
    std::erase_if(g_widgets, [&](auto& w) {
        bool gone = std::ranges::find(taskbars, w->taskbar) == taskbars.end();
        if (gone) RemoveWidget(*w);
        return gone;
    });
    for (HWND hwnd : taskbars) {
        auto it = std::ranges::find_if(g_widgets, [&](auto& w) { return w->taskbar == hwnd; });
        if (it != g_widgets.end() && Parent((*it)->root) && (*it)->root.IsLoaded()) {
            if (!(*it)->fitted)
                if (auto tray = Parent((*it)->root).try_as<wuxc::StackPanel>()) (*it)->fitted = FitToTray(**it, tray);
            continue;
        }
        auto tray = TrayPanel(hwnd);
        if (!tray) continue;
        if (it == g_widgets.end()) {
            g_widgets.push_back(BuildWidget(hwnd));
            it = g_widgets.end() - 1;
        } else {
            RemoveWidget(**it);
        }
        // A widget of an earlier load of this mod would be a stranger now.
        for (uint32_t i = tray.Children().Size(); i-- > 0;)
            if (auto e = tray.Children().GetAt(i).try_as<wux::FrameworkElement>(); e && e.Name() == kWidgetName)
                tray.Children().RemoveAt(i);
        tray.Children().InsertAt(InsertIndex(tray), (*it)->root);
        (*it)->fitted = FitToTray(**it, tray);
        PaintWidget(**it, true);
        Wh_Log(L"widget placed on %p", hwnd);
    }
}

// ============================================================================
// Parts in Bloom's design: buttons, segments, switch, number box, panels, row slider
// ============================================================================

constexpr double kInner = 326;  // the window's 348 less its border and padding
constexpr double kTrack = kInner - 24;

const Palette* g_c = &kDark;  // the palette the open window is drawn with

constexpr PCWSTR kGlyphMonitor = L"\uE7F4", kGlyphLaptop = L"\uE7F8", kGlyphLink = L"\uE71B",
                 kGlyphPower = L"\uE7E8", kGlyphSettings = L"\uE713", kGlyphBack = L"\uE72B",
                 kGlyphDown = L"\uE70D", kGlyphUp = L"\uE70E", kGlyphContrast = L"\uE7A1", kGlyphIdle = L"\uE708",
                 kGlyphView = L"\uE890", kGlyphHide = L"\uED1A";

wuxc::TextBlock Text(const std::wstring& s, double size, uint32_t color, int weight = 400) {
    wuxc::TextBlock t;
    t.Text(s);
    t.FontFamily(wuxm::FontFamily(kFont));
    t.FontSize(size);
    t.Foreground(Brush(color));
    if (weight != 400) t.FontWeight(Weight(weight));
    t.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    t.VerticalAlignment(wux::VerticalAlignment::Center);
    return t;
}

wuxc::TextBlock Wrapped(const std::wstring& s, double size, uint32_t color) {
    auto t = Text(s, size, color);
    t.TextWrapping(wux::TextWrapping::Wrap);
    t.TextTrimming(wux::TextTrimming::None);
    return t;
}

// A glyph from Segoe Fluent Icons (Segoe MDL2 Assets on older systems).
wuxc::TextBlock Glyph(PCWSTR glyph, double size, uint32_t color) {
    wuxc::TextBlock t;
    t.Text(glyph);
    t.FontFamily(wuxm::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
    t.FontSize(size);
    t.Foreground(Brush(color));
    t.VerticalAlignment(wux::VerticalAlignment::Center);
    t.HorizontalAlignment(wux::HorizontalAlignment::Center);
    return t;
}

wuxc::Border Box(double radius, uint32_t background) {
    wuxc::Border b;
    b.CornerRadius({radius, radius, radius, radius});
    b.Background(Brush(background));
    return b;
}

wuxc::StackPanel HStack(double spacing) {
    wuxc::StackPanel s;
    s.Orientation(wuxc::Orientation::Horizontal);
    s.Spacing(spacing);
    return s;
}

wuxc::StackPanel VStack(double spacing) {
    wuxc::StackPanel s;
    s.Spacing(spacing);
    return s;
}

// Columns: > 0 pixels, 0 auto, < 0 star.
wuxc::Grid Columns(std::initializer_list<double> widths, double spacing) {
    wuxc::Grid g;
    g.ColumnSpacing(spacing);
    for (double w : widths) {
        wuxc::ColumnDefinition col;
        col.Width(w > 0 ? wux::GridLength{w, wux::GridUnitType::Pixel}
                  : w == 0 ? wux::GridLengthHelper::Auto()
                           : wux::GridLength{-w, wux::GridUnitType::Star});
        g.ColumnDefinitions().Append(col);
    }
    return g;
}

template <class E>
E InColumn(E e, int column) {
    wuxc::Grid::SetColumn(e, column);
    return e;
}

// Handlers of the window's elements, revoked when the page is rebuilt or the window
// closes: a handler holding its own element would otherwise keep it alive for good.
std::vector<std::shared_ptr<void>> g_keep;

template <class R>
void Keep(R&& revoker) {
    g_keep.push_back(std::make_shared<std::decay_t<R>>(std::forward<R>(revoker)));
}

// Drops the page's handlers once the event that asked for it has finished.
void ReleaseHandlers() {
    if (g_keep.empty()) return;
    auto old = std::make_shared<std::vector<std::shared_ptr<void>>>(std::move(g_keep));
    g_keep.clear();
    if (!Post([old] { old->clear(); })) old->clear();
}

template <class E, class F>
void OnClick(const E& e, F&& action) {
    Keep(e.Tapped(winrt::auto_revoke, [action = std::forward<F>(action)](auto&&, wux::Input::TappedRoutedEventArgs const& args) {
        args.Handled(true);
        action();
    }));
}

template <class E, class In, class Out>
void OnHover(const E& e, In&& enter, Out&& leave) {
    Keep(e.PointerEntered(winrt::auto_revoke, [enter = std::forward<In>(enter)](auto&&, auto&&) { enter(); }));
    Keep(e.PointerExited(winrt::auto_revoke, [leave = std::forward<Out>(leave)](auto&&, auto&&) { leave(); }));
}

void SetTip(const wux::FrameworkElement& e, const std::wstring& tip) {
    wuxc::ToolTipService::SetToolTip(e, box_value(tip));
    wux::Automation::AutomationProperties::SetName(e, tip);
}

// ---- buttons ------------------------------------------------------------------------

enum class Kind { kSolid, kGhost };

struct Button {
    wuxc::Border root{nullptr};
    wuxc::TextBlock glyph{nullptr}, label{nullptr};
};

// Height 32 (28 small), radius 8. An empty label makes a square icon button (28, or `square`).
Button MakeButton(Kind kind, PCWSTR glyph, const std::wstring& label, bool small, std::function<void()> action,
                  double square = 28) {
    const Palette& c = *g_c;
    Button b;
    b.root = Box(8, 0);
    bool iconOnly = label.empty();
    b.root.Height(iconOnly ? square : small ? 28 : 32);
    if (iconOnly) b.root.Width(square);
    else b.root.Padding({small ? 10.0 : 12.0, 0, small ? 10.0 : 12.0, 0});
    uint32_t text = kind == Kind::kGhost ? c.muted : c.fg;
    uint32_t bg = kind == Kind::kSolid ? c.btn : 0;
    uint32_t hover = kind == Kind::kSolid ? c.btnHover : Ink(c, 8);
    b.root.Background(Brush(bg));
    if (kind == Kind::kSolid && &c == &kLight) {
        b.root.BorderThickness({1, 1, 1, 1});
        b.root.BorderBrush(Brush(Ink(c, 10)));
    }
    auto content = HStack(6);
    content.HorizontalAlignment(wux::HorizontalAlignment::Center);
    content.VerticalAlignment(wux::VerticalAlignment::Center);
    if (glyph) {
        b.glyph = Glyph(glyph, iconOnly && square < 28 ? 11 : small ? 12 : 14, text);
        content.Children().Append(b.glyph);
    }
    if (!iconOnly) {
        b.label = Text(label, small ? 12 : 13, text, 500);
        content.Children().Append(b.label);
    }
    b.root.Child(content);
    auto root = b.root;
    auto parts = b;
    OnHover(
        b.root,
        [root, parts, hover, kind, cp = &c] {
            root.Background(Brush(hover));
            if (kind == Kind::kGhost) {
                if (parts.glyph) parts.glyph.Foreground(Brush(cp->fg));
                if (parts.label) parts.label.Foreground(Brush(cp->fg));
            }
        },
        [root, parts, bg, text] {
            root.Background(Brush(bg));
            if (parts.glyph) parts.glyph.Foreground(Brush(text));
            if (parts.label) parts.label.Foreground(Brush(text));
        });
    OnClick(b.root, std::move(action));
    return b;
}

// An icon button that stays on: a w10 ground and the accent glyph, no lit line under it.
wuxc::Border MakeIconToggle(PCWSTR glyph, bool on, const std::wstring& tip, std::function<void(bool)> changed) {
    const Palette& c = *g_c;
    auto root = Box(8, 0);
    root.Width(28);
    root.Height(28);
    auto mark = Glyph(glyph, 14, c.muted);
    root.Child(mark);
    SetTip(root, tip);
    auto state = std::make_shared<bool>(on);
    auto hover = std::make_shared<bool>(false);
    auto paint = [root, mark, state, hover, cp = &c] {
        bool v = *state;
        root.Background(Brush(v ? Ink(*cp, 10) : *hover ? Ink(*cp, 8) : 0));
        mark.Foreground(Brush(v ? cp->accent : *hover ? cp->fg : cp->muted));
    };
    paint();
    OnHover(
        root,
        [hover, paint] {
            *hover = true;
            paint();
        },
        [hover, paint] {
            *hover = false;
            paint();
        });
    OnClick(root, [state, paint, changed] {
        *state = !*state;
        paint();
        changed(*state);
    });
    return root;
}

// ---- segments -------------------------------------------------------------------------

struct Segment {
    struct State {
        std::vector<wuxc::Border> items;
        std::vector<wuxc::TextBlock> labels;
        int on = 0;
    };
    std::shared_ptr<State> s;
    wuxc::Border root{nullptr};

    void Select(int index) {
        const Palette& c = *g_c;
        s->on = index;
        for (size_t i = 0; i < s->items.size(); i++) {
            bool sel = int(i) == index;
            s->items[i].Background(Brush(sel ? Ink(c, 15) : 0));
            s->labels[i].Foreground(Brush(sel ? c.fg : c.faint));
        }
    }
};

// w5 track, radius 8, 2 inside; items radius 6, 12 px 500.
Segment MakeSegment(std::initializer_list<std::wstring> names, int on, std::function<void(int)> picked) {
    const Palette& c = *g_c;
    Segment seg;
    seg.s = std::make_shared<Segment::State>();
    seg.root = Box(8, Ink(c, 5));
    seg.root.Padding({2, 2, 2, 2});
    auto row = HStack(2);
    int i = 0;
    for (auto& name : names) {
        auto item = Box(6, 0);
        item.Height(24);
        item.Padding({10, 0, 10, 0});
        auto label = Text(name, 12, c.faint, 500);
        label.HorizontalAlignment(wux::HorizontalAlignment::Center);
        item.Child(label);
        row.Children().Append(item);
        seg.s->items.push_back(item);
        seg.s->labels.push_back(label);
        auto state = seg.s;
        OnHover(
            item, [label, cp = &c] { label.Foreground(Brush(cp->fg)); },
            [label, state, i, cp = &c] { label.Foreground(Brush(state->on == i ? cp->fg : cp->faint)); });
        OnClick(item, [seg, i, picked] {
            auto copy = seg;
            copy.Select(i);
            picked(i);
        });
        i++;
    }
    seg.root.Child(row);
    seg.Select(on);
    return seg;
}

// ---- the switch: 40 x 22, lit line inside and a lit knob when on ------------------

wuxc::Border MakeSwitch(bool on, std::function<void(bool)> changed) {
    const Palette& c = *g_c;
    auto track = Box(11, 0);
    track.Width(40);
    track.Height(22);
    track.BorderThickness({1, 1, 1, 1});
    wuxc::Canvas canvas;
    wuxs::Rectangle glowLine, line;
    for (auto* r : {&glowLine, &line}) {
        r->Fill(Brush(r == &line ? c.accent : WithAlpha(c.accent, 0x40)));
        r->RadiusX(1);
        r->RadiusY(1);
        canvas.Children().Append(*r);
    }
    line.Height(2);
    wuxc::Canvas::SetLeft(line, 6);
    wuxc::Canvas::SetTop(line, 9);
    glowLine.Height(6);
    wuxc::Canvas::SetLeft(glowLine, 4);
    wuxc::Canvas::SetTop(glowLine, 7);
    wuxs::Ellipse halo, knob;
    halo.Width(22);
    halo.Height(22);
    halo.Fill(Brush(WithAlpha(c.accent, 0x30)));
    halo.IsHitTestVisible(false);
    knob.Width(14);
    knob.Height(14);
    knob.StrokeThickness(1.5);
    wuxc::Canvas::SetTop(halo, -1);
    wuxc::Canvas::SetTop(knob, 3);
    canvas.Children().Append(halo);
    canvas.Children().Append(knob);
    track.Child(canvas);
    auto state = std::make_shared<bool>(on);
    auto paint = [track, line, glowLine, halo, knob, state, cp = &c] {
        bool v = *state;
        track.BorderBrush(Brush(Ink(*cp, v ? 15 : 25)));
        line.Width(v ? 18 : 0);
        glowLine.Width(v ? 22 : 0);
        halo.Visibility(v ? wux::Visibility::Visible : wux::Visibility::Collapsed);
        wuxc::Canvas::SetLeft(halo, v ? 17 : -1);
        wuxc::Canvas::SetLeft(knob, v ? 21 : 3);
        knob.Fill(Brush(v ? cp->accent : 0));
        knob.Stroke(Brush(v ? cp->accent : cp->faint));
    };
    paint();
    track.Background(Brush(0));
    OnClick(track, [state, paint, changed] {
        *state = !*state;
        paint();
        changed(*state);
    });
    return track;
}

// ---- the number box: press to type, the wheel over it steps by 1 --------------------------

void StyleTextBox(const wuxc::TextBox& box, const Palette& c) {
    box.FontFamily(wuxm::FontFamily(kFont));
    box.FontSize(12);
    box.Padding({0, 0, 0, 0});
    box.MinWidth(0);
    box.MinHeight(0);
    box.BorderThickness({0, 0, 0, 0});
    box.VerticalAlignment(wux::VerticalAlignment::Center);
    box.IsSpellCheckEnabled(false);
    auto res = box.Resources();
    for (auto [key, argb] : std::initializer_list<std::pair<PCWSTR, uint32_t>>{
             {L"TextControlBackground", 0}, {L"TextControlBackgroundPointerOver", 0},
             {L"TextControlBackgroundFocused", 0}, {L"TextControlBorderBrush", 0},
             {L"TextControlBorderBrushPointerOver", 0}, {L"TextControlBorderBrushFocused", 0},
             {L"TextControlForeground", c.fg}, {L"TextControlForegroundPointerOver", c.fg},
             {L"TextControlForegroundFocused", c.fg}, {L"TextControlSelectionHighlightColor", c.accent}})
        res.Insert(box_value(key), Brush(argb));
    res.Insert(box_value(L"TextControlThemeMinHeight"), box_value(0.0));
    res.Insert(box_value(L"TextControlThemeMinWidth"), box_value(0.0));
    res.Insert(box_value(L"TextControlThemePadding"), box_value(wux::Thickness{0, 0, 0, 0}));
    res.Insert(box_value(L"TextControlBorderThemeThickness"), box_value(wux::Thickness{0, 0, 0, 0}));
}

wux::Input::InputScope NumberScope() {
    wux::Input::InputScope scope;
    wux::Input::InputScopeName name;
    name.NameValue(wux::Input::InputScopeNameValue::Number);
    scope.Names().Append(name);
    return scope;
}

// Reads the number typed into `box`, or `fallback`.
int TypedNumber(const wuxc::TextBox& box, int fallback) {
    std::wstring text(box.Text());
    wchar_t* end = nullptr;
    long v = wcstol(text.c_str(), &end, 10);
    return end && end != text.c_str() ? int(v) : fallback;
}

// w8 field, height 28, radius 8: the number, then its unit in the faint colour. A `width`
// gives a fixed field with the number in the middle (the range fields).
wuxc::Border NumberBox(int value, int lo, int hi, PCWSTR unit, std::function<void(int)> changed, double width = 0) {
    const Palette& c = *g_c;
    auto field = Box(8, Ink(c, 8));
    field.Height(28);
    field.BorderThickness({1, 1, 1, 1});
    field.BorderBrush(Brush(0));
    if (width) field.Width(width);
    else field.Padding({10, 0, 10, 0});
    auto row = HStack(3);
    row.VerticalAlignment(wux::VerticalAlignment::Center);
    row.HorizontalAlignment(wux::HorizontalAlignment::Center);
    wuxc::TextBox box;
    StyleTextBox(box, c);
    int digits = hi >= 1000 ? 4 : hi >= 100 ? 3 : 2;
    box.Width(7.0 * digits);
    box.TextAlignment(width ? wux::TextAlignment::Center : wux::TextAlignment::Right);
    box.InputScope(NumberScope());
    box.Text(std::to_wstring(value));
    row.Children().Append(box);
    if (unit && *unit) row.Children().Append(Text(unit, 12, c.faint));
    field.Child(row);
    auto current = std::make_shared<int>(value);
    auto set = [box, current, lo, hi, changed](int v) {
        v = std::clamp(v, lo, hi);
        box.Text(std::to_wstring(v));
        if (v == *current) return;
        *current = v;
        changed(v);
    };
    auto commit = [box, current, set] { set(TypedNumber(box, *current)); };
    OnHover(
        field, [field, cp = &c] { field.BorderBrush(Brush(Ink(*cp, 25))); }, [field] { field.BorderBrush(Brush(0)); });
    OnClick(field, [box] {
        box.Focus(wux::FocusState::Programmatic);
        box.SelectAll();
    });
    Keep(box.GotFocus(winrt::auto_revoke, [box](auto&&, auto&&) { box.SelectAll(); }));
    Keep(box.LostFocus(winrt::auto_revoke, [commit](auto&&, auto&&) { commit(); }));
    Keep(box.KeyDown(winrt::auto_revoke, [commit](auto&&, wux::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
            args.Handled(true);
            commit();
        }
    }));
    Keep(field.PointerWheelChanged(winrt::auto_revoke, [field, current, set](auto&&, wux::Input::PointerRoutedEventArgs const& args) {
        args.Handled(true);
        int delta = args.GetCurrentPoint(field).Properties().MouseWheelDelta();
        if (delta) set(*current + (delta > 0 ? 1 : -1));
    }));
    return field;
}

// ---- panels, rows, section heads ----------------------------------------------------------

// A w5 panel, radius 12; its rows are parted by a w5 line.
wuxc::Border PanelOf(const std::vector<wux::FrameworkElement>& rows) {
    const Palette& c = *g_c;
    auto panel = Box(12, Ink(c, 5));
    auto stack = VStack(0);
    for (size_t i = 0; i < rows.size(); i++) {
        wuxc::Border wrap;
        if (i) {
            wrap.BorderThickness({0, 1, 0, 0});
            wrap.BorderBrush(Brush(Ink(c, 5)));
        }
        wrap.Child(rows[i]);
        stack.Children().Append(wrap);
    }
    panel.Child(stack);
    return panel;
}

// Title (13, 500) with a quieter line under it, then controls at the right.
wuxc::Grid OptionRow(const wux::FrameworkElement& lead, const std::wstring& title, const std::wstring& sub,
                     const std::vector<wux::FrameworkElement>& right) {
    const Palette& c = *g_c;
    wuxc::Grid g;
    g.ColumnSpacing(12);
    g.Padding({12, 8, 12, 8});
    g.MinHeight(48);
    int column = 0;
    auto add = [&](const wux::FrameworkElement& e, bool star) {
        wuxc::ColumnDefinition col;
        col.Width(star ? wux::GridLength{1, wux::GridUnitType::Star} : wux::GridLengthHelper::Auto());
        g.ColumnDefinitions().Append(col);
        e.VerticalAlignment(wux::VerticalAlignment::Center);
        wuxc::Grid::SetColumn(e, column++);
        g.Children().Append(e);
    };
    if (lead) add(lead, false);
    auto texts = VStack(1);
    if (!title.empty()) texts.Children().Append(Text(title, 13, c.fg, 500));
    if (!sub.empty()) texts.Children().Append(Wrapped(sub, 12, c.faint));
    add(texts, true);
    for (auto& e : right) add(e, false);
    return g;
}

wuxc::Grid SectionHead(const std::wstring& title) {
    const Palette& c = *g_c;
    auto g = Columns({-1}, 8);
    g.Margin({2, 16, 2, 6});
    g.Children().Append(Text(title, 12, c.muted, 600));
    return g;
}

// A key cap: w8, radius 4, height 20, 11 px 500 in the muted colour.
wuxc::Border KeyCap(const std::wstring& label) {
    const Palette& c = *g_c;
    auto cap = Box(4, Ink(c, 8));
    cap.Height(20);
    cap.Padding({6, 0, 6, 0});
    cap.Child(Text(label, 11, c.muted, 500));
    return cap;
}

// ---- the row slider: name and value on one line, a thin track, a tick at the value -------

struct Slider {
    struct State {
        int value = 0, wheel = 0;
        bool dragging = false, typing = false;
        wuxc::Border fill{nullptr}, tick{nullptr};
        wuxc::TextBlock text{nullptr};
        wuxc::TextBox box{nullptr};
        bool percent = true;
        std::function<void(int)> changed;
    };
    std::shared_ptr<State> s;
    wuxc::StackPanel root{nullptr};

    static void Show(State& s, int v) {
        s.value = v;
        double x = kTrack * std::clamp(v, 0, 100) / 100.0;
        s.fill.Width(x);
        s.tick.Margin({std::clamp(x - 1, 0.0, kTrack - 2), 0, 0, 0});
        s.text.Text(v < 0 ? std::wstring(L"—") : s.percent ? Format(L"%d %%", v) : std::to_wstring(v));
    }
    void Set(int v) {
        if (!s->dragging && !s->typing) Show(*s, v);
    }
};

// `sub`: the quieter contrast line under a monitor (20 high, 12 px, the value in fg).
Slider MakeSlider(PCWSTR glyph, const std::wstring& name, int value, bool percent, bool sub,
                  const wux::FrameworkElement& extra, std::function<void(int)> changed) {
    const Palette& c = *g_c;
    Slider sl;
    sl.s = std::make_shared<Slider::State>();
    auto s = sl.s;
    s->percent = percent;
    s->changed = std::move(changed);
    sl.root = VStack(4);

    auto top = Columns({0, -1, 0, 0}, 8);
    top.Height(sub ? 20 : 22);
    top.Children().Append(Glyph(glyph, 14, c.muted));
    top.Children().Append(InColumn(Text(name, sub ? 12 : 13, sub ? c.muted : c.fg), 1));
    // The value: a number, or a box to type one into after a press.
    wuxc::Grid valueHost;
    valueHost.Background(Brush(0));
    s->text = Text(L"", sub ? 12 : 13, c.fg, 600);
    s->text.HorizontalAlignment(wux::HorizontalAlignment::Right);
    s->box = wuxc::TextBox();
    StyleTextBox(s->box, c);
    s->box.FontWeight(Weight(600));
    s->box.FontSize(sub ? 12 : 13);
    s->box.Width(30);
    s->box.TextAlignment(wux::TextAlignment::Right);
    s->box.InputScope(NumberScope());
    s->box.Visibility(wux::Visibility::Collapsed);
    valueHost.Children().Append(s->text);
    valueHost.Children().Append(s->box);
    top.Children().Append(InColumn(valueHost, 2));
    if (extra) top.Children().Append(InColumn(extra, 3));
    sl.root.Children().Append(top);

    wuxc::Grid track;
    track.Height(14);
    track.Width(kTrack);
    track.HorizontalAlignment(wux::HorizontalAlignment::Left);
    track.Background(Brush(0));
    auto line = Box(2, Ink(c, 8));
    line.Height(4);
    line.VerticalAlignment(wux::VerticalAlignment::Center);
    s->fill = Box(2, c.accent);
    s->fill.HorizontalAlignment(wux::HorizontalAlignment::Left);
    line.Child(s->fill);
    s->tick = Box(1, c.fg);
    s->tick.Width(2);
    s->tick.Height(14);
    s->tick.HorizontalAlignment(wux::HorizontalAlignment::Left);
    s->tick.IsHitTestVisible(false);
    track.Children().Append(line);
    track.Children().Append(s->tick);
    sl.root.Children().Append(track);
    Slider::Show(*s, value);

    auto setTo = [s](int v) {
        v = std::clamp(v, 0, 100);
        if (v == s->value) return;
        Slider::Show(*s, v);
        s->changed(v);
    };
    auto atX = [setTo](double x) { setTo(int(std::lround(x / kTrack * 100))); };
    Keep(track.PointerPressed(winrt::auto_revoke, [s, track, atX](auto&&, wux::Input::PointerRoutedEventArgs const& args) {
        args.Handled(true);
        s->dragging = track.CapturePointer(args.Pointer());
        atX(args.GetCurrentPoint(track).Position().X);
    }));
    Keep(track.PointerMoved(winrt::auto_revoke, [s, track, atX](auto&&, wux::Input::PointerRoutedEventArgs const& args) {
        if (s->dragging) atX(args.GetCurrentPoint(track).Position().X);
    }));
    Keep(track.PointerReleased(winrt::auto_revoke, [s, track](auto&&, wux::Input::PointerRoutedEventArgs const& args) {
        s->dragging = false;
        track.ReleasePointerCapture(args.Pointer());
    }));
    Keep(track.PointerCaptureLost(winrt::auto_revoke, [s](auto&&, auto&&) { s->dragging = false; }));
    Keep(sl.root.PointerWheelChanged(winrt::auto_revoke, [s, setTo, root = sl.root](auto&&, wux::Input::PointerRoutedEventArgs const& args) {
        args.Handled(true);
        int notches = Notches(s->wheel, args.GetCurrentPoint(root).Properties().MouseWheelDelta());
        if (notches && s->value >= 0) setTo(s->value + notches);
    }));
    // Pressing the value types one; Enter or leaving keeps it, Escape drops it.
    auto endTyping = [s, setTo](bool keep) {
        if (!s->typing) return;
        s->typing = false;
        int v = TypedNumber(s->box, s->value);
        s->box.Visibility(wux::Visibility::Collapsed);
        s->text.Visibility(wux::Visibility::Visible);
        if (keep) setTo(v);
    };
    OnClick(valueHost, [s] {
        if (s->typing || s->value < 0) return;
        s->typing = true;
        s->box.Text(std::to_wstring(s->value));
        s->text.Visibility(wux::Visibility::Collapsed);
        s->box.Visibility(wux::Visibility::Visible);
        s->box.Focus(wux::FocusState::Programmatic);
        s->box.SelectAll();
    });
    Keep(s->box.LostFocus(winrt::auto_revoke, [endTyping](auto&&, auto&&) { endTyping(true); }));
    Keep(s->box.KeyDown(winrt::auto_revoke, [endTyping](auto&&, wux::Input::KeyRoutedEventArgs const& args) {
        auto key = args.Key();
        if (key == winrt::Windows::System::VirtualKey::Enter || key == winrt::Windows::System::VirtualKey::Escape) {
            args.Handled(true);
            endTyping(key == winrt::Windows::System::VirtualKey::Enter);
        }
    }));
    return sl;
}

// ============================================================================
// Hotkeys and the one-flyout rule, on a message-only window of the taskbar thread
// ============================================================================

// Every Bloom add-on's flyout window carries this title; its class is the add-on's own.
constexpr PCWSTR kFlyoutTitle = L"Bloom.Addon.Flyout";
constexpr PCWSTR kFlyoutClass = L"Bloom.Addon.brightness.Flyout";
constexpr int kKeyUp = 1, kKeyDown = 2;

HWND g_flyoutWnd;  // taskbar thread
bool g_keyBusy[2];  // registering failed: another program holds the combination
int g_recording;    // 0, or kKeyUp / kKeyDown while that field waits for a combination

UINT CloseFlyoutMsg() {
    static UINT msg = RegisterWindowMessageW(L"Bloom.Addon.CloseFlyout");
    return msg;
}

void ClosePanel();

void RegisterKeys() {
    if (!g_flyoutWnd) return;
    UnregisterHotKey(g_flyoutWnd, kKeyUp);
    UnregisterHotKey(g_flyoutWnd, kKeyDown);
    g_keyBusy[0] = g_keyBusy[1] = false;
    if (g_recording) return;  // the combination being pressed must reach the field
    if (g_prefs.up.vk) g_keyBusy[0] = !RegisterHotKey(g_flyoutWnd, kKeyUp, g_prefs.up.mods, g_prefs.up.vk);
    if (g_prefs.down.vk) g_keyBusy[1] = !RegisterHotKey(g_flyoutWnd, kKeyDown, g_prefs.down.mods, g_prefs.down.vk);
}

LRESULT CALLBACK FlyoutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == CloseFlyoutMsg()) {
        if (!g_unloading) ClosePanel();
        return 0;
    }
    if (msg == WM_HOTKEY) {
        if (!g_unloading && (wParam == kKeyUp || wParam == kKeyDown))
            StepAll(wParam == kKeyUp ? g_prefs.keyStep : -g_prefs.keyStep);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateFlyoutWindow() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = FlyoutWndProc;
    wc.hInstance = ModuleInstance();
    wc.lpszClassName = kFlyoutClass;
    RegisterClassExW(&wc);
    g_flyoutWnd = CreateWindowExW(0, kFlyoutClass, kFlyoutTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  wc.hInstance, nullptr);
    if (!g_flyoutWnd) Wh_Log(L"flyout window not created: %u", GetLastError());
    RegisterKeys();
}

void DestroyFlyoutWindow() {
    if (g_flyoutWnd) {
        UnregisterHotKey(g_flyoutWnd, kKeyUp);
        UnregisterHotKey(g_flyoutWnd, kKeyDown);
        DestroyWindow(g_flyoutWnd);
        g_flyoutWnd = nullptr;
    }
    UnregisterClassW(kFlyoutClass, ModuleInstance());
}

// Before this add-on's window opens, every other add-on's closes.
void CloseOtherFlyouts() {
    for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, nullptr, kFlyoutTitle));)
        if (w != g_flyoutWnd) PostMessageW(w, CloseFlyoutMsg(), 0, 0);
}

std::wstring KeyName(UINT vk) {
    switch (vk) {
        case VK_UP: return L"↑";
        case VK_DOWN: return L"↓";
        case VK_LEFT: return L"←";
        case VK_RIGHT: return L"→";
    }
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lparam = LONG(scan << 16);
    if (vk == VK_PRIOR || vk == VK_NEXT || vk == VK_END || vk == VK_HOME || vk == VK_INSERT || vk == VK_DELETE)
        lparam |= 1 << 24;
    wchar_t name[64];
    if (scan && GetKeyNameTextW(lparam, name, ARRAYSIZE(name)) > 0) return name;
    return Format(L"0x%02X", vk);
}

std::vector<std::wstring> KeyParts(const HotKey& k) {
    std::vector<std::wstring> parts;
    if (k.mods & MOD_CONTROL) parts.push_back(L"Ctrl");
    if (k.mods & MOD_ALT) parts.push_back(L"Alt");
    if (k.mods & MOD_SHIFT) parts.push_back(L"Shift");
    if (k.mods & MOD_WIN) parts.push_back(L"Win");
    parts.push_back(KeyName(k.vk));
    return parts;
}

// ============================================================================
// Dimming while idle
// ============================================================================

winrt::Windows::System::DispatcherQueueTimer g_idleTimer{nullptr};
bool g_dimmed = false;
DWORD g_dimInputAt = 0;               // GetLastInputInfo's time when the dim began
std::map<std::wstring, int> g_dimFrom;  // values to go back to

bool DisplayRequired() {
    EXECUTION_STATE state = 0;
    return CallNtPowerInformation(SystemExecutionState, nullptr, 0, &state, sizeof(state)) == 0 &&
           (state & ES_DISPLAY_REQUIRED);
}

void SetIdleInterval() {
    if (!g_idleTimer) return;
    if (g_dimmed || g_prefs.idleOn) {
        g_idleTimer.Interval(std::chrono::seconds(g_dimmed ? 1 : 30));
        g_idleTimer.Start();
    } else {
        g_idleTimer.Stop();
    }
}

// Back to the values from before the dim; a key or wheel that wakes it acts on those.
void Undim() {
    if (!g_dimmed) return;
    auto from = std::move(g_dimFrom);
    g_dimmed = false;
    g_dimFrom.clear();
    for (auto& m : g_mons)
        if (auto it = from.find(m.id); it != from.end() && m.ok) SetValue(m, it->second);
    SetIdleInterval();
}

void IdleTick() {
    LASTINPUTINFO input{sizeof(input)};
    if (!GetLastInputInfo(&input)) return;
    if (g_dimmed) {
        if (input.dwTime == g_dimInputAt) return;
        Undim();
        PaintWidgets();
        RefreshMain();
        return;
    }
    if (!g_prefs.idleOn || GetTickCount() - input.dwTime < DWORD(g_prefs.idleMinutes) * 60000 || DisplayRequired())
        return;
    for (auto& m : g_mons) {
        if (!Shown(m) || m.value <= g_prefs.idleLevel) continue;
        g_dimFrom[m.id] = m.value;
        SetValue(m, g_prefs.idleLevel);
    }
    if (g_dimFrom.empty()) return;
    g_dimmed = true;
    g_dimInputAt = input.dwTime;
    Wh_Log(L"dimmed after %u s idle", (GetTickCount() - input.dwTime) / 1000);
    SetIdleInterval();
    PaintWidgets();
}

// ============================================================================
// The window: the monitors and the settings, as a flyout over the widget
// ============================================================================

enum class Page { kMain, kSettings };

struct PanelState {
    HWND taskbar = nullptr;
    const Palette* c = &kDark;
    wuxc::Flyout flyout{nullptr};
    wuxc::Border bubble{nullptr}, headHost{nullptr}, pageHost{nullptr};
    Page page = Page::kMain;
    std::set<std::wstring> open;  // monitors whose contrast and power are shown
    std::map<std::wstring, Slider> sliders, contrasts;
    std::vector<std::wstring> listed;  // ids on the page, to tell when to rebuild it
};
std::unique_ptr<PanelState> g_panel;
uint64_t g_panelClosedAt = 0;

void ShowPage(Page page);

// "은" or "는" after a name, by how its last letter is read.
PCWSTR Topic(const std::wstring& name) {
    if (name.empty()) return L"는";
    wchar_t last = name.back();
    if (last >= 0xAC00 && last <= 0xD7A3) return (last - 0xAC00) % 28 ? L"은" : L"는";
    if (wcschr(L"013678LMNRlmnr", last)) return L"은";
    return L"는";
}

std::vector<std::wstring> ListedIds() {
    std::vector<std::wstring> ids;
    for (auto& m : g_mons) ids.push_back(m.id + (m.ok ? L"+" : L"-") + (m.contrastOk ? L"c" : L"") +
                                         (PrefOf(m.id).hidden ? L"h" : L""));
    return ids;
}

wux::FrameworkElement BuildMonitorRow(const Mon& m) {
    auto& p = *g_panel;
    const Palette& c = *p.c;
    std::wstring id = m.id;
    auto row = VStack(0);
    row.Padding({12, 10, 12, 12});
    bool open = p.open.contains(id);
    wux::FrameworkElement chevron{nullptr};
    if (!m.internal) {
        auto more = MakeButton(Kind::kGhost, open ? kGlyphUp : kGlyphDown, L"", true,
                               [id] {
                                   if (!g_panel) return;
                                   if (!g_panel->open.erase(id)) g_panel->open.insert(id);
                                   ShowPage(Page::kMain);
                               },
                               24);
        more.root.Margin({0, 0, -4, 0});
        if (open) more.root.Background(Brush(Ink(c, 8)));
        SetTip(more.root, T(L"대비와 전원", L"Contrast and power"));
        chevron = more.root;
    }
    auto slider = MakeSlider(m.internal ? kGlyphLaptop : kGlyphMonitor, m.name, m.value, true, false, chevron,
                             [id](int v) { ChangeOne(id, v); });
    row.Children().Append(slider.root);
    p.sliders[id] = slider;
    if (open) {
        auto more = VStack(10);
        more.Margin({0, 12, 0, 0});
        if (m.contrastOk) {
            auto contrast = MakeSlider(kGlyphContrast, T(L"대비", L"Contrast"), m.contrast, false, true, nullptr,
                                       [id](int v) { ChangeContrast(id, v); });
            more.Children().Append(contrast.root);
            p.contrasts[id] = contrast;
        }
        auto act = HStack(10);
        auto off = MakeButton(Kind::kSolid, kGlyphPower, T(L"이 모니터 끄기", L"Turn this monitor off"), true,
                              [id] { QueueWrite(id, What::kPower, 0); });
        act.Children().Append(off.root);
        act.Children().Append(Text(T(L"전원 버튼으로 다시 켭니다", L"Its power button turns it on"), 12, c.faint));
        more.Children().Append(act);
        row.Children().Append(more);
    }
    return row;
}

std::wstring IdleFoot() {
    return g_prefs.idleOn ? Format(T(L"%d분 뒤 %d %%", L"%d %% after %d min"), g_korean ? g_prefs.idleMinutes : g_prefs.idleLevel,
                                   g_korean ? g_prefs.idleLevel : g_prefs.idleMinutes)
                          : std::wstring(T(L"유휴 시 어둡게 꺼짐", L"Dim when idle is off"));
}

wux::UIElement BuildMain() {
    auto& p = *g_panel;
    const Palette& c = *p.c;
    auto page = VStack(0);
    p.sliders.clear();
    p.contrasts.clear();
    p.listed = ListedIds();

    std::vector<wux::FrameworkElement> rows;
    std::vector<std::wstring> off;
    for (auto& m : g_mons) {
        if (!m.ok) off.push_back(m.name);
        else if (Shown(m)) rows.push_back(BuildMonitorRow(m));
    }
    if (rows.empty()) {
        auto empty = OptionRow(Glyph(kGlyphMonitor, 14, c.muted), L"",
                               !g_scanned ? T(L"모니터를 찾는 중…", L"Looking for monitors…")
                               : off.empty() ? T(L"보이게 둔 모니터가 없습니다. 설정에서 고릅니다.",
                                                 L"No monitor is shown. Pick them in the settings.")
                                             : T(L"밝기를 바꿀 수 있는 모니터가 없습니다.",
                                                 L"No monitor whose brightness can be set."),
                               {});
        rows.push_back(empty);
    }
    auto list = PanelOf(rows);
    list.Margin({0, 8, 0, 0});
    page.Children().Append(list);

    // Monitors that do not answer: say why, with a way to look again.
    if (!off.empty()) {
        std::wstring names;
        for (auto& n : off) names += (names.empty() ? L"" : L", ") + n;
        auto notice = Box(12, Ink(c, 5));
        notice.Margin({0, 8, 0, 0});
        notice.Padding({12, 10, 10, 10});
        auto line = Columns({0, -1, 0}, 10);
        line.Children().Append(Glyph(kGlyphMonitor, 14, c.muted));
        line.Children().Append(InColumn(
            Wrapped(g_korean ? Format(L"%s%s DDC/CI가 꺼져 있어 밝기를 바꿀 수 없습니다. 모니터 메뉴에서 켜십시오.",
                                      names.c_str(), Topic(names))
                             : Format(L"%s: DDC/CI is off, so its brightness cannot be set. Turn it on in the "
                                      L"monitor's menu.",
                                      names.c_str()),
                    12, c.muted),
            1));
        auto again = MakeButton(Kind::kSolid, nullptr, T(L"다시 찾기", L"Look again"), true, [] { RequestScan(); });
        again.root.VerticalAlignment(wux::VerticalAlignment::Center);
        line.Children().Append(InColumn(again.root, 2));
        notice.Child(line);
        page.Children().Append(notice);
    }

    wuxc::Border foot;
    foot.BorderThickness({0, 1, 0, 0});
    foot.BorderBrush(Brush(Ink(c, 8)));
    foot.Padding({4, 8, 4, 0});
    foot.Margin({0, 8, 0, 0});
    auto footRow = Columns({0, -1, 0}, 8);
    footRow.Children().Append(Glyph(kGlyphIdle, 12, c.faint));
    footRow.Children().Append(InColumn(Text(IdleFoot(), 12, c.faint), 1));
    auto keys = HStack(4);
    auto& up = g_prefs.up;
    auto& down = g_prefs.down;
    if (up.vk && down.vk && up.mods == down.mods) {
        auto parts = KeyParts(up);
        parts.push_back(KeyName(down.vk));
        for (auto& k : parts) keys.Children().Append(KeyCap(k));
    } else {
        for (auto* k : {&up, &down})
            if (k->vk)
                for (auto& part : KeyParts(*k)) keys.Children().Append(KeyCap(part));
    }
    footRow.Children().Append(InColumn(keys, 2));
    foot.Child(footRow);
    page.Children().Append(foot);
    return page;
}

// New numbers into the open main page; a changed list rebuilds it.
void RefreshMain() {
    if (!g_panel || g_panel->page != Page::kMain) return;
    auto& p = *g_panel;
    if (p.listed != ListedIds()) {
        ShowPage(Page::kMain);
        return;
    }
    for (auto& m : g_mons) {
        if (auto it = p.sliders.find(m.id); it != p.sliders.end()) it->second.Set(m.value);
        if (auto it = p.contrasts.find(m.id); it != p.contrasts.end()) it->second.Set(m.contrast);
    }
}

// ---- settings page --------------------------------------------------------------------

// A hotkey field: its key caps; pressed, it waits for the next combination.
wuxc::UserControl HotKeyField(int which) {
    const Palette& c = *g_c;
    HotKey key = which == kKeyUp ? g_prefs.up : g_prefs.down;
    bool recording = g_recording == which;
    wuxc::UserControl field;
    field.IsTabStop(true);
    field.UseSystemFocusVisuals(false);
    auto box = Box(8, Ink(c, 8));
    box.Height(28);
    box.Padding({recording ? 8.0 : 6.0, 0, recording ? 10.0 : 6.0, 0});
    box.BorderThickness({1, 1, 1, 1});
    box.BorderBrush(Brush(recording ? Ink(c, 25) : 0));
    auto row = HStack(4);
    row.VerticalAlignment(wux::VerticalAlignment::Center);
    if (recording) {
        auto caret = Box(0, c.accent);
        caret.Width(1.5);
        caret.Height(14);
        row.Children().Append(caret);
        row.Children().Append(Text(T(L"키를 누르십시오", L"Press the keys"), 12, c.faint));
    } else if (key.vk) {
        for (auto& part : KeyParts(key)) row.Children().Append(KeyCap(part));
    } else {
        row.Children().Append(Text(T(L"없음", L"None"), 12, c.faint));
    }
    box.Child(row);
    field.Content(box);
    if (!recording)
        OnHover(
            box, [box, cp = &c] { box.BorderBrush(Brush(Ink(*cp, 25))); }, [box] { box.BorderBrush(Brush(0)); });
    OnClick(box, [which] {
        g_recording = g_recording == which ? 0 : which;
        RegisterKeys();
        ShowPage(Page::kSettings);
    });
    if (recording) {
        Keep(field.Loaded(winrt::auto_revoke, [field](auto&&, auto&&) { field.Focus(wux::FocusState::Programmatic); }));
        Keep(field.LostFocus(winrt::auto_revoke, [](auto&&, auto&&) {
            if (!g_recording) return;
            g_recording = 0;
            RegisterKeys();
            Post([] {
                if (g_panel && g_panel->page == Page::kSettings) ShowPage(Page::kSettings);
            });
        }));
        Keep(field.PreviewKeyDown(winrt::auto_revoke, [which](auto&&, wux::Input::KeyRoutedEventArgs const& args) {
            args.Handled(true);
            UINT vk = UINT(args.Key());
            if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN ||
                vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT ||
                vk == VK_RSHIFT)
                return;  // wait for the key that completes it
            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
            bool function = vk >= VK_F1 && vk <= VK_F24;
            HotKey& target = which == kKeyUp ? g_prefs.up : g_prefs.down;
            if (vk == VK_ESCAPE && !mods) {
                // keep the old one
            } else if ((vk == VK_BACK || vk == VK_DELETE) && !mods) {
                target = {};
                SavePrefs();
            } else if (mods || function) {
                target = {mods, vk};
                SavePrefs();
            } else {
                return;  // a plain key alone would take that key from every program
            }
            g_recording = 0;
            RegisterKeys();
            Post([] {
                if (g_panel && g_panel->page == Page::kSettings) ShowPage(Page::kSettings);
            });
        }));
    }
    return field;
}

wux::UIElement Scrolled(const wux::UIElement& content, double maxHeight) {
    wuxc::ScrollViewer scroll;
    scroll.MaxHeight(maxHeight);
    scroll.HorizontalScrollBarVisibility(wuxc::ScrollBarVisibility::Disabled);
    scroll.VerticalScrollBarVisibility(wuxc::ScrollBarVisibility::Auto);
    scroll.Content(content);
    return scroll;
}

void PrefsChanged() {
    SavePrefs();
    for (auto& m : g_mons) m.value = FromHw(m, m.hw);
    PaintWidgets(true);
}

wux::UIElement BuildSettings() {
    auto& p = *g_panel;
    const Palette& c = *p.c;
    auto page = VStack(0);

    page.Children().Append(SectionHead(T(L"작업표시줄", L"Taskbar")));
    auto number = MakeSegment({T(L"첫 모니터", L"First monitor"), T(L"평균", L"Average")}, g_prefs.pillNumber, [](int i) {
        g_prefs.pillNumber = i;
        PrefsChanged();
    });
    auto wheel = NumberBox(g_prefs.wheelStep, 1, 25, L"%", [](int v) {
        g_prefs.wheelStep = v;
        SavePrefs();
    });
    page.Children().Append(PanelOf({OptionRow(nullptr, T(L"알약 숫자", L"Number on the taskbar"), L"", {number.root}),
                                     OptionRow(nullptr, T(L"휠 한 칸", L"One wheel notch"), L"", {wheel})}));

    if (!g_mons.empty()) {
        page.Children().Append(SectionHead(T(L"범위 맞추기", L"Ranges")));
        std::vector<wux::FrameworkElement> rows;
        for (auto& m : g_mons) {
            std::wstring id = m.id;
            MonPref mp = PrefOf(id);
            auto range = HStack(4);
            range.VerticalAlignment(wux::VerticalAlignment::Center);
            range.Children().Append(NumberBox(mp.lo, 0, 99, L"", [id](int v) {
                auto& e = EditPref(id);
                e.lo = v;
                e.hi = std::max(e.hi, v + 1);
                PrefsChanged();
            }, 46));
            range.Children().Append(Text(L"–", 12, c.faint));
            range.Children().Append(NumberBox(mp.hi, 1, 100, L"", [id](int v) {
                auto& e = EditPref(id);
                e.hi = v;
                e.lo = std::min(e.lo, v - 1);
                PrefsChanged();
            }, 46));
            auto eye = MakeButton(Kind::kGhost, mp.hidden ? kGlyphHide : kGlyphView, L"", true, [id] {
                auto& e = EditPref(id);
                e.hidden = !e.hidden;
                PrefsChanged();
                ShowPage(Page::kSettings);
            });
            SetTip(eye.root, mp.hidden ? T(L"창에 보이기", L"Show in the window") : T(L"창에서 숨기기", L"Hide from the window"));
            auto lead = Glyph(m.internal ? kGlyphLaptop : kGlyphMonitor, 14, c.muted);
            auto row = OptionRow(lead, m.name, m.ok ? L"" : T(L"DDC/CI 꺼짐", L"DDC/CI off"), {range, eye.root});
            if (mp.hidden) row.Opacity(0.6);
            rows.push_back(row);
        }
        page.Children().Append(PanelOf(rows));
    }

    page.Children().Append(SectionHead(T(L"단축키", L"Hotkeys")));
    auto keyRow = [&](int which, PCWSTR title) {
        bool busy = g_keyBusy[which - 1];
        return OptionRow(nullptr, title, busy ? T(L"다른 프로그램이 사용 중", L"In use by another program") : L"",
                         {HotKeyField(which)});
    };
    auto step = NumberBox(g_prefs.keyStep, 1, 50, L"%", [](int v) {
        g_prefs.keyStep = v;
        SavePrefs();
    });
    page.Children().Append(PanelOf({keyRow(kKeyUp, T(L"밝게", L"Brighter")), keyRow(kKeyDown, T(L"어둡게", L"Dimmer")),
                                     OptionRow(nullptr, T(L"한 번에", L"Per press"), L"", {step})}));

    page.Children().Append(SectionHead(T(L"유휴 시 어둡게", L"Dim when idle")));
    auto idleSwitch = MakeSwitch(g_prefs.idleOn, [](bool on) {
        g_prefs.idleOn = on;
        SavePrefs();
        SetIdleInterval();
    });
    auto minutes = NumberBox(g_prefs.idleMinutes, 1, 120, T(L"분", L"min"), [](int v) {
        g_prefs.idleMinutes = v;
        SavePrefs();
    });
    auto level = NumberBox(g_prefs.idleLevel, 0, 100, L"%", [](int v) {
        g_prefs.idleLevel = v;
        SavePrefs();
    });
    page.Children().Append(PanelOf({
        OptionRow(nullptr, T(L"쓰기", L"Use it"), T(L"입력이 오면 원래 밝기로 돌아갑니다", L"Any input brings the brightness back"),
                  {idleSwitch}),
        OptionRow(nullptr, T(L"기다리는 시간", L"After"), L"", {minutes}),
        OptionRow(nullptr, T(L"어둡게 할 밝기", L"Dim to"), L"", {level}),
        OptionRow(nullptr, L"",
                  T(L"동영상처럼 화면을 켜 두는 프로그램이 있으면 어둡게 하지 않습니다.",
                    L"Nothing dims while a program keeps the screen on, such as a video."),
                  {}),
    }));
    return Scrolled(page, 520);
}

// ---- the frame ---------------------------------------------------------------------

// The top line (32): the title and three icon buttons, or a way back and the page's name.
wux::UIElement BuildHead(Page page) {
    const Palette& c = *g_panel->c;
    if (page == Page::kMain) {
        auto row = Columns({-1, 0, 0, 0}, 4);
        row.Height(32);
        auto title = Text(T(L"밝기", L"Brightness"), 13, c.fg, 600);
        title.Margin({6, 0, 0, 0});
        row.Children().Append(title);
        auto link = MakeIconToggle(kGlyphLink, g_prefs.link, T(L"모두 함께", L"All together"), [](bool on) {
            g_prefs.link = on;
            SavePrefs();
        });
        row.Children().Append(InColumn(link, 1));
        auto screenOff = MakeButton(Kind::kGhost, kGlyphPower, L"", false, [] {
            ClosePanel();
            if (HWND w = g_workerWnd) PostMessageW(w, kMsgScreenOff, 0, 0);
        });
        SetTip(screenOff.root, T(L"화면 끄기", L"Turn the screens off"));
        row.Children().Append(InColumn(screenOff.root, 2));
        auto gear = MakeButton(Kind::kGhost, kGlyphSettings, L"", false, [] { ShowPage(Page::kSettings); });
        SetTip(gear.root, T(L"설정", L"Settings"));
        row.Children().Append(InColumn(gear.root, 3));
        for (uint32_t i = 1; i < row.Children().Size(); i++)
            row.Children().GetAt(i).as<wux::FrameworkElement>().VerticalAlignment(wux::VerticalAlignment::Center);
        return row;
    }
    auto row = Columns({0, -1}, 4);
    row.Height(32);
    auto back = MakeButton(Kind::kGhost, kGlyphBack, L"", false, [] {
        if (g_recording) {
            g_recording = 0;
            RegisterKeys();
        }
        ShowPage(Page::kMain);
    });
    back.root.VerticalAlignment(wux::VerticalAlignment::Center);
    row.Children().Append(back.root);
    row.Children().Append(InColumn(Text(T(L"밝기 설정", L"Brightness settings"), 13, c.fg, 600), 1));
    return row;
}

void ShowPage(Page page) {
    if (!g_panel) return;
    auto& p = *g_panel;
    g_c = p.c;
    p.page = page;
    ReleaseHandlers();
    wux::UIElement content = page == Page::kMain ? BuildMain() : BuildSettings();
    p.headHost.Child(BuildHead(page));
    p.pageHost.Child(content);
}

// A popup window draws the desktop behind it only once DWM lets it.
void AllowHostBackdrop() {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND window, LPARAM) -> BOOL {
            wchar_t name[32];
            if (GetClassNameW(window, name, ARRAYSIZE(name)) && wcscmp(name, L"Xaml_WindowedPopupClass") == 0) {
                BOOL allow = TRUE;
                DwmSetWindowAttribute(window, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &allow, sizeof(allow));
            }
            return TRUE;
        },
        0);
}

// The flyout's own presenter shows nothing: the bubble inside draws the glass.
wux::Style BarePresenter() {
    wux::Style style{winrt::xaml_typename<wuxc::FlyoutPresenter>()};
    auto setters = style.Setters();
    auto set = [&](const wux::DependencyProperty& property, const wf::IInspectable& value) {
        setters.Append(wux::Setter(property, value));
    };
    set(wuxc::Control::PaddingProperty(), box_value(wux::Thickness{}));
    set(wuxc::Control::BackgroundProperty(), Brush(0));
    set(wuxc::Control::BorderThicknessProperty(), box_value(wux::Thickness{}));
    set(wuxc::Control::CornerRadiusProperty(), box_value(wux::CornerRadius{}));
    set(wuxc::FlyoutPresenter::IsDefaultShadowEnabledProperty(), box_value(false));
    set(wux::FrameworkElement::MinWidthProperty(), box_value(0.0));
    set(wux::FrameworkElement::MinHeightProperty(), box_value(0.0));
    set(wux::FrameworkElement::MaxWidthProperty(), box_value(10000.0));
    set(wux::FrameworkElement::MaxHeightProperty(), box_value(10000.0));
    return style;
}

void ClosePanel() {
    if (g_recording) {
        g_recording = 0;
        RegisterKeys();
    }
    if (!g_panel) return;
    auto panel = std::move(g_panel);
    g_panelClosedAt = GetTickCount64();
    ReleaseHandlers();
    try {
        if (panel->flyout) panel->flyout.Hide();
    } catch (...) {
    }
}

void TogglePanel(Widget& w) {
    if (g_panel) {
        ClosePanel();
        return;
    }
    if (GetTickCount64() - g_panelClosedAt < 300) return;  // the press that just closed it
    CloseOtherFlyouts();
    g_panel = std::make_unique<PanelState>();
    auto& p = *g_panel;
    p.taskbar = w.taskbar;
    p.c = &PaletteFor(w.root);
    g_c = p.c;
    const Palette& c = *p.c;

    // Width 348, padding 10, radius 12, a w8 edge.
    p.bubble = wuxc::Border();
    p.bubble.Width(348);
    p.bubble.Padding({10, 10, 10, 10});
    p.bubble.CornerRadius({12, 12, 12, 12});
    p.bubble.Background(GlassBrush(c));
    p.bubble.BorderThickness({1, 1, 1, 1});
    p.bubble.BorderBrush(Brush(Ink(c, 8)));
    p.bubble.RequestedTheme(&c == &kLight ? wux::ElementTheme::Light : wux::ElementTheme::Dark);
    auto frame = VStack(0);
    p.headHost = wuxc::Border();
    p.pageHost = wuxc::Border();
    frame.Children().Append(p.headHost);
    frame.Children().Append(p.pageHost);
    p.bubble.Child(frame);

    p.flyout = wuxc::Flyout();
    p.flyout.FlyoutPresenterStyle(BarePresenter());
    p.flyout.Content(p.bubble);
    p.flyout.ShouldConstrainToRootBounds(false);
    p.flyout.ShowMode(wuxc::Primitives::FlyoutShowMode::Standard);
    p.flyout.Opened([](auto&&, auto&&) { AllowHostBackdrop(); });
    p.flyout.Closed([](wf::IInspectable const& sender, auto&&) {
        if (g_panel && g_panel->flyout == sender.try_as<wuxc::Flyout>()) {
            g_panel.reset();
            g_panelClosedAt = GetTickCount64();
            if (g_recording) {
                g_recording = 0;
                RegisterKeys();
            }
            ReleaseHandlers();
        }
    });
    ShowPage(Page::kMain);
    ShowFlyout(w, p.flyout);
    RequestScan();  // the list and the numbers as the monitors have them now
}

// ============================================================================
// Monitors arriving from the worker; the tick that keeps the widget in place
// ============================================================================

void OnMonitors(std::vector<MonView> list, uint64_t readAt) {
    std::vector<Mon> next;
    for (auto& v : list) {
        Mon m;
        m.id = v.id;
        m.name = v.name;
        m.internal = v.internal;
        m.ok = v.ok;
        m.contrastOk = v.contrastOk;
        m.hw = v.hw;
        m.contrast = v.contrast;
        auto old = std::ranges::find_if(g_mons, [&](auto& o) { return o.id == v.id; });
        // A change made here after the read started is newer than what was read.
        if (old != g_mons.end() && old->changedAt > readAt) {
            m.hw = old->hw;
            m.contrast = old->contrast;
            m.changedAt = old->changedAt;
        }
        m.value = FromHw(m, m.hw);
        next.push_back(std::move(m));
    }
    g_mons = std::move(next);
    g_scanned = true;
    Wh_Log(L"%d monitors", int(g_mons.size()));
    PaintWidgets(true);
    RefreshMain();
}

winrt::Windows::System::DispatcherQueueTimer g_timer{nullptr};

void Tick() {
    if (g_unloading) return;
    PlaceWidgets();
}

void Start() {
    g_queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    if (!g_queue) {
        Wh_Log(L"no dispatcher queue on the taskbar thread");
        return;
    }
    LoadPrefs();
    g_look = ReadLook();
    CreateFlyoutWindow();
    PlaceWidgets();
    g_timer = g_queue.CreateTimer();
    g_timer.Interval(std::chrono::seconds(5));
    g_timer.Tick([](auto&&, auto&&) { Tick(); });
    g_timer.Start();
    g_idleTimer = g_queue.CreateTimer();
    g_idleTimer.Tick([](auto&&, auto&&) {
        if (!g_unloading) IdleTick();
    });
    SetIdleInterval();
}

void Stop() {
    for (auto* timer : {&g_timer, &g_idleTimer}) {
        if (*timer) {
            timer->Stop();
            *timer = nullptr;
        }
    }
    ClosePanel();
    for (auto& w : g_widgets) RemoveWidget(*w);
    g_widgets.clear();
    DestroyFlyoutWindow();
    g_queue = nullptr;
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L">");
    if (!HookTaskbarDll()) {
        Wh_Log(L"taskbar symbols missing");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");
    RunOn(PrimaryTaskbar(), Start);
    StartWorker();
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");
    g_unloading = true;
    RunOn(PrimaryTaskbar(), Stop);
    if (g_subclassed) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(g_subclassed, TaskbarSubclass);
        g_subclassed = nullptr;
    }
}

void Wh_ModUninit() {
    Wh_Log(L">");
    StopWorker();
    for (int i = 0; g_pending > 0 && i < 100; i++) Sleep(10);
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    *bReload = FALSE;
    Look look = ReadLook();
    RunOn(PrimaryTaskbar(), [look] {
        g_look = look;
        PaintWidgets(true);
        ClosePanel();  // the window opens again in the new look
    });
    return TRUE;
}
