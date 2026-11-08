#include "common.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <thread>

// Ventana de configuracion (y bienvenida la primera vez) con la identidad de Zargch Studio:
// fondo casi negro con resplandor rojizo, naranja de marca y el logo en blanco.
// Los campos de texto y la lista son controles de Windows (teclado, IME y lector de pantalla
// funcionan solos); botones, opciones e interruptores se dibujan con Direct2D.

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------- colores de marca
namespace Z {
const D2D1_COLOR_F bg = RGBA(11, 11, 11), field = RGBA(27, 27, 27), line = RGBA(52, 48, 46),
                   orange = RGBA(232, 115, 26), orangeDn = RGBA(196, 92, 14), ember = RGBA(150, 29, 18), glow = RGBA(91, 28, 15),
                   text = RGBA(245, 242, 238), dim = RGBA(160, 152, 145), faint = RGBA(105, 99, 94);
const COLORREF cBg = RGB(11, 11, 11), cField = RGB(27, 27, 27), cText = RGB(245, 242, 238), cDim = RGB(120, 114, 108);
}

enum {
  ID_SEARCH_EDIT = 200, ID_SEARCH_BTN, ID_RESULTS, ID_LOCATE, ID_CELSIUS, ID_FAHRENHEIT,
  ID_CAL0, ID_CAL1, ID_CAL2, ID_CAL3, ID_CAL4,  // una opcion por tipo de calendario (CalendarKind)
  ID_CAL_PATH, ID_CAL_BROWSE, ID_STARTUP, ID_AUTOHIDE, ID_FULLSCREEN, ID_SAVE,
};
enum { WM_SET_RESULTS = WM_APP + 50, WM_SET_LOCATED };
enum Kind { K_PRIMARY, K_SECONDARY, K_CHIP, K_TOGGLE };

static HWND s_wnd;
static bool s_welcome;
static HFONT s_font;
static HBRUSH s_bgBrush, s_fieldBrush;
static ID2D1Bitmap* s_logo;
static std::vector<Place> s_results;
static Place s_sel;
static bool s_hasSel, s_busy;
static int s_cal;
static bool s_fahr, s_tog[3];
static std::map<int, Kind> s_kind;
static std::vector<D2D1_RECT_F> s_fields;  // recuadros de los campos (se pintan en el fondo)
static float s_W, s_H, s_headerH;
static D2D1_RECT_F s_cityRect;
static float s_colL, s_colR, s_colW, s_bodyY;

// ---------------------------------------------------------------- inicio con Windows
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
static const wchar_t* kRunName = L"DinamicZaur";

bool Startup_Enabled() {
  wchar_t buf[MAX_PATH * 2]; DWORD sz = sizeof buf;
  if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
  // desactivado desde el Administrador de tareas / Aplicaciones de inicio: primer byte impar
  BYTE ap[12] = {}; DWORD asz = sizeof ap;
  if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunName, RRF_RT_REG_BINARY, nullptr, ap, &asz) == ERROR_SUCCESS && (ap[0] & 1)) return false;
  return true;
}

void Startup_Set(bool on) {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    if (on) {
      wchar_t exe[MAX_PATH];
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      std::wstring v = L"\"" + std::wstring(exe) + L"\" --autostart";
      RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)(v.size() + 1) * 2);
    } else RegDeleteValueW(k, kRunName);
    RegCloseKey(k);
  }
  // marca de "aprobado" que usa Windows para las apps de inicio
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
    if (on) { BYTE enabled[12] = { 2 }; RegSetValueExW(k, kRunName, 0, REG_BINARY, enabled, sizeof enabled); }
    else RegDeleteValueW(k, kRunName);
    RegCloseKey(k);
  }
}

// ---------------------------------------------------------------- calendario
void OpenCalendar() {
  switch (g_cfg.calendar) {
  case CAL_WINDOWS:
    // app Calendario de Windows / Outlook (nuevo); si no hay ninguna registrada, Outlook en la web
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"outlookcal:", nullptr, nullptr, SW_SHOWNORMAL) <= 32)
      RunShell(L"https://outlook.live.com/calendar/");
    break;
  case CAL_GOOGLE: RunShell(L"https://calendar.google.com/"); break;
  case CAL_OUTLOOK_WEB: RunShell(L"https://outlook.live.com/calendar/"); break;
  case CAL_CUSTOM:
    if (!g_cfg.calendarPath.empty()) RunShell(g_cfg.calendarPath);
    else Settings_Open(false);
    break;
  default: break;
  }
}

bool Settings_IsOpen() { return s_wnd != nullptr; }
HWND Settings_Hwnd() { return s_wnd; }

// ---------------------------------------------------------------- dibujo
// tipografia de la marca (Montserrat) si esta instalada; si no, la de Windows
static IDWriteTextFormat* BrandFont(float size, DWRITE_FONT_WEIGHT w, bool wrap = false) {
  static std::map<std::tuple<int, int, bool>, IDWriteTextFormat*> cache;
  auto key = std::make_tuple((int)(size * 10), (int)w, wrap);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  IDWriteFontCollection* fc = nullptr;
  g_dw->GetSystemFontCollection(&fc);
  std::wstring fam = IsWin11() ? L"Segoe UI Variable Display" : L"Segoe UI";
  UINT32 idx; BOOL exists = FALSE;
  if (fc && SUCCEEDED(fc->FindFamilyName(L"Montserrat", &idx, &exists)) && exists) fam = L"Montserrat";
  if (fc) fc->Release();
  IDWriteTextFormat* f = nullptr;
  g_dw->CreateTextFormat(fam.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
  f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  if (!wrap) {
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    IDWriteInlineObject* ell = nullptr;
    g_dw->CreateEllipsisTrimmingSign(f, &ell);
    f->SetTrimming(&tr, ell);
    if (ell) ell->Release();
  }
  return cache[key] = f;
}

// logo de Zargch Studio en blanco (PNG incrustado en el exe)
static ID2D1Bitmap* Logo() {
  if (s_logo) return s_logo;
  HRSRC rs = FindResourceW(g_inst, MAKEINTRESOURCEW(2), RT_RCDATA);
  HGLOBAL rg = rs ? LoadResource(g_inst, rs) : nullptr;
  void* data = rg ? LockResource(rg) : nullptr;
  if (!data) return nullptr;
  IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
  if (SUCCEEDED(g_wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)data, SizeofResource(g_inst, rs))) &&
      SUCCEEDED(g_wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
      SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
      SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
    g_rt->CreateBitmapFromWicBitmap(cv, nullptr, &s_logo);
  if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); if (st) st->Release();
  return s_logo;
}

// dibuja con Direct2D en el rectangulo rc del DC (el origen del dibujo queda en rc.left/top)
static void WithD2D(HDC dc, RECT rc, const std::function<void(ID2D1RenderTarget*)>& f) {
  g_rt->BindDC(dc, &rc);
  g_rt->BeginDraw();
  f(g_rt);
  if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET && s_logo) { s_logo->Release(); s_logo = nullptr; }
}

static std::wstring Spaced(const std::wstring& s) {  // tracking amplio, como el lema de la marca
  std::wstring r;
  for (wchar_t c : s) { if (!r.empty()) r += L' '; r += c; }
  return r;
}

static void Section(ID2D1RenderTarget* rt, float x, float y, float w, const wchar_t* t) {
  DrawText(rt, Spaced(t), BrandFont(11, DWRITE_FONT_WEIGHT_BOLD), { x, y, x + w, y + 16 }, Z::orange);
  rt->FillRectangle({ x, y + 21, x + w, y + 22 }, Brush(Z::line));
}

static void PaintBackground(ID2D1RenderTarget* rt) {
  rt->Clear(Z::bg);
  // resplandor rojizo de la marca detras del logo
  ID2D1GradientStopCollection* sc = nullptr;
  D2D1_GRADIENT_STOP stops[] = { { 0, WithA(Z::ember, .55f) }, { .45f, WithA(Z::glow, .45f) }, { 1, WithA(Z::bg, 0) } };
  rt->CreateGradientStopCollection(stops, 3, &sc);
  ID2D1RadialGradientBrush* rb = nullptr;
  float cx = s_W / 2, cy = s_welcome ? 62.f : 30.f;
  rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({ cx, cy }, { 0, 0 }, s_W * .42f, s_welcome ? 150.f : 70.f), sc, &rb);
  if (rb) { rt->FillRectangle({ 0, 0, s_W, s_headerH + 30 }, rb); rb->Release(); }
  if (sc) sc->Release();
  // fragmentos geometricos (como el camaleon low-poly), fijos y discretos
  static const float tri[][4] = { { .07f, .18f, 5, 20 }, { .16f, .62f, 4, 200 }, { .27f, .3f, 3, 110 }, { .74f, .22f, 5, 300 }, { .83f, .58f, 4, 40 },
                                  { .92f, .3f, 3, 160 }, { .64f, .78f, 3, 250 }, { .36f, .82f, 3, 80 } };
  for (auto& t : tri) {
    float x = s_W * t[0], y = s_headerH * t[1], s = t[2];
    auto p = MakePath([&](ID2D1GeometrySink* g) {
      g->BeginFigure({ x, y - s }, D2D1_FIGURE_BEGIN_FILLED);
      g->AddLine({ x + s, y + s * .8f }); g->AddLine({ x - s, y + s * .6f });
      g->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    D2D1_MATRIX_3X2_F old; rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(t[3], { x, y }) * old);
    rt->FillGeometry(p, Brush(WithA(Z::orange, .35f)));
    rt->SetTransform(old);
    p->Release();
  }
  // cabecera
  ID2D1Bitmap* logo = Logo();
  D2D1_SIZE_F ls = logo ? logo->GetSize() : D2D1_SIZE_F{ 1, 1 };
  float ar = ls.width / ls.height;
  if (s_welcome) {
    float lh = 64, lw = lh * ar;
    if (logo) DrawBitmapHQ(rt, logo, { cx - lw / 2, 22, cx + lw / 2, 22 + lh });
    DrawText(rt, Spaced(T(L"TECNOLOGÍA QUE SE ADAPTA", L"TECHNOLOGY THAT ADAPTS")), BrandFont(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             { 0, 92, s_W, 106 }, Z::dim, AL_CENTER);
    DrawText(rt, T(L"¡Hola! Vamos a dejar lista tu isla dinámica", L"Hi! Let's set up your dynamic island"), BrandFont(21, DWRITE_FONT_WEIGHT_BOLD),
             { 0, 118, s_W, 146 }, Z::text, AL_CENTER);
    DrawText(rt, T(L"Elige tu ciudad para el clima y qué calendario abrir. Puedes cambiarlo cuando quieras.",
                   L"Pick your city for the weather and which calendar to open. You can change it anytime."),
             BrandFont(12.5f, DWRITE_FONT_WEIGHT_NORMAL), { 0, 148, s_W, 168 }, Z::dim, AL_CENTER);
  } else {
    float lh = 34, lw = lh * ar;
    if (logo) DrawBitmapHQ(rt, logo, { 28, 18, 28 + lw, 18 + lh });
    rt->FillRectangle({ 28 + lw + 16, 22, 28 + lw + 17, 48 }, Brush(Z::line));
    DrawText(rt, std::wstring(DZ_NAME) + T(L" · Configuración", L" · Settings"), BrandFont(16, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             { 28 + lw + 32, 18, s_W - 28, 52 }, Z::text);
  }
  // secciones
  float y = s_bodyY;
  Section(rt, s_colL, y, s_colW, T(L"CLIMA", L"WEATHER"));
  Section(rt, s_colR, y, s_colW, T(L"CALENDARIO", L"CALENDAR"));
  Section(rt, s_colR, y + 168, s_colW, L"GENERAL");
  // ubicacion actual
  std::wstring city;
  if (s_hasSel) city = s_sel.label;
  else if (g_cfg.hasLoc) city = g_cfg.weatherCity.empty() ? T(L"Coordenadas guardadas", L"Saved coordinates") : g_cfg.weatherCity;
  rt->FillEllipse(D2D1::Ellipse({ s_cityRect.left + 4, (s_cityRect.top + s_cityRect.bottom) / 2 }, 4, 4), Brush(city.empty() ? Z::faint : Z::orange));
  DrawText(rt, city.empty() ? T(L"Aún no has elegido tu ciudad", L"You haven't picked your city yet") : city,
           BrandFont(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), { s_cityRect.left + 16, s_cityRect.top, s_cityRect.right, s_cityRect.bottom }, city.empty() ? Z::dim : Z::text);
  // recuadros de los campos de texto
  for (auto& f : s_fields) {
    FillRound(rt, f, 9, Z::field);
    StrokeRound(rt, { f.left + .5f, f.top + .5f, f.right - .5f, f.bottom - .5f }, 9, Z::line);
  }
  // pie
  DrawText(rt, T(L"Clima: MET Norway (CC BY 4.0) · Lugares: © OpenStreetMap (ODbL)", L"Weather: MET Norway (CC BY 4.0) · Places: © OpenStreetMap (ODbL)"),
           BrandFont(10.5f, DWRITE_FONT_WEIGHT_NORMAL), { s_colL, s_H - 56, s_colL + 420, s_H - 40 }, Z::faint);
  DrawText(rt, std::wstring(DZ_NAME) + L" " + DZ_VERSION + T(L" · un proyecto de Zargch Studio", L" · a Zargch Studio project"),
           BrandFont(10.5f, DWRITE_FONT_WEIGHT_NORMAL), { s_colL, s_H - 38, s_colL + 420, s_H - 22 }, Z::faint);
}

static void DrawControl(const DRAWITEMSTRUCT* di) {
  int id = (int)di->CtlID;
  RECT rc = di->rcItem;
  float w = Dip(rc.right - rc.left), h = Dip(rc.bottom - rc.top);
  wchar_t txt[128] = {};
  GetWindowTextW(di->hwndItem, txt, 128);
  bool down = di->itemState & ODS_SELECTED, focus = (di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT);
  bool disabled = di->itemState & ODS_DISABLED;
  Kind k = s_kind.count(id) ? s_kind[id] : K_SECONDARY;
  WithD2D(di->hDC, rc, [&](ID2D1RenderTarget* rt) {
    rt->Clear(Z::bg);
    D2D1_RECT_F r{ 1, 1, w - 1, h - 1 };
    float a = disabled ? .4f : 1.f;
    if (k == K_PRIMARY) {
      FillRound(rt, r, h / 2 - 1, WithA(down ? Z::orangeDn : Z::orange, a));
      DrawText(rt, txt, BrandFont(13, DWRITE_FONT_WEIGHT_BOLD), r, WithA(RGBA(255, 255, 255), a), AL_CENTER);
    } else if (k == K_SECONDARY) {
      FillRound(rt, r, h / 2 - 1, WithA(down ? RGBA(44, 41, 39) : Z::field, a));
      StrokeRound(rt, r, h / 2 - 1, WithA(Z::line, a));
      DrawText(rt, txt, BrandFont(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), r, WithA(Z::text, a), AL_CENTER);
    } else if (k == K_CHIP) {
      bool on = (id >= ID_CAL0 && id <= ID_CAL4 && s_cal == id - ID_CAL0) || (id == ID_CELSIUS && !s_fahr) || (id == ID_FAHRENHEIT && s_fahr);
      FillRound(rt, r, 9, on ? WithA(Z::orange, .16f) : Z::field);
      StrokeRound(rt, { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, 9, on ? Z::orange : Z::line, on ? 1.4f : 1.f);
      DrawText(rt, txt, BrandFont(12, on ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD), { r.left + 6, r.top, r.right - 6, r.bottom },
               on ? Z::text : Z::dim, AL_CENTER);
    } else {  // interruptor
      bool on = s_tog[id - ID_STARTUP];
      DrawText(rt, txt, BrandFont(12.5f, DWRITE_FONT_WEIGHT_NORMAL), { 0, 0, w - 56, h }, Z::text);
      D2D1_RECT_F sw{ w - 44, h / 2 - 11, w - 2, h / 2 + 11 };
      FillRound(rt, sw, 11, on ? Z::orange : RGBA(58, 54, 51));
      float kx = on ? sw.right - 11 : sw.left + 11;
      rt->FillEllipse(D2D1::Ellipse({ kx, h / 2 }, 8.5f, 8.5f), Brush(RGBA(255, 255, 255)));
    }
    if (focus) StrokeRound(rt, { .5f, .5f, w - .5f, h - .5f }, k == K_PRIMARY || k == K_SECONDARY ? h / 2 : 10, WithA(Z::orange, .9f), 1.2f);
  });
}

static void DrawListItem(const DRAWITEMSTRUCT* di) {
  RECT rc = di->rcItem;
  if (di->itemID == (UINT)-1) { FillRect(di->hDC, &rc, s_fieldBrush); return; }
  wchar_t txt[512] = {};
  SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)txt);
  bool sel = di->itemState & ODS_SELECTED;
  bool isPlace = di->itemID < s_results.size();
  float w = Dip(rc.right - rc.left), h = Dip(rc.bottom - rc.top);
  WithD2D(di->hDC, rc, [&](ID2D1RenderTarget* rt) {
    rt->Clear(Z::field);
    if (sel && isPlace) FillRound(rt, { 3, 1, w - 3, h - 1 }, 6, WithA(Z::orange, .22f));
    if (isPlace) DrawSymbol(rt, L"", { 6, 0, 28, h }, 11, sel ? Z::orange : Z::faint);
    DrawText(rt, txt, BrandFont(12.5f, sel && isPlace ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL),
             { isPlace ? 30.f : 10.f, 0, w - 8, h }, isPlace ? Z::text : Z::dim);
  });
}

// ---------------------------------------------------------------- controles
static HWND Item(int id) { return GetDlgItem(s_wnd, id); }

static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, float x, float y, float w, float h, int id) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(y), Px(w), Px(h), s_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
  SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
  return c;
}

static HWND Btn(Kind k, const wchar_t* text, float x, float y, float w, float h, int id) {
  s_kind[id] = k;
  return Ctl(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, x, y, w, h, id);
}

// campo de texto sin borde dentro de un recuadro redondeado pintado en el fondo
static HWND Field(float x, float y, float w, float h, int id, const wchar_t* cue) {
  s_fields.push_back({ x, y, x + w, y + h });
  float th = 20;
  HWND e = Ctl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x + 12, y + (h - th) / 2, w - 24, th, id);
  SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
  return e;
}

static void Repaint(int id) { InvalidateRect(Item(id), nullptr, FALSE); }
static void RepaintCity() { RECT r{ Px(s_cityRect.left), Px(s_cityRect.top), Px(s_cityRect.right), Px(s_cityRect.bottom) }; InvalidateRect(s_wnd, &r, TRUE); }

static void SetCalendar(int c) {
  s_cal = c;
  for (int i = ID_CAL0; i <= ID_CAL4; i++) Repaint(i);
  EnableWindow(Item(ID_CAL_PATH), c == CAL_CUSTOM);
  EnableWindow(Item(ID_CAL_BROWSE), c == CAL_CUSTOM);
  if (c == CAL_CUSTOM) SetFocus(Item(ID_CAL_PATH));
}

static void SetBusy(bool b) {
  s_busy = b;
  EnableWindow(Item(ID_SEARCH_BTN), !b);
  EnableWindow(Item(ID_LOCATE), !b);
}

static void ListMessage(std::initializer_list<const wchar_t*> lines) {
  HWND lb = Item(ID_RESULTS);
  SendMessageW(lb, LB_RESETCONTENT, 0, 0);
  for (auto l : lines) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)l);
}

static void Search() {
  if (s_busy) return;
  wchar_t q[256];
  GetWindowTextW(Item(ID_SEARCH_EDIT), q, 256);
  std::wstring query = q;
  while (!query.empty() && query.back() == L' ') query.pop_back();
  if (query.size() < 2) { SetFocus(Item(ID_SEARCH_EDIT)); return; }
  s_results.clear();
  ListMessage({ T(L"Buscando…", L"Searching…") });
  SetBusy(true);
  HWND h = s_wnd;
  std::thread([query, h] {
    auto r = new std::vector<Place>(Weather_Search(query));
    if (!PostMessageW(h, WM_SET_RESULTS, 0, (LPARAM)r)) delete r;
  }).detach();
}

static void Locate() {
  if (s_busy) return;
  s_results.clear();
  ListMessage({ T(L"Buscando tu ubicación…", L"Finding your location…") });
  SetBusy(true);
  HWND h = s_wnd;
  std::thread([h] {
    auto p = new Place{};
    if (!Weather_Locate(*p)) { delete p; p = nullptr; }
    if (!PostMessageW(h, WM_SET_LOCATED, 0, (LPARAM)p)) delete p;
  }).detach();
}

static void Browse() {
  wchar_t file[MAX_PATH] = {};
  OPENFILENAMEW of{ sizeof of };
  of.hwndOwner = s_wnd;
  of.lpstrFilter = g_es ? L"Programas\0*.exe;*.lnk;*.url\0Todos los archivos\0*.*\0" : L"Programs\0*.exe;*.lnk;*.url\0All files\0*.*\0";
  of.lpstrFile = file; of.nMaxFile = MAX_PATH;
  of.Flags = OFN_FILEMUSTEXIST | OFN_NODEREFERENCELINKS | OFN_HIDEREADONLY;
  if (GetOpenFileNameW(&of)) SetWindowTextW(Item(ID_CAL_PATH), file);
}

static void Save() {
  Config& c = g_cfg;
  bool locChanged = false;
  if (s_hasSel) {
    c.lat = s_sel.lat; c.lon = s_sel.lon; c.hasLoc = true;
    c.weatherCity = s_sel.label;
    locChanged = true;
  }
  c.fahrenheit = s_fahr;
  c.calendar = s_cal;
  wchar_t p[MAX_PATH * 2];
  GetWindowTextW(Item(ID_CAL_PATH), p, MAX_PATH * 2);
  c.calendarPath = p;
  if (c.calendar == CAL_CUSTOM && c.calendarPath.empty()) c.calendar = CAL_WINDOWS;
  c.autohide = s_tog[1];
  c.hideOnFullscreen = s_tog[2];
  c.firstRunDone = true;
  SaveConfig();
  if (s_tog[0] != Startup_Enabled()) Startup_Set(s_tog[0]);
  if (locChanged) Weather_Fetch();
  App_SettingsChanged();
}

// ---------------------------------------------------------------- ventana
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
  case WM_ERASEBKGND: return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT cr; GetClientRect(h, &cr);
    // doble buffer: sin parpadeo al pintar el resplandor
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    WithD2D(mem, cr, PaintBackground);
    BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(h, &ps);
    return 0;
  }
  case WM_MEASUREITEM: {
    auto mi = (MEASUREITEMSTRUCT*)lp;
    if (mi->CtlType == ODT_LISTBOX) mi->itemHeight = Px(28);
    return TRUE;
  }
  case WM_DRAWITEM: {
    auto di = (DRAWITEMSTRUCT*)lp;
    if (di->CtlType == ODT_LISTBOX) DrawListItem(di); else DrawControl(di);
    return TRUE;
  }
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORSTATIC: {  // los campos deshabilitados llegan como "static"
    HDC dc = (HDC)wp;
    bool enabled = IsWindowEnabled((HWND)lp);
    SetTextColor(dc, enabled ? Z::cText : Z::cDim);
    SetBkColor(dc, Z::cField);
    return (LRESULT)s_fieldBrush;
  }
  case WM_COMMAND: {
    int id = LOWORD(wp), code = HIWORD(wp);
    switch (id) {
    case IDOK:  // Enter
      if (GetFocus() == Item(ID_SEARCH_EDIT)) Search();
      else { Save(); DestroyWindow(h); }
      return 0;
    case IDCANCEL: DestroyWindow(h); return 0;
    case ID_SEARCH_BTN: Search(); return 0;
    case ID_LOCATE: Locate(); return 0;
    case ID_CAL_BROWSE: Browse(); return 0;
    case ID_SAVE: Save(); DestroyWindow(h); return 0;
    case ID_CELSIUS: case ID_FAHRENHEIT: s_fahr = id == ID_FAHRENHEIT; Repaint(ID_CELSIUS); Repaint(ID_FAHRENHEIT); return 0;
    case ID_STARTUP: case ID_AUTOHIDE: case ID_FULLSCREEN: s_tog[id - ID_STARTUP] = !s_tog[id - ID_STARTUP]; Repaint(id); return 0;
    case ID_RESULTS:
      if (code == LBN_SELCHANGE) {
        int i = (int)SendMessageW(Item(ID_RESULTS), LB_GETCURSEL, 0, 0);
        if (i >= 0 && i < (int)s_results.size()) { s_sel = s_results[i]; s_hasSel = true; RepaintCity(); }
      }
      return 0;
    }
    if (id >= ID_CAL0 && id <= ID_CAL4) { SetCalendar(id - ID_CAL0); return 0; }
    break;
  }
  case WM_SET_RESULTS: {
    std::unique_ptr<std::vector<Place>> r((std::vector<Place>*)lp);
    SetTimer(h, 1, 1100, nullptr);  // pausa minima entre consultas (politica de uso de Nominatim)
    s_results = *r;
    HWND lb = Item(ID_RESULTS);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    if (s_results.empty()) ListMessage({ T(L"No se encontró ningún lugar (¿sin internet?)", L"No places found (offline?)") });
    for (auto& p : s_results) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)p.label.c_str());
    if (s_results.size() == 1) { SendMessageW(lb, LB_SETCURSEL, 0, 0); s_sel = s_results[0]; s_hasSel = true; RepaintCity(); }
    return 0;
  }
  case WM_SET_LOCATED: {
    std::unique_ptr<Place> p((Place*)lp);
    SetTimer(h, 1, 1100, nullptr);
    s_results.clear();
    if (p) {
      s_results.push_back(*p);
      ListMessage({ p->label.c_str() });
      SendMessageW(Item(ID_RESULTS), LB_SETCURSEL, 0, 0);
      s_sel = *p; s_hasSel = true; RepaintCity();
    } else {
      ListMessage({ T(L"No se pudo obtener la ubicación.", L"Couldn't get your location."),
                    T(L"Activa la ubicación de Windows o busca tu ciudad.", L"Turn on Windows location or search your city.") });
    }
    return 0;
  }
  case WM_TIMER:
    if (wp == 1) { KillTimer(h, 1); SetBusy(false); }
    return 0;
  case WM_DESTROY:
    s_wnd = nullptr;
    s_results.clear();
    s_fields.clear();
    s_kind.clear();
    s_hasSel = false;
    s_busy = false;
    // la primera vez, aunque se cierre sin guardar, no se vuelve a mostrar la bienvenida
    if (!g_cfg.firstRunDone) { g_cfg.firstRunDone = true; SaveConfig(); }
    return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

void Settings_Open(bool welcome) {
  if (s_wnd) { ShowWindow(s_wnd, SW_RESTORE); SetForegroundWindow(s_wnd); return; }
  static bool reg = false;
  if (!reg) {
    INITCOMMONCONTROLSEX ic{ sizeof ic, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
    s_bgBrush = CreateSolidBrush(Z::cBg);
    s_fieldBrush = CreateSolidBrush(Z::cField);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc; wc.hInstance = g_inst; wc.lpszClassName = L"DinamicZaurSettings";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = s_bgBrush;
    wc.hIcon = App_Icon(GetSystemMetrics(SM_CXICON));
    RegisterClassW(&wc);
    reg = true;
  }
  if (!s_font) {
    NONCLIENTMETRICSW nm{ sizeof nm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
    LOGFONTW lf = nm.lfMessageFont;
    lf.lfHeight = -MulDiv(10, (int)(96 * g_scale), 72);
    s_font = CreateFontIndirectW(&lf);
  }
  s_welcome = welcome;
  s_hasSel = false;
  s_cal = g_cfg.calendar;
  s_fahr = g_cfg.fahrenheit;
  s_tog[0] = welcome ? true : Startup_Enabled();
  s_tog[1] = g_cfg.autohide;
  s_tog[2] = g_cfg.hideOnFullscreen;

  // dos columnas: clima a la izquierda; calendario y general a la derecha
  const float M = 32, GAP = 32;
  s_W = 800;
  s_colW = (s_W - 2 * M - GAP) / 2;
  s_colL = M; s_colR = M + s_colW + GAP;
  s_headerH = welcome ? 184.f : 74.f;
  s_bodyY = s_headerH + 8;
  s_H = s_bodyY + 300 + 72;
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  RECT r{ 0, 0, Px(s_W), Px(s_H) };
  AdjustWindowRectEx(&r, style, FALSE, 0);
  RECT mon; SystemParametersInfoW(SPI_GETWORKAREA, 0, &mon, 0);
  int ww = r.right - r.left, wh = r.bottom - r.top;
  std::wstring title = std::wstring(welcome ? T(L"Bienvenido a ", L"Welcome to ") : T(L"Configuración de ", L"Settings - ")) + DZ_NAME;
  s_wnd = CreateWindowExW(0, L"DinamicZaurSettings", title.c_str(), style, (mon.left + mon.right - ww) / 2,
                          std::max((int)mon.top, (int)(mon.top + mon.bottom - wh) / 2), ww, wh, nullptr, nullptr, g_inst, nullptr);
  // barra de titulo oscura y del mismo negro que la ventana
  BOOL dark = TRUE;
  DwmSetWindowAttribute(s_wnd, 20, &dark, sizeof dark);
  DwmSetWindowAttribute(s_wnd, 19, &dark, sizeof dark);  // Windows 10 1809-1909
  COLORREF cap = Z::cBg, border = RGB(60, 30, 18);
  DwmSetWindowAttribute(s_wnd, 35, &cap, sizeof cap);
  DwmSetWindowAttribute(s_wnd, 34, &border, sizeof border);

  // ---- clima (columna izquierda)
  float x = s_colL, y = s_bodyY + 30, W = s_colW;
  s_cityRect = { x, y, x + W, y + 22 };
  y += 32;
  HWND ed = Field(x, y, W - 112, 38, ID_SEARCH_EDIT, T(L"Tu ciudad: Lima, Madrid, Tokio…", L"Your city: London, Lima, Tokyo…"));
  Btn(K_PRIMARY, T(L"Buscar", L"Search"), x + W - 100, y, 100, 38, ID_SEARCH_BTN);
  y += 48;
  s_fields.push_back({ x, y, x + W, y + 128 });
  HWND lb = Ctl(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                x + 4, y + 4, W - 8, 120, ID_RESULTS);
  SetWindowTheme(lb, L"DarkMode_Explorer", nullptr);  // barra de desplazamiento oscura
  ListMessage({ T(L"Busca tu ciudad o usa tu ubicación.", L"Search your city or use your location.") });
  y += 140;
  Btn(K_SECONDARY, T(L"Usar mi ubicación", L"Use my location"), x, y, 176, 36, ID_LOCATE);
  Btn(K_CHIP, L"°C", x + W - 120, y, 56, 36, ID_CELSIUS);
  Btn(K_CHIP, L"°F", x + W - 56, y, 56, 36, ID_FAHRENHEIT);

  // ---- calendario (columna derecha)
  x = s_colR; y = s_bodyY + 32;
  const wchar_t* cals[] = { T(L"Windows / Outlook", L"Windows / Outlook"), L"Google", L"Outlook.com", T(L"Otra app…", L"Other app…"), T(L"Ninguno", L"None") };
  // primera fila: el nombre largo ("Windows / Outlook") recibe mas ancho
  float w0 = (W - 16) * .42f, w1 = (W - 16) * .26f, w2 = (W - 16) - w0 - w1, cw2 = (W - 8) / 2;
  const float rx[5] = { x, x + w0 + 8, x + w0 + w1 + 16, x, x + cw2 + 8 }, rw[5] = { w0, w1, w2, cw2, cw2 };
  for (int i = 0; i < 5; i++) Btn(K_CHIP, cals[i], rx[i], i < 3 ? y : y + 40, rw[i], 34, ID_CAL0 + i);
  y += 82;
  HWND path = Field(x, y, W - 112, 34, ID_CAL_PATH, T(L"Programa o dirección web", L"Program or web address"));
  SetWindowTextW(path, g_cfg.calendarPath.c_str());
  Btn(K_SECONDARY, T(L"Examinar…", L"Browse…"), x + W - 100, y, 100, 34, ID_CAL_BROWSE);

  // ---- general
  y = s_bodyY + 168 + 30;
  Btn(K_TOGGLE, T(L"Iniciar con Windows", L"Start with Windows"), x, y, W, 32, ID_STARTUP);
  Btn(K_TOGGLE, T(L"Esconder la isla mientras uso una app", L"Hide the island while using an app"), x, y + 36, W, 32, ID_AUTOHIDE);
  Btn(K_TOGGLE, T(L"Ocultar en juegos y videos a pantalla completa", L"Hide over full-screen games and videos"), x, y + 72, W, 32, ID_FULLSCREEN);

  // ---- botones
  Btn(K_SECONDARY, T(L"Cancelar", L"Cancel"), s_W - M - 260, s_H - 58, 120, 40, IDCANCEL);
  Btn(K_PRIMARY, welcome ? T(L"Empezar", L"Get started") : T(L"Guardar", L"Save"), s_W - M - 130, s_H - 58, 130, 40, ID_SAVE);

  SetCalendar(s_cal);
  ShowWindow(s_wnd, SW_SHOWNORMAL);
  SetForegroundWindow(s_wnd);
  SetFocus(ed);
}
