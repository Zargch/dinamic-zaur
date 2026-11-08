#include "common.h"
#include <winhttp.h>

HINSTANCE g_inst;
ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
IWICImagingFactory* g_wic;
ID2D1DCRenderTarget* g_rt;
ID2D1DeviceContext* g_dc;
float g_scale = 1.f;
HWND g_msgWnd;
std::wstring g_dir, g_dataDir;
bool g_es = true;
Theme g_th;
Config g_cfg;

bool IsWin11() {
  static int v = -1;
  if (v < 0) {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi{ sizeof vi };
    auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    v = fn && fn(&vi) == 0 && vi.dwBuildNumber >= 22000;
  }
  return v == 1;
}

// ================================================================ tema (menus emergentes)
void LoadTheme() {
  DWORD v = 1, sz = sizeof v;
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
  Theme& t = g_th;
  t.dark = v == 0;
  if (t.dark) {
    t.text = RGBA(255, 255, 255, .92f); t.textDim = RGBA(255, 255, 255, .55f);
    t.tint = RGBA(28, 28, 30, .7f); t.rim = RGBA(255, 255, 255, .22f);
    t.accent = RGBA(10, 132, 255); t.hover = RGBA(255, 255, 255, .12f);
    t.sep = RGBA(255, 255, 255, .14f); t.pill = RGBA(44, 44, 46, .92f);
  } else {
    t.text = RGBA(0, 0, 0, .85f); t.textDim = RGBA(0, 0, 0, .5f);
    t.tint = RGBA(255, 255, 255, .62f); t.rim = RGBA(255, 255, 255, .65f);
    t.accent = RGBA(0, 122, 255); t.hover = RGBA(0, 0, 0, .07f);
    t.sep = RGBA(0, 0, 0, .11f); t.pill = RGBA(246, 246, 246, .94f);
  }
}

// ================================================================ configuracion (%APPDATA%\DinamicZaur\config.ini)
static std::wstring IniPath() { return g_dataDir + L"\\config.ini"; }
static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
  wchar_t buf[2048];
  GetPrivateProfileStringW(sec, key, def, buf, 2048, IniPath().c_str());
  return buf;
}
static int IniInt(const wchar_t* sec, const wchar_t* key, int def) { return GetPrivateProfileIntW(sec, key, def, IniPath().c_str()); }
static void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(sec, key, v.c_str(), IniPath().c_str()); }

void LoadConfig() {
  Config& c = g_cfg;
  c.firstRunDone = GetFileAttributesW(IniPath().c_str()) != INVALID_FILE_ATTRIBUTES && IniInt(L"General", L"setup_done", 0);
  c.font = IniStr(L"General", L"font", c.font.c_str());
  c.fontDisplay = IniStr(L"General", L"font_display", c.fontDisplay.c_str());
  c.autohide = IniInt(L"General", L"autohide", c.autohide);
  c.hideOnFullscreen = IniInt(L"General", L"hide_on_fullscreen", c.hideOnFullscreen);
  c.calendar = std::max(0, std::min((int)CAL_NONE, IniInt(L"Calendar", L"app", c.calendar)));
  c.calendarPath = IniStr(L"Calendar", L"path", L"");
  c.fahrenheit = IniInt(L"Weather", L"fahrenheit", c.fahrenheit);
  c.weatherCity = IniStr(L"Weather", L"place", L"");
  std::wstring la = IniStr(L"Weather", L"latitude", L""), lo = IniStr(L"Weather", L"longitude", L"");
  c.hasLoc = !la.empty() && !lo.empty();
  if (c.hasLoc) { c.lat = _wtof(la.c_str()); c.lon = _wtof(lo.c_str()); }
}

void SaveConfig() {
  Config& c = g_cfg;
  CreateDirectoryW(g_dataDir.c_str(), nullptr);
  IniSet(L"General", L"setup_done", c.firstRunDone ? L"1" : L"0");
  IniSet(L"General", L"font", c.font);
  IniSet(L"General", L"font_display", c.fontDisplay);
  IniSet(L"General", L"autohide", std::to_wstring(c.autohide));
  IniSet(L"General", L"hide_on_fullscreen", std::to_wstring(c.hideOnFullscreen));
  IniSet(L"Calendar", L"app", std::to_wstring(c.calendar));
  IniSet(L"Calendar", L"path", c.calendarPath);
  IniSet(L"Weather", L"fahrenheit", std::to_wstring(c.fahrenheit));
  IniSet(L"Weather", L"place", c.weatherCity);
  wchar_t b[32];
  if (c.hasLoc) {
    swprintf_s(b, L"%.4f", c.lat); IniSet(L"Weather", L"latitude", b);
    swprintf_s(b, L"%.4f", c.lon); IniSet(L"Weather", L"longitude", b);
  } else {
    WritePrivateProfileStringW(L"Weather", L"latitude", nullptr, IniPath().c_str());
    WritePrivateProfileStringW(L"Weather", L"longitude", nullptr, IniPath().c_str());
  }
}

// ================================================================ carpetas y archivos
std::wstring KnownPath(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::wstring r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r;
}

std::wstring DownloadsPath() { return KnownPath(FOLDERID_Downloads); }

void OpenFolder(const std::wstring& path) { RunShell(L"explorer.exe", L"\"" + path + L"\""); }
void ShowInFolder(const std::wstring& path) { RunShell(L"explorer.exe", L"/select,\"" + path + L"\""); }

bool ConfirmBox(const wchar_t* text) {
  return MessageBoxW(nullptr, text, DZ_NAME, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
}

// ================================================================ internet (WinHTTP, HTTPS)
std::string HttpGet(const wchar_t* host, const std::wstring& path, DWORD* status) {
  std::string out;
  if (status) *status = 0;
  HINTERNET s = WinHttpOpen(DZ_USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  if (!s) {  // Windows 10 antiguo: sin proxy automatico
    s = WinHttpOpen(DZ_USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!s) return out;
  }
  WinHttpSetTimeouts(s, 8000, 8000, 10000, 15000);
  HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
  if (r && WinHttpSendRequest(r, L"Accept: application/json\r\n", (DWORD)-1L, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
    DWORD code = 0, sz = sizeof code;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
    if (status) *status = code;
    DWORD n;
    while (WinHttpQueryDataAvailable(r, &n) && n) {
      std::string buf(n, 0);
      DWORD got = 0;
      if (!WinHttpReadData(r, buf.data(), n, &got)) break;
      out.append(buf.data(), got);
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return out;
}

std::wstring Utf8(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

std::wstring UrlEnc(const std::wstring& s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string u(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, u.data(), n, nullptr, nullptr);
  u.pop_back();
  std::wstring r;
  for (unsigned char c : u) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.') r += (wchar_t)c;
    else { wchar_t b[4]; swprintf_s(b, L"%%%02X", c); r += b; }
  }
  return r;
}

size_t JFind(const std::string& s, const char* key, size_t from) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = s.find(k, from);
  if (p == std::string::npos) return p;
  p += k.size();
  while (p < s.size() && s[p] == ' ') p++;
  return p;
}

double JNum(const std::string& s, const char* key, size_t from) {
  size_t p = JFind(s, key, from);
  if (p == std::string::npos) return 0;
  if (s[p] == '"') p++;  // Nominatim entrega las coordenadas como texto
  return strtod(s.c_str() + p, nullptr);
}

std::wstring JStr(const std::string& s, const char* key, size_t from) {
  size_t p = JFind(s, key, from);
  if (p == std::string::npos || s[p] != '"') return L"";
  std::string out;
  for (size_t i = p + 1; i < s.size() && s[i] != '"'; i++) {
    if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
    char e = s[++i];
    if (e == 'u' && i + 4 < s.size()) {
      unsigned cp = strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
      i += 4;
      if (cp < 0x80) out += (char)cp;
      else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
      else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    } else if (e == 'n') out += ' ';
    else out += e;
  }
  return Utf8(out);
}

// ================================================================ logo: una isla negra con un punto de luz
void DrawAppLogo(ID2D1RenderTarget* rt, D2D1_RECT_F r, float a) {
  float s = std::min(r.right - r.left, r.bottom - r.top), cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  FillRound(rt, { cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2 }, s * .24f, RGBA(10, 132, 255, a));
  FillRound(rt, { cx - s * .36f, cy - s * .13f, cx + s * .36f, cy + s * .13f }, s * .13f, RGBA(0, 0, 0, a));
  rt->FillEllipse(D2D1::Ellipse({ cx + s * .2f, cy }, s * .055f, s * .055f), Brush(RGBA(48, 209, 88, a)));
}

// ================================================================ utilidades
std::wstring Lower(std::wstring s) {
  CharLowerBuffW(s.data(), (DWORD)s.size());
  return s;
}

bool IContains(const std::wstring& hay, const std::wstring& needle) {
  if (needle.empty()) return true;
  return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                         hay.c_str(), (int)hay.size(), needle.c_str(), (int)needle.size(), nullptr, nullptr, nullptr, 0) >= 0;
}

RECT MonitorRect() {
  MONITORINFO mi{ sizeof mi };
  GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
  return mi.rcMonitor;
}

void RunShell(const std::wstring& target, const std::wstring& args, const wchar_t* verb) {
  ShellExecuteW(nullptr, verb, target.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
}

void SendKeys(std::initializer_list<WORD> keys) {
  std::vector<INPUT> in;
  for (WORD k : keys) { INPUT i{ INPUT_KEYBOARD }; i.ki.wVk = k; in.push_back(i); }
  for (auto it = keys.end(); it != keys.begin();) {
    --it; INPUT i{ INPUT_KEYBOARD }; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
  }
  SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

void TrimMemory() {
  // el render target compartido conserva un bufer del tamaño de la ultima superficie dibujada
  // (una pantalla completa son ~15 MB): lo encogemos atandolo a un bitmap de 1x1
  BITMAPINFO bi{};
  bi.bmiHeader = { sizeof(BITMAPINFOHEADER), 1, -1, 1, 32, BI_RGB };
  void* bits;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP b = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(dc, b);
  RECT rc{ 0, 0, 1, 1 };
  g_rt->BindDC(dc, &rc);
  g_rt->BeginDraw();
  g_rt->EndDraw();
  SelectObject(dc, old);
  DeleteObject(b);
  DeleteDC(dc);
  HeapCompact(GetProcessHeap(), 0);
  SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

bool IsOwnWindow(HWND h) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  return pid == GetCurrentProcessId();
}

struct ACCENT_POLICY { int AccentState, AccentFlags; DWORD GradientColor; int AnimationId; };
struct WINCOMPATTRDATA { int Attribute; PVOID Data; SIZE_T SizeOfData; };

void ApplyGlass(HWND h, bool on) {
  static auto swca = (BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*))GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
  if (!swca) return;
  ACCENT_POLICY ap{ on ? 3 : 0, 0, 0, 0 };  // 3 = blur gaussiano detras de la ventana
  WINCOMPATTRDATA d{ 19, &ap, sizeof ap };
  swca(h, &d);
}

std::wstring FormatDate(const wchar_t* fmt, const SYSTEMTIME& st) {
  wchar_t buf[128];
  GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, fmt, buf, 128, nullptr);
  return buf;
}

// ================================================================ dibujo
static bool FontExists(const std::wstring& fam) {
  static std::map<std::wstring, bool> cache;
  auto it = cache.find(fam);
  if (it != cache.end()) return it->second;
  IDWriteFontCollection* col = nullptr;
  g_dw->GetSystemFontCollection(&col);
  UINT32 idx; BOOL exists = FALSE;
  if (col) { col->FindFamilyName(fam.c_str(), &idx, &exists); col->Release(); }
  return cache[fam] = !!exists;
}

IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT w, bool display) {
  static std::map<std::tuple<int, int, bool>, IDWriteTextFormat*> cache;
  auto key = std::make_tuple((int)(size * 10), (int)w, display);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  // San Francisco tiene prioridad si esta instalada; luego la fuente configurada, Inter y Segoe
  std::wstring fam = display ? g_cfg.fontDisplay : g_cfg.font;
  bool isDefault = fam.rfind(L"Segoe UI", 0) == 0;
  std::vector<std::wstring> cands;
  if (isDefault) {
    if (display) cands = { L"SF Pro Display", L"SF Pro", L"Inter Display", L"Inter", fam };
    else cands = { L"SF Pro Text", L"SF Pro", L"Inter", fam };
  }
  else cands = { fam };
  cands.push_back(L"Segoe UI Variable Text");
  cands.push_back(L"Segoe UI");
  for (auto& c : cands) if (FontExists(c)) { fam = c; break; }
  IDWriteTextFormat* f = nullptr;
  g_dw->CreateTextFormat(fam.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
  f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
  IDWriteInlineObject* ell = nullptr;
  g_dw->CreateEllipsisTrimmingSign(f, &ell);
  f->SetTrimming(&tr, ell);
  if (ell) ell->Release();
  return cache[key] = f;
}

ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c) {
  static ID2D1SolidColorBrush* b = nullptr;
  if (!b) g_rt->CreateSolidColorBrush(c, &b);
  b->SetColor(c);
  b->SetOpacity(1.f);
  return b;
}

float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
  IDWriteTextLayout* l = nullptr;
  g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 1000, &l);
  DWRITE_TEXT_METRICS m{};
  if (l) { l->GetMetrics(&m); l->Release(); }
  return m.widthIncludingTrailingWhitespace;
}

void DrawText(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c, Align a, bool vcenter) {
  f->SetTextAlignment(a == AL_LEFT ? DWRITE_TEXT_ALIGNMENT_LEADING : a == AL_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
  f->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void FillRound(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
  if (rad <= 0) rt->FillRectangle(r, Brush(c));
  else rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), Brush(c));
}

void StrokeRound(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w) {
  rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), Brush(c), w);
}

void DrawGlass(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float tintBoost) {
  D2D1_COLOR_F tint = g_th.tint;
  tint.a = Clamp(tint.a + tintBoost, 0.f, 1.f);
  FillRound(rt, r, rad, tint);
  // brillo superior tipo "liquid glass"
  ID2D1GradientStopCollection* stops = nullptr;
  D2D1_GRADIENT_STOP gs[2] = { { 0.f, RGBA(255, 255, 255, g_th.dark ? .07f : .22f) }, { 1.f, RGBA(255, 255, 255, 0) } };
  rt->CreateGradientStopCollection(gs, 2, &stops);
  ID2D1LinearGradientBrush* lg = nullptr;
  rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.top + (r.bottom - r.top) * .5f }), stops, &lg);
  rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg);
  lg->Release(); stops->Release();
  D2D1_RECT_F in = { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f };
  StrokeRound(rt, in, std::max(0.f, rad - .5f), g_th.rim, 1.f);
}

void DrawShadow(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float size, float alpha) {
  int steps = (int)size;
  for (int i = steps; i >= 1; i--) {
    float t = i / (float)steps;
    D2D1_RECT_F s = { r.left - i, r.top - i * .6f, r.right + i, r.bottom + i * 1.2f };
    StrokeRound(rt, s, rad + i, RGBA(0, 0, 0, alpha * (1 - t) * (1 - t) / steps * 3.f), 1.6f);
  }
}

ID2D1PathGeometry* MakePath(std::function<void(ID2D1GeometrySink*)> build, bool filled) {
  ID2D1PathGeometry* p = nullptr;
  g_d2d->CreatePathGeometry(&p);
  ID2D1GeometrySink* s = nullptr;
  p->Open(&s);
  build(s);
  s->Close(); s->Release();
  return p;
}

void DrawSymbol(ID2D1RenderTarget* rt, const wchar_t* glyph, D2D1_RECT_F r, float size, D2D1_COLOR_F c) {
  static std::map<int, IDWriteTextFormat*> fmts;
  int k = (int)(size * 10);
  if (!fmts[k]) {
    const wchar_t* fam = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    g_dw->CreateTextFormat(fam, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmts[k]);
    fmts[k]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmts[k]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  rt->DrawTextW(glyph, (UINT32)wcslen(glyph), fmts[k], r, Brush(c));
}

void DrawBitmapHQ(ID2D1RenderTarget* rt, ID2D1Bitmap* b, D2D1_RECT_F dst, float alpha, const D2D1_RECT_F* src) {
  // la interpolacion cubica de alta calidad evita el "ruido" al reducir iconos grandes
  if (g_dc && rt == g_rt) g_dc->DrawBitmap(b, &dst, alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, src, nullptr);
  else rt->DrawBitmap(b, dst, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
}

void PushRoundClip(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad) {
  ID2D1RoundedRectangleGeometry* g = nullptr;
  g_d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, rad, rad), &g);
  rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
  g->Release();
}

void PopClip(ID2D1RenderTarget* rt) { rt->PopLayer(); }

void DrawFolderIcon(ID2D1RenderTarget* rt, D2D1_RECT_F r, float a, const wchar_t* glyph) {
  // carpeta azul estilo macOS
  float s = r.right - r.left, l = r.left + s * .04f, rr = r.right - s * .04f, t = r.top + s * .14f, b = r.bottom - s * .1f;
  FillRound(rt, { l, t, l + s * .4f, t + s * .2f }, s * .05f, RGBA(78, 160, 236, a));
  FillRound(rt, { l, t + s * .07f, rr, b }, s * .07f, RGBA(78, 160, 236, a));
  ID2D1GradientStopCollection* st = nullptr;
  D2D1_GRADIENT_STOP gs[2] = { { 0, RGBA(132, 196, 255, a) }, { 1, RGBA(92, 172, 248, a) } };
  rt->CreateGradientStopCollection(gs, 2, &st);
  ID2D1LinearGradientBrush* lg = nullptr;
  rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, t + s * .16f }, { 0, b }), st, &lg);
  rt->FillRoundedRectangle(D2D1::RoundedRect({ l, t + s * .16f, rr, b }, s * .07f, s * .07f), lg);
  lg->Release(); st->Release();
  rt->FillRectangle({ l + s * .02f, t + s * .16f, rr - s * .02f, t + s * .16f + std::max(.6f, s * .012f) }, Brush(RGBA(255, 255, 255, .55f * a)));
  if (glyph) DrawSymbol(rt, glyph, { l, t + s * .2f, rr, b }, s * .34f, RGBA(48, 128, 214, a));
}

// ================================================================ superficies
static const wchar_t* kSurfaceClass = L"DinamicZaurSurface";

Surface::~Surface() {
  if (memDC) { SelectObject(memDC, oldBmp); DeleteDC(memDC); }
  if (dib) DeleteObject(dib);
  if (hwnd) DestroyWindow(hwnd);
}

void Surface::Create(DWORD exStyle, bool layered_, bool glass) {
  static bool reg = false;
  if (!reg) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc; wc.hInstance = g_inst; wc.lpszClassName = kSurfaceClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    reg = true;
  }
  layered = layered_;
  hwnd = CreateWindowExW(exStyle | WS_EX_TOOLWINDOW | (layered ? WS_EX_LAYERED : 0), kSurfaceClass, DZ_NAME, WS_POPUP,
                         0, 0, 10, 10, nullptr, nullptr, g_inst, this);
  int corner = dwmRound ? 2 : 1;  // 2 = redondeo de Windows 11 con antialias; 1 = sin redondeo
  DwmSetWindowAttribute(hwnd, 33, &corner, sizeof corner);
  COLORREF none = 0xFFFFFFFE;
  DwmSetWindowAttribute(hwnd, 34, &none, sizeof none);
  if (glass) {
    MARGINS m{ -1 };
    DwmExtendFrameIntoClientArea(hwnd, &m);
    ApplyGlass(hwnd, true);
  }
}

void Surface::ApplyShape() {
  if (layered || dwmRound) return;
  if (radius > 0) {
    int r = Px(radius * 2);
    SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, r, r), TRUE);
  } else SetWindowRgn(hwnd, nullptr, TRUE);
}

void Surface::SetBounds(int nx, int ny, int nw, int nh, HWND after) {
  bool resized = nw != w || nh != h;
  x = nx; y = ny; w = nw; h = nh;
  if (layered) { RenderNow(); return; }
  UINT fl = SWP_NOACTIVATE | (after ? 0 : SWP_NOZORDER);
  SetWindowPos(hwnd, after, x, y, w, h, fl);
  if (resized) ApplyShape();
  Invalidate();
}

void Surface::Invalidate() {
  if (!hwnd) return;
  if (!layered) { InvalidateRect(hwnd, nullptr, FALSE); return; }
  if (!pending) { pending = true; PostMessageW(hwnd, WM_APP_RENDER, 0, 0); }
}

void Surface::Show(bool show, bool activate) {
  if (!hwnd) return;
  if (show) {
    if (layered) RenderNow();
    ShowWindow(hwnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(hwnd);
  } else ShowWindow(hwnd, SW_HIDE);
}

void Surface::RenderNow() {
  pending = false;
  if (w <= 0 || h <= 0) return;
  if (!layered) { InvalidateRect(hwnd, nullptr, FALSE); UpdateWindow(hwnd); return; }
  if (!memDC) memDC = CreateCompatibleDC(nullptr);
  if (dibW != w || dibH != h) {
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits;
    HBITMAP nb = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ prev = SelectObject(memDC, nb);
    if (!oldBmp) oldBmp = prev;
    if (dib) DeleteObject(dib);
    dib = nb; dibW = w; dibH = h;
  }
  RECT rc{ 0, 0, w, h };
  g_rt->BindDC(memDC, &rc);
  g_rt->BeginDraw();
  g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
  Paint(g_rt);
  g_rt->EndDraw();
  POINT pos{ x, y }, src{ 0, 0 };
  SIZE sz{ w, h };
  BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
  UpdateLayeredWindow(hwnd, nullptr, &pos, &sz, memDC, &src, 0, &bf, ULW_ALPHA);
}

LRESULT Surface::Handle(UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(hwnd, m, wp, lp); }

LRESULT CALLBACK Surface::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_NCCREATE) {
    auto cs = (CREATESTRUCTW*)lp;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    ((Surface*)cs->lpCreateParams)->hwnd = h;
  }
  auto s = (Surface*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (!s) return DefWindowProcW(h, m, wp, lp);
  switch (m) {
  case WM_ERASEBKGND: return 1;
  case WM_APP_RENDER: s->RenderNow(); return 0;
  case WM_WINDOWPOSCHANGED:
    if (s->layered) {  // al arrastrar una ventana layered, mantenemos su posicion al dia
      auto p = (WINDOWPOS*)lp;
      if (!(p->flags & SWP_NOMOVE)) { s->x = p->x; s->y = p->y; }
    }
    break;
  case WM_PAINT:
    if (!s->layered) {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc{ 0, 0, s->w, s->h };
      g_rt->BindDC(dc, &rc);
      g_rt->BeginDraw();
      g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
      s->Paint(g_rt);
      g_rt->EndDraw();
      EndPaint(h, &ps);
      return 0;
    }
    break;
  }
  return s->Handle(m, wp, lp);
}

