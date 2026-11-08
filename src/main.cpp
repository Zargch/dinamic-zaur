#include "common.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "winhttp.lib")

HWND g_fg;

static UINT s_taskbarCreatedMsg;
static NOTIFYICONDATAW s_nid;
static bool s_fullscreen = false;
static RECT s_layoutMon{};
enum { HK_QUIT = 1 };

static void Cleanup() {
  static bool done = false;
  if (done) return;
  done = true;
  CloseMenu();
  Shell_NotifyIconW(NIM_DELETE, &s_nid);
}

static std::wstring LocalDir() {
  wchar_t p[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", p, MAX_PATH)) return g_dataDir;
  std::wstring d = std::wstring(p) + L"\\DinamicZaur";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
  // deja rastro en %LOCALAPPDATA%\DinamicZaur\crash.log y se vuelve a abrir (salvo si fallo hace < 1 min)
  bool relaunch = true;
  std::wstring f = LocalDir() + L"\\crash.log";
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa)) {
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULONGLONG t0 = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    ULONGLONG t1 = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    if (t1 > t0 && t1 - t0 < 60ULL * 10000000ULL) relaunch = false;
  }
  HANDLE h = CreateFileW(f.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    SYSTEMTIME st; GetLocalTime(&st);
    char line[256];
    ULONG_PTR addr = ep ? (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress : 0, base = (ULONG_PTR)GetModuleHandleW(nullptr);
    int n = sprintf_s(line, "%04d-%02d-%02d %02d:%02d:%02d  v%ls  code=0x%08lX  exe+0x%llX\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                      st.wSecond, DZ_VERSION, ep ? ep->ExceptionRecord->ExceptionCode : 0, (unsigned long long)(addr - base));
    DWORD w; WriteFile(h, line, n, &w, nullptr);
    CloseHandle(h);
  }
  Cleanup();
  if (relaunch) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    wchar_t cmd[MAX_PATH + 32];
    swprintf_s(cmd, L"\"%s\" --restart", exe);
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

// DPI por monitor si el sistema lo permite (Windows 10 1703+); si no, DPI del sistema
static void SetDpiAwareness() {
  typedef BOOL(WINAPI * Fn)(DPI_AWARENESS_CONTEXT);
  if (auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
    if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
  SetProcessDPIAware();
}

static UINT SystemDpi() {
  typedef UINT(WINAPI * Fn)();
  if (auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem")) return fn();
  HDC dc = GetDC(nullptr);
  UINT d = GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(nullptr, dc);
  return d;
}

static void InitCore() {
  SetDpiAwareness();
  SetUnhandledExceptionFilter(CrashHandler);
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  g_dir = exe;
  g_dir = g_dir.substr(0, g_dir.find_last_of(L'\\'));
  // configuracion por usuario (la app puede estar en Archivos de programa, sin permiso de escritura)
  std::wstring ad = KnownPath(FOLDERID_RoamingAppData);
  g_dataDir = (ad.empty() ? g_dir : ad) + L"\\DinamicZaur";
  CreateDirectoryW(g_dataDir.c_str(), nullptr);
  g_es = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
  // °F por defecto donde se usa el sistema de medidas de EE. UU.
  wchar_t ms[4] = {};
  if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, ms, 4) && ms[0] == L'1') g_cfg.fahrenheit = true;
  OleInitialize(nullptr);  // STA + arrastrar y soltar
  g_scale = SystemDpi() / 96.f;
  ID2D1Factory1* f1 = nullptr;
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f1);
  g_d2d = f1;
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
  // render por software: no carga el driver de la GPU en este proceso
  auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * g_scale, 96 * g_scale);
  g_d2d->CreateDCRenderTarget(&props, &g_rt);
  g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  g_rt->QueryInterface(&g_dc);
  LoadConfig();
  // Windows 10 no trae "Segoe UI Variable": se usa Segoe UI
  if (!IsWin11()) {
    if (g_cfg.font == L"Segoe UI Variable Text") g_cfg.font = L"Segoe UI";
    if (g_cfg.fontDisplay == L"Segoe UI Variable Display") g_cfg.fontDisplay = L"Segoe UI";
  }
  LoadTheme();
}

// ================================================================ icono de bandeja
HICON App_Icon(int px) {
  BITMAPINFO bi{};
  bi.bmiHeader = { sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB };
  void* bits;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(dc, color);
  RECT rc{ 0, 0, px, px };
  g_rt->BindDC(dc, &rc);
  g_rt->BeginDraw();
  g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
  float s = Dip(px);
  DrawAppLogo(g_rt, { 0, 0, s, s });
  g_rt->EndDraw();
  SelectObject(dc, old);
  DeleteDC(dc);
  HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
  ICONINFO ii{ TRUE, 0, 0, mask, color };
  HICON ic = CreateIconIndirect(&ii);
  DeleteObject(mask); DeleteObject(color);
  return ic;
}

static void AddTray() {
  s_nid = { sizeof s_nid };
  s_nid.hWnd = g_msgWnd;
  s_nid.uID = 1;
  s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  s_nid.uCallbackMessage = WM_APP_TRAY;
  s_nid.hIcon = App_Icon(GetSystemMetrics(SM_CXSMICON));
  wcscpy_s(s_nid.szTip, DZ_NAME);
  Shell_NotifyIconW(NIM_ADD, &s_nid);
}

static void Restart() {
  Cleanup();
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  ShellExecuteW(nullptr, L"open", exe, L"--restart", g_dir.c_str(), SW_SHOWNORMAL);
  ExitProcess(0);
}

static void Quit() { Cleanup(); PostQuitMessage(0); }

static void TrayMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, 1, T(L"Configuración…", L"Settings…"));
  AppendMenuW(m, MF_STRING | (Startup_Enabled() ? MF_CHECKED : 0), 2, T(L"Iniciar con Windows", L"Start with Windows"));
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 3, T(L"Reiniciar", L"Restart"));
  AppendMenuW(m, MF_STRING, 4, T(L"Salir", L"Quit"));
  SetMenuDefaultItem(m, 1, FALSE);
  POINT p; GetCursorPos(&p);
  SetForegroundWindow(g_msgWnd);
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_msgWnd, nullptr);
  DestroyMenu(m);
  switch (cmd) {
  case 1: Settings_Open(false); break;
  case 2: Startup_Set(!Startup_Enabled()); break;
  case 3: Restart(); break;
  case 4: Quit(); break;
  }
}

// ================================================================ foco y pantalla completa
// pantalla completa "de verdad" (juegos, videos en F11, presentaciones). Una ventana maximizada
// normal no cuenta, aunque ocupe toda la pantalla.
static bool IsFullscreen(HWND h) {
  QUERY_USER_NOTIFICATION_STATE qs;
  if (SUCCEEDED(SHQueryUserNotificationState(&qs)) && (qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE)) return true;
  if (!h || !IsWindowVisible(h) || IsOwnWindow(h)) return false;
  wchar_t cls[64];
  GetClassNameW(h, cls, 64);
  if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return false;
  HMONITOR hm = MonitorFromWindow(h, MONITOR_DEFAULTTONULL);
  MONITORINFO mi{ sizeof mi };
  if (!hm || !GetMonitorInfoW(hm, &mi)) return false;
  RECT r, mon = mi.rcMonitor, prim = MonitorRect();
  if (!EqualRect(&mon, &prim)) return false;  // la isla vive en el monitor principal
  GetWindowRect(h, &r);
  bool covers = r.left <= mon.left && r.top <= mon.top && r.right >= mon.right && r.bottom >= mon.bottom;
  LONG st = GetWindowLongW(h, GWL_STYLE);
  if (covers && (st & WS_CAPTION) != WS_CAPTION && !(st & WS_THICKFRAME)) return true;
  return covers && !IsZoomed(h);
}

// Con una app en uso la isla se esconde (si asi se configuro) y reaparece al llevar el raton al
// borde superior o un momento para avisos. Con el escritorio activo se ve siempre. En pantalla completa, nunca.
static bool s_appActive = false, s_revealed = false;

void TopBars_Refresh() {
  bool autoh = g_cfg.autohide;
  bool show = !s_fullscreen && (!autoh || !s_appActive || s_revealed);
  Island_Show(show || (!s_fullscreen && Island_Busy()));
  if (autoh && s_appActive && !s_fullscreen) SetTimer(g_msgWnd, 7, 120, nullptr);
  else KillTimer(g_msgWnd, 7);
}

static void TopBarsTick() {
  POINT p; GetCursorPos(&p);
  RECT mon = MonitorRect();
  static DWORD edgeSince = 0;
  if (!s_revealed) {
    // hay que dejar el raton un momento arriba: asi no salta al ir a las pestañas de las ventanas
    if (p.y <= mon.top + 1 && p.x >= mon.left && p.x < mon.right) {
      if (!edgeSince) edgeSince = GetTickCount();
      else if (GetTickCount() - edgeSince >= 300) { s_revealed = true; edgeSince = 0; }
    } else edgeSince = 0;
  } else if (p.y > mon.top + Px(48) && !MenuOpen() && !Island_Busy()) s_revealed = false;
  TopBars_Refresh();
}

static void CheckFullscreen(HWND h) {
  bool fs = g_cfg.hideOnFullscreen && h && IsFullscreen(h);
  if (fs != s_fullscreen) { s_fullscreen = fs; TopBars_Refresh(); }
}

static void OnForegroundChanged(HWND h) {
  if (!h || IsOwnWindow(h)) return;
  wchar_t cls[64];
  GetClassNameW(h, cls, 64);
  if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcscmp(cls, L"Windows.UI.Core.CoreWindow") ||
      !wcscmp(cls, L"XamlExplorerHostIslandWindow") || !wcscmp(cls, L"NotifyIconOverflowWindow") || !wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland"))
    return;
  g_fg = h;
  s_appActive = wcscmp(cls, L"Progman") && wcscmp(cls, L"WorkerW");  // el escritorio no cuenta como app
  s_fullscreen = g_cfg.hideOnFullscreen && IsFullscreen(h);
  TopBars_Refresh();
}

void App_SettingsChanged() {
  if (g_fg) CheckFullscreen(g_fg);
  s_revealed = false;
  TopBars_Refresh();
  Island_Invalidate();
}

static void CALLBACK WinEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG, DWORD, DWORD) {
  if (obj != OBJID_WINDOW) return;
  if (ev == EVENT_SYSTEM_FOREGROUND) OnForegroundChanged(h);
  else if (ev == EVENT_OBJECT_LOCATIONCHANGE && h == g_fg) CheckFullscreen(h);  // entrar/salir de F11, juegos
}

// ================================================================ ventana de mensajes
static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == s_taskbarCreatedMsg && s_taskbarCreatedMsg) { AddTray(); return 0; }  // el Explorador se reinicio
  switch (m) {
  case WM_APP_RENDER: Menu_OnMessage(wp); return 0;
  case WM_APP_ICONREADY: SetTimer(h, 3, 30, nullptr); return 0;  // agrupa varios iconos en un solo repintado
  case WM_APP_WEATHER: Island_Invalidate(); return 0;
  case WM_APP_UPDATE: Island_OnUpdate(); return 0;
  case WM_APP_MEDIA: {
    Island_OnMedia();
    static bool trimmed = false;  // tras la primera lectura ya no hacen falta los datos de arranque de WinRT
    if (!trimmed) { trimmed = true; TrimMemory(); }
    return 0;
  }
  case WM_APP_VOLUME: Island_OnVolume((int)wp, lp != 0); return 0;
  case WM_APP_TRAY:
    if (LOWORD(lp) == WM_LBUTTONDBLCLK) Settings_Open(false);
    else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) TrayMenu();
    return 0;
  case WM_HOTKEY:
    if (wp == HK_QUIT) Quit();
    return 0;
  case WM_TIMER:
    if (wp == 1) {  // por si algo paso a pantalla completa sin cambiar de ventana
      HWND fg = GetForegroundWindow();
      if (fg && fg == g_fg) CheckFullscreen(fg);
    } else if (wp == 3) { KillTimer(h, 3); Island_Invalidate(); }
    else if (wp == 7) TopBarsTick();
    else if (wp == 9) {  // cambio de pantalla pendiente (ver WM_DISPLAYCHANGE)
      HWND fg = GetForegroundWindow();
      if (s_fullscreen || (fg && IsFullscreen(fg))) return 0;  // sigue el juego: se mira otra vez en 2 s
      KillTimer(h, 9);
      RECT now = MonitorRect();
      if (SystemDpi() / 96.f != g_scale) Restart();  // otra escala: todo se vuelve a crear
      else if (!EqualRect(&now, &s_layoutMon)) { s_layoutMon = now; Island_Relayout(); }
    }
    return 0;
  case WM_SETTINGCHANGE:
    if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet")) { LoadTheme(); Island_Invalidate(); }
    return 0;
  case WM_DISPLAYCHANGE:
  case WM_DPICHANGED:
    // los juegos a pantalla completa exclusiva cambian la resolucion al entrar y salir:
    // se espera a que no haya nada a pantalla completa y se recoloca solo si la pantalla cambio
    SetTimer(h, 9, 2000, nullptr);
    return 0;
  case WM_CLOSE: Quit(); return 0;
  case WM_ENDSESSION:
    if (wp) Cleanup();
    return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
  g_inst = inst;
  bool restart = wcsstr(cmd, L"--restart") != nullptr;
  if (restart) Sleep(400);
  if (wcsstr(cmd, L"--autostart")) {
    // al iniciar sesion el Explorador puede no estar listo: se espera a la barra de tareas
    for (int i = 0; i < 120 && !FindWindowW(L"Shell_TrayWnd", nullptr); i++) Sleep(500);
    Sleep(1500);
  }
  HANDLE mtx = CreateMutexW(nullptr, TRUE, L"DinamicZaur.SingleInstance");
  for (int i = 0; i < 20 && GetLastError() == ERROR_ALREADY_EXISTS && restart; i++) {  // la instancia anterior aun se esta cerrando
    CloseHandle(mtx);
    Sleep(250);
    mtx = CreateMutexW(nullptr, TRUE, L"DinamicZaur.SingleInstance");
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // ya esta abierta: si el usuario la abrio otra vez, se le muestra la configuracion
    if (HWND other = FindWindowW(L"DinamicZaurMsg", nullptr)) {
      AllowSetForegroundWindow(ASFW_ANY);
      PostMessageW(other, WM_APP_TRAY, 0, WM_LBUTTONDBLCLK);
    }
    return 0;
  }
  InitCore();
  s_layoutMon = MonitorRect();

  WNDCLASSW wc{};
  wc.lpfnWndProc = MsgProc; wc.hInstance = inst; wc.lpszClassName = L"DinamicZaurMsg";
  RegisterClassW(&wc);
  g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, DZ_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
  s_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
  ChangeWindowMessageFilterEx(g_msgWnd, WM_APP_TRAY, MSGFLT_ALLOW, nullptr);
  AddTray();
  RegisterHotKey(g_msgWnd, HK_QUIT, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'Q');  // salida de emergencia

  Island_Create();
  OnForegroundChanged(GetForegroundWindow());
  SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
  SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  SetTimer(g_msgWnd, 1, 3000, nullptr);
  if (!g_cfg.firstRunDone) Settings_Open(true);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    HWND sw = Settings_Hwnd();
    if (sw && IsDialogMessageW(sw, &msg)) continue;  // Tab, Enter y Esc en la configuracion
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  Cleanup();
  ReleaseMutex(mtx);
  return 0;
}
