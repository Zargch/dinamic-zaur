#include "common.h"
#include <appmodel.h>

// Ventanas abiertas: se usan para llevar al frente la app que esta sonando.

static bool EndsWith(const std::wstring& s, const wchar_t* suf) {
  size_t n = wcslen(suf);
  return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

std::wstring ExeOfWindow(HWND h) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return L"";
  wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
  std::wstring r;
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) r = Lower(buf);
  CloseHandle(p);
  return r;
}

// apps empaquetadas (Store) a veces no publican el ID en la ventana; el proceso si lo conoce
static std::wstring ProcessAumid(HWND h) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return L"";
  wchar_t buf[512]; UINT32 n = 512;
  std::wstring r;
  if (GetApplicationUserModelId(p, &n, buf) == ERROR_SUCCESS) r = Lower(buf);
  CloseHandle(p);
  return r;
}

std::wstring AumidOfWindow(HWND h) {
  IPropertyStore* ps = nullptr;
  std::wstring r;
  if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) {
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR) r = Lower(pv.pwszVal);
    PropVariantClear(&pv);
    ps->Release();
  }
  if (r.empty()) r = ProcessAumid(h);
  return r;
}

static bool IsAppWindow(HWND h) {
  if (!IsWindowVisible(h) || IsOwnWindow(h)) return false;
  LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
  if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
  if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
  if (ex & WS_EX_NOACTIVATE && !(ex & WS_EX_APPWINDOW)) return false;
  int cloaked = 0;
  DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
  if (cloaked) return false;
  if (GetWindowTextLengthW(h) == 0) return false;
  wchar_t cls[64];
  GetClassNameW(h, cls, 64);
  for (auto c : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow" })
    if (!wcscmp(cls, c)) return false;
  return true;
}

std::vector<RunningWin> EnumRunning() {
  std::vector<RunningWin> out;
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    if (!IsAppWindow(h)) return TRUE;
    RunningWin r{ h };
    r.exe = ExeOfWindow(h);
    r.aumid = AumidOfWindow(h);
    if (r.aumid.empty() && EndsWith(r.exe, L"\\explorer.exe")) r.aumid = L"microsoft.windows.explorer";
    r.key = r.aumid.empty() ? r.exe : r.aumid;
    wchar_t t[256];
    GetWindowTextW(h, t, 256);
    r.title = t;
    ((std::vector<RunningWin>*)lp)->push_back(std::move(r));
    return TRUE;
  }, (LPARAM)&out);
  return out;
}

void ActivateWindow(HWND h) {
  if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
  if (!SetForegroundWindow(h)) SwitchToThisWindow(h, TRUE);
}

// nombre visible de una app (Spotify, Google Chrome, una PWA...) a partir de su AppUserModelID
std::wstring AppDisplayName(const std::wstring& aumid) {
  static std::map<std::wstring, std::wstring> cache;
  static std::mutex mx;
  {
    std::lock_guard<std::mutex> lk(mx);
    auto it = cache.find(aumid);
    if (it != cache.end()) return it->second;
  }
  std::wstring name;
  IShellItem* it = nullptr;
  if (SUCCEEDED(SHCreateItemFromParsingName((L"shell:AppsFolder\\" + aumid).c_str(), nullptr, IID_PPV_ARGS(&it)))) {
    PWSTR n = nullptr;
    if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &n)) && n) { name = n; CoTaskMemFree(n); }
    it->Release();
  }
  if (name.empty()) {
    name = aumid;
    if (EndsWith(name, L".exe")) name = name.substr(0, name.size() - 4);
    size_t s = name.find_last_of(L"\\!");
    if (s != std::wstring::npos) name = name.substr(s + 1);
    if (!name.empty()) name[0] = towupper(name[0]);
  }
  std::lock_guard<std::mutex> lk(mx);
  return cache[aumid] = name;
}
