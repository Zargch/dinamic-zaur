// Dinamic Zaur - isla dinamica para Windows 10 y 11 (32 y 64 bits).
// Todo el render es Direct2D por software sobre un unico render target compartido,
// para no cargar el driver de la GPU y mantener la RAM al minimo.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <algorithm>
#include <memory>
#include <mutex>
#include <cmath>

// ---------------------------------------------------------------- datos de la app
#define DZ_NAME L"Dinamic Zaur"
#define DZ_VERSION L"1.0.0"
#define DZ_AUTHOR L"Zargch"
#define DZ_REPO_OWNER L"Zargch"
#define DZ_REPO_NAME L"dinamic-zaur"
#define DZ_REPO_URL L"https://github.com/Zargch/dinamic-zaur"
#define DZ_USER_AGENT L"DinamicZaur/1.0.0 (+https://github.com/Zargch/dinamic-zaur)"

// ---------------------------------------------------------------- mensajes propios
enum : UINT {
  WM_APP_RENDER = WM_APP + 1,   // render diferido de una superficie
  WM_APP_ICONREADY,             // un icono termino de cargarse en segundo plano
  WM_APP_TRAY,                  // icono de bandeja
  WM_APP_WEATHER,               // datos del clima listos
  WM_APP_MEDIA,                 // cambio en la musica que suena
  WM_APP_VOLUME,                // cambio de volumen (wParam = 0..100, lParam = silencio)
  WM_APP_UPDATE,                // termino la comprobacion de actualizaciones
};

// ---------------------------------------------------------------- globales
extern HINSTANCE g_inst;
extern ID2D1Factory* g_d2d;
extern IDWriteFactory* g_dw;
extern IWICImagingFactory* g_wic;
extern ID2D1DCRenderTarget* g_rt;
extern ID2D1DeviceContext* g_dc;  // misma superficie que g_rt; da filtros de alta calidad y capas
extern float g_scale;           // px por DIP
extern HWND g_msgWnd;           // ventana oculta para mensajes globales
extern std::wstring g_dir;      // carpeta del exe
extern std::wstring g_dataDir;  // %APPDATA%\DinamicZaur (configuracion)
extern bool g_es;               // interfaz en español (si no, en ingles)
bool IsWin11();

// texto en el idioma de Windows: español o ingles
inline const wchar_t* T(const wchar_t* es, const wchar_t* en) { return g_es ? es : en; }

struct Theme {
  bool dark;
  D2D1_COLOR_F text, textDim, tint, rim, accent, hover, sep, pill;
};
extern Theme g_th;
void LoadTheme();

// calendario que se abre desde la isla
enum CalendarKind { CAL_WINDOWS = 0, CAL_GOOGLE, CAL_OUTLOOK_WEB, CAL_CUSTOM, CAL_NONE };

struct Config {
  std::wstring font = L"Segoe UI Variable Text";
  std::wstring fontDisplay = L"Segoe UI Variable Display";
  bool autohide = true;          // la isla se esconde mientras se usa una app (aparece con el raton arriba)
  bool hideOnFullscreen = true;  // nunca encima de juegos ni videos a pantalla completa
  int calendar = CAL_WINDOWS;
  std::wstring calendarPath;     // para CAL_CUSTOM
  bool fahrenheit = false;
  std::wstring weatherCity;      // nombre visible de la ubicacion
  double lat = 0, lon = 0; bool hasLoc = false;
  bool firstRunDone = false;
};
extern Config g_cfg;
void LoadConfig();
void SaveConfig();

// ---------------------------------------------------------------- utilidades
inline D2D1_COLOR_F RGBA(float r, float g, float b, float a = 1.f) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }
inline D2D1_COLOR_F WithA(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
inline int Px(float dip) { return (int)lroundf(dip * g_scale); }
inline float Dip(int px) { return px / g_scale; }
inline float Clamp(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
std::wstring Lower(std::wstring s);
bool IContains(const std::wstring& hay, const std::wstring& needle);
RECT MonitorRect();             // monitor principal, px
void RunShell(const std::wstring& target, const std::wstring& args = L"", const wchar_t* verb = L"open");
void SendKeys(std::initializer_list<WORD> keys);  // pulsa en orden y suelta en orden inverso
void TrimMemory();
bool IsOwnWindow(HWND h);
std::wstring FormatDate(const wchar_t* fmt, const SYSTEMTIME& st);
std::wstring KnownPath(REFKNOWNFOLDERID id);
std::wstring DownloadsPath();
bool ConfirmBox(const wchar_t* text);
void OpenFolder(const std::wstring& path);   // abre una carpeta en el Explorador
void ShowInFolder(const std::wstring& path); // abre el Explorador con el archivo seleccionado
std::string HttpGet(const wchar_t* host, const std::wstring& path, DWORD* status = nullptr);
std::wstring Utf8(const std::string& s);
std::wstring UrlEnc(const std::wstring& s);
// JSON minimo (las respuestas de los servicios son planas y conocidas)
size_t JFind(const std::string& s, const char* key, size_t from = 0);
double JNum(const std::string& s, const char* key, size_t from = 0);
std::wstring JStr(const std::string& s, const char* key, size_t from = 0);

// ---------------------------------------------------------------- dibujo
IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, bool display = false);
ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
float TextWidth(const std::wstring& s, IDWriteTextFormat* f);
enum Align { AL_LEFT, AL_CENTER, AL_RIGHT };
void DrawText(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c,
              Align a = AL_LEFT, bool vcenter = true);
void FillRound(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, D2D1_COLOR_F c);
void StrokeRound(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1.f);
void DrawGlass(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float tintBoost = 0.f);  // tinte + borde de luz
void DrawShadow(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float size, float alpha);
void ApplyGlass(HWND h, bool on);
ID2D1PathGeometry* MakePath(std::function<void(ID2D1GeometrySink*)> build, bool filled = true);
void DrawSymbol(ID2D1RenderTarget* rt, const wchar_t* glyph, D2D1_RECT_F r, float size, D2D1_COLOR_F c);
void DrawBitmapHQ(ID2D1RenderTarget* rt, ID2D1Bitmap* b, D2D1_RECT_F dst, float alpha = 1.f, const D2D1_RECT_F* src = nullptr);
void PushRoundClip(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad);
void PopClip(ID2D1RenderTarget* rt);
void DrawFolderIcon(ID2D1RenderTarget* rt, D2D1_RECT_F r, float alpha = 1.f, const wchar_t* glyph = nullptr);
void DrawAppLogo(ID2D1RenderTarget* rt, D2D1_RECT_F r, float alpha = 1.f);  // logo de Dinamic Zaur

// ---------------------------------------------------------------- superficies
// Una superficie es una ventana popup. Con "glass" usa el desenfoque de DWM detras del
// contenido; sin glass es opaca o una ventana layered con alfa por pixel.
class Surface {
public:
  HWND hwnd = nullptr;
  int x = 0, y = 0, w = 0, h = 0;   // px
  bool layered = false;
  float radius = 0;                 // DIPs; >0 recorta la ventana con esquinas redondeadas
  bool dwmRound = false;            // usa las esquinas redondeadas (con antialias y sombra) de Windows 11
  virtual ~Surface();
  void Create(DWORD exStyle, bool layered, bool glass);
  void SetBounds(int x, int y, int w, int h, HWND after = nullptr);
  void Invalidate();
  void Show(bool show, bool activate = false);
  bool Visible() const { return hwnd && IsWindowVisible(hwnd); }
  float Wd() const { return Dip(w); }
  float Hd() const { return Dip(h); }
  virtual void Paint(ID2D1RenderTarget* rt) = 0;
  virtual LRESULT Handle(UINT m, WPARAM wp, LPARAM lp);
  void RenderNow();
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
protected:
  bool pending = false;
  HDC memDC = nullptr; HBITMAP dib = nullptr; HGDIOBJ oldBmp = nullptr; int dibW = 0, dibH = 0;
  void ApplyShape();
};

// ---------------------------------------------------------------- ventanas y apps
struct RunningWin { HWND hwnd; std::wstring key, exe, aumid, title; };
std::vector<RunningWin> EnumRunning();
std::wstring ExeOfWindow(HWND h);
std::wstring AumidOfWindow(HWND h);
void ActivateWindow(HWND h);
std::wstring AppDisplayName(const std::wstring& aumid);  // nombre visible de una app por su AppUserModelID

// iconos: se cargan en un hilo aparte; devuelve nullptr mientras no estan listos
ID2D1Bitmap* GetIcon(const std::wstring& shellPath, const std::wstring& name, int px, bool* custom = nullptr);
void ReleaseIcons(int px);
DWORD DragFilesOut(const std::vector<std::wstring>& paths, DWORD allowed);

// ---------------------------------------------------------------- menus emergentes
struct MenuItem {
  std::wstring text, shortcut;
  std::function<void()> action;
  bool sep = false, disabled = false, checked = false, header = false;
  static MenuItem Sep() { MenuItem m; m.sep = true; return m; }
};
void ShowMenu(std::vector<MenuItem> items, int x, int y, bool above = false, std::function<void()> onClose = nullptr);
void CloseMenu();
bool MenuOpen();
HWND MenuHwnd();
void Menu_OnMessage(WPARAM what);

// ---------------------------------------------------------------- musica, clima e isla dinamica
struct MediaSnap {
  bool has = false, playing = false, trackChanged = false;
  std::wstring title, artist, app, aumid;  // aumid: app de origen (Spotify.exe, Chrome, una PWA...)
  double pos = 0, dur = 0;
  DWORD posTick = 0;
  std::vector<BYTE> art;  // caratula PBGRA artPx x artPx
  int artPx = 0, version = 0;
  D2D1_COLOR_F color{ .5f, .5f, .5f, 1 };
};
void Media_Start();
MediaSnap Media_Get();
void Media_FocusSource();  // trae al frente la app o pestaña del navegador que esta sonando

struct WeatherInfo { bool ok = false; std::wstring city, err; double temp = 0, tmax = 0, tmin = 0; int code = 0; bool day = true; };
bool Weather_Info(WeatherInfo& out);
void Weather_Fetch();
void Weather_Icon(ID2D1RenderTarget* rt, int code, bool day, float cx, float cy, float s);
const wchar_t* Weather_Text(int code);
std::wstring Weather_Temp(double celsius, bool unit = false);  // en °C o °F segun la configuracion
struct Place { std::wstring name, label; double lat, lon; };
std::vector<Place> Weather_Search(const std::wstring& query);  // geocodificacion (OpenStreetMap)
bool Weather_Locate(Place& out);                              // ubicacion de Windows + nombre del lugar

void Settings_Open(bool welcome = false);
bool Settings_IsOpen();
void OpenCalendar();

void Island_Create();
void Island_Show(bool show);
void Island_OnMedia();
void Island_OnVolume(int level, bool muted);
void Island_OnUpdate();
void Island_Invalidate();
bool Island_Busy();
void Island_Relayout();
void TopBars_Refresh();
extern HWND g_fg;   // ultima ventana de otra app que tuvo el foco

// comprobacion de actualizaciones (GitHub Releases)
struct UpdateInfo { int state = 0; std::wstring latest, url; };  // 0 sin comprobar, 1 comprobando, 2 al dia, 3 hay nueva, 4 error
UpdateInfo Update_Get();
void Update_Check();

// inicio con Windows (HKCU\...\Run) e icono de la app
bool Startup_Enabled();
void Startup_Set(bool on);
HICON App_Icon(int px);
HWND Settings_Hwnd();
void App_SettingsChanged();  // la ventana de configuracion guardo cambios
