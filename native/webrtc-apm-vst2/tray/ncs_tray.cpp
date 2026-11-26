// SPDX-License-Identifier: MIT
//
// Noise Cancelling Stack tray icon. Runs unelevated in the user's session and gives the user three
// controls: turn filtering on or off, start the icon with Windows, and close the icon.
//
// Filtering is machine-wide and needs administrator rights to change, so the icon never changes it
// itself: it writes HKLM\SOFTWARE\NoiseCancellingStack\UserControl\FilteringEnabled, a value setup
// makes writable by standard users, and the SYSTEM watcher applies it. The watcher records the
// state it has applied in HKLM\SOFTWARE\NoiseCancellingStack\AppliedFilteringEnabled, which the
// icon shows. Closing the icon changes nothing about filtering.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"NoiseCancellingStackTray";
constexpr wchar_t kSettingsKey[] = L"SOFTWARE\\NoiseCancellingStack";
constexpr wchar_t kUserControlKey[] = L"SOFTWARE\\NoiseCancellingStack\\UserControl";
constexpr wchar_t kFilteringValue[] = L"FilteringEnabled";
constexpr wchar_t kAppliedValue[] = L"AppliedFilteringEnabled";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Noise Cancelling Stack";
constexpr wchar_t kUserSettingsKey[] = L"Software\\NoiseCancellingStack";
constexpr wchar_t kStartWithWindowsValue[] = L"StartWithWindows";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kIconId = 1;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshMilliseconds = 1500;

enum Command : UINT { kToggleFiltering = 100, kToggleStartup, kClose };
enum class Look { kOn, kOff, kPending, kMissing };

struct Status {
  bool installed = false;  // the control key exists (setup has run)
  bool desired = true;     // what the user asked for
  bool applied = true;     // what the watcher has applied
};

UINT g_taskbarCreated = 0;
HICON g_icons[4] = {};
const wchar_t* g_shownTooltip = nullptr;
bool g_iconAdded = false;

DWORD ReadDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD fallback) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  return RegGetValueW(root, key, name, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &value, &size) == ERROR_SUCCESS
      ? value
      : fallback;
}

Status ReadStatus() {
  Status status;
  HKEY key = nullptr;
  status.installed = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUserControlKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key)
      == ERROR_SUCCESS;
  if (key != nullptr) RegCloseKey(key);
  status.desired = ReadDword(HKEY_LOCAL_MACHINE, kUserControlKey, kFilteringValue, 1) != 0;
  status.applied = ReadDword(HKEY_LOCAL_MACHINE, kSettingsKey, kAppliedValue, 1) != 0;
  return status;
}

Look LookFor(const Status& status) {
  if (!status.installed) return Look::kMissing;
  if (status.desired != status.applied) return Look::kPending;
  return status.applied ? Look::kOn : Look::kOff;
}

const wchar_t* TooltipFor(const Status& status) {
  switch (LookFor(status)) {
    case Look::kOn: return L"Noise Cancelling Stack: microphones filtered";
    case Look::kOff: return L"Noise Cancelling Stack: filtering off (microphones unprocessed)";
    case Look::kPending:
      return status.desired ? L"Noise Cancelling Stack: turning filtering on..."
                            : L"Noise Cancelling Stack: turning filtering off...";
    case Look::kMissing: break;
  }
  return L"Noise Cancelling Stack is not installed; run setup or Repair all microphones";
}

std::wstring ExecutablePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length < path.size()) {
      path.resize(length);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

std::wstring RunCommand() { return L"\"" + ExecutablePath() + L"\""; }

bool StartsWithWindows() {
  wchar_t value[1024] = {};
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
    return false;
  }
  return _wcsicmp(value, RunCommand().c_str()) == 0;
}

void SetStartsWithWindows(bool enabled, bool rememberChoice) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
    if (enabled) {
      const std::wstring command = RunCommand();
      RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                     static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
      RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
  }
  if (rememberChoice) {
    const DWORD choice = enabled ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kUserSettingsKey, kStartWithWindowsValue, REG_DWORD, &choice, sizeof(choice));
  }
}

bool RequestFiltering(HWND window, bool enabled) {
  HKEY key = nullptr;
  LSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUserControlKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
  if (result == ERROR_SUCCESS) {
    const DWORD value = enabled ? 1 : 0;
    result = RegSetValueExW(key, kFilteringValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
  }
  if (result != ERROR_SUCCESS) {
    wchar_t message[256];
    swprintf_s(message, L"Could not change the filtering setting (error %ld).\n\n"
                        L"Run \"Repair all microphones\" from the Start menu, then try again.", static_cast<long>(result));
    MessageBoxW(window, message, L"Noise Cancelling Stack", MB_OK | MB_ICONWARNING);
    return false;
  }
  return true;
}

// --- Icon: a white microphone on a coloured disc, rasterized analytically so no image files ship.

float Clamp01(float value) { return std::min(1.0F, std::max(0.0F, value)); }

float SegmentDistance(float x, float y, float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  const float t = Clamp01(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy));
  return std::hypot(x - (ax + t * dx), y - (ay + t * dy));
}

HICON MakeIcon(int size, COLORREF disc) {
  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size;  // top-down
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (color == nullptr) return nullptr;

  auto* pixels = static_cast<uint32_t*>(bits);
  const float scale = static_cast<float>(size) / 16.0F;  // geometry is drawn on a 16-unit grid
  const float discR = GetRValue(disc), discG = GetGValue(disc), discB = GetBValue(disc);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float fx = (static_cast<float>(x) + 0.5F) / scale;
      const float fy = (static_cast<float>(y) + 0.5F) / scale;
      const float discCoverage = Clamp01(0.5F - (std::hypot(fx - 8.0F, fy - 8.0F) - 7.5F) * scale);
      // Capsule, U-shaped holder (lower half of a ring), stem, and base.
      float glyph = SegmentDistance(fx, fy, 8.0F, 4.3F, 8.0F, 7.6F) - 2.0F;
      const float ring = fy >= 8.0F
          ? std::fabs(std::hypot(fx - 8.0F, fy - 8.0F) - 3.7F) - 0.55F
          : std::min(std::hypot(fx - 4.3F, fy - 8.0F), std::hypot(fx - 11.7F, fy - 8.0F)) - 0.55F;
      glyph = std::min(glyph, ring);
      glyph = std::min(glyph, SegmentDistance(fx, fy, 8.0F, 11.7F, 8.0F, 13.0F) - 0.55F);
      glyph = std::min(glyph, SegmentDistance(fx, fy, 6.2F, 13.0F, 9.8F, 13.0F) - 0.55F);
      const float glyphCoverage = Clamp01(0.5F - glyph * scale);
      const float r = discR + (255.0F - discR) * glyphCoverage;
      const float g = discG + (255.0F - discG) * glyphCoverage;
      const float b = discB + (255.0F - discB) * glyphCoverage;
      const float alpha = std::max(discCoverage, glyphCoverage);
      pixels[y * size + x] = (static_cast<uint32_t>(alpha * 255.0F + 0.5F) << 24)
          | (static_cast<uint32_t>(r + 0.5F) << 16) | (static_cast<uint32_t>(g + 0.5F) << 8)
          | static_cast<uint32_t>(b + 0.5F);
    }
  }

  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO info = {};
  info.fIcon = TRUE;
  info.hbmMask = mask;
  info.hbmColor = color;
  HICON icon = CreateIconIndirect(&info);
  DeleteObject(mask);
  DeleteObject(color);
  return icon;
}

void CreateIcons() {
  const int size = GetSystemMetrics(SM_CXSMICON);
  g_icons[static_cast<int>(Look::kOn)] = MakeIcon(size, RGB(0x1E, 0x8E, 0x3E));
  g_icons[static_cast<int>(Look::kOff)] = MakeIcon(size, RGB(0x6B, 0x6B, 0x6B));
  g_icons[static_cast<int>(Look::kPending)] = MakeIcon(size, RGB(0xC7, 0x7C, 0x02));
  g_icons[static_cast<int>(Look::kMissing)] = MakeIcon(size, RGB(0xB3, 0x26, 0x1E));
}

// Adds the icon, or updates it when the state changed. Cheap enough to call on a timer: it only
// reads two registry values unless something changed.
void UpdateIcon(HWND window, bool force) {
  const Status status = ReadStatus();
  const wchar_t* tooltip = TooltipFor(status);  // one string per distinct state
  if (!force && g_iconAdded && tooltip == g_shownTooltip) return;
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = window;
  data.uID = kIconId;
  data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
  data.uCallbackMessage = kTrayMessage;
  data.hIcon = g_icons[static_cast<int>(LookFor(status))];
  wcsncpy_s(data.szTip, tooltip, _TRUNCATE);
  if (g_iconAdded && Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE) g_iconAdded = false;
  if (!g_iconAdded) g_iconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
  g_shownTooltip = tooltip;
}

void ShowMenu(HWND window) {
  const Status status = ReadStatus();
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) return;
  AppendMenuW(menu, MF_STRING | (status.desired ? MF_CHECKED : 0) | (status.installed ? 0 : MF_GRAYED),
              kToggleFiltering, L"Filter microphones");
  AppendMenuW(menu, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), kToggleStartup, L"Start with Windows");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kClose, L"Close");
  POINT cursor;
  GetCursorPos(&cursor);
  // Required so the menu closes when the user clicks elsewhere (documented TrackPopupMenu behaviour).
  SetForegroundWindow(window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, window, nullptr);
  PostMessageW(window, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == g_taskbarCreated && g_taskbarCreated != 0) {  // Explorer restarted
    g_iconAdded = false;
    UpdateIcon(window, true);
    return 0;
  }
  switch (message) {
    case WM_CREATE:
      UpdateIcon(window, true);
      SetTimer(window, kRefreshTimer, kRefreshMilliseconds, nullptr);
      return 0;
    case WM_TIMER:
      UpdateIcon(window, false);
      return 0;
    case kTrayMessage:
      if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
        ShowMenu(window);
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case kToggleFiltering:
          if (RequestFiltering(window, !ReadStatus().desired)) UpdateIcon(window, false);
          return 0;
        case kToggleStartup:
          SetStartsWithWindows(!StartsWithWindows(), true);
          return 0;
        case kClose:
          DestroyWindow(window);
          return 0;
        default:
          break;
      }
      break;
    case WM_DESTROY: {
      KillTimer(window, kRefreshTimer);
      NOTIFYICONDATAW data = {};
      data.cbSize = sizeof(data);
      data.hWnd = window;
      data.uID = kIconId;
      Shell_NotifyIconW(NIM_DELETE, &data);
      PostQuitMessage(0);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

void EnablePerMonitorDpiAwareness() {
  using SetContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) return;
  const auto set = reinterpret_cast<SetContext>(
      reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
  if (set != nullptr) set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
  // "--install" (from setup, as the signed-in user): start with Windows unless the user turned it off.
  if (commandLine != nullptr && wcsstr(commandLine, L"--install") != nullptr
      && ReadDword(HKEY_CURRENT_USER, kUserSettingsKey, kStartWithWindowsValue, 1) != 0) {
    SetStartsWithWindows(true, false);
  }

  HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\NoiseCancellingStack.Tray");
  if (single == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;  // already running in this session
  }

  EnablePerMonitorDpiAwareness();
  CreateIcons();
  g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  WNDCLASSEXW windowClass = {};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = kWindowClass;
  if (RegisterClassExW(&windowClass) == 0) return 1;
  // A hidden top-level window (not message-only) so Explorer's TaskbarCreated broadcast reaches it.
  HWND window = CreateWindowExW(0, kWindowClass, L"Noise Cancelling Stack", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) return 1;

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  for (HICON icon : g_icons) {
    if (icon != nullptr) DestroyIcon(icon);
  }
  CloseHandle(single);
  return 0;
}
