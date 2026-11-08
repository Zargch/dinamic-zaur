#include "common.h"
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <powrprof.h>
#include <audiopolicy.h>
#include <thread>

// Isla dinamica de vidrio arriba al centro. Se transforma (animacion de resorte) entre reposo,
// musica, avisos (volumen, Bloq Mayus, cancion), zona para soltar archivos y un panel expandido
// con dos pestañas: "Panel" (musica, clima, calendario, sistema, energia) y "Bandeja" (archivos
// guardados temporalmente: copiar, comprimir, copiar/mover a, arrastrar fuera).

// ---------------------------------------------------------------- aviso de volumen
class VolumeCallback : public IAudioEndpointVolumeCallback {
  LONG ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
    if (id == IID_IUnknown || id == __uuidof(IAudioEndpointVolumeCallback)) { *p = this; AddRef(); return S_OK; }
    *p = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
    PostMessageW(g_msgWnd, WM_APP_VOLUME, (WPARAM)lroundf(d->fMasterVolume * 100), d->bMuted);
    return S_OK;
  }
};

static IAudioEndpointVolume* s_vol = nullptr;
static VolumeCallback* s_volCb = nullptr;
static IAudioSessionManager2* s_sessMgr = nullptr;
static std::wstring s_devId, s_devName;

static void UnhookVolume() {
  if (s_vol) { if (s_volCb) s_vol->UnregisterControlChangeNotify(s_volCb); s_vol->Release(); s_vol = nullptr; }
  if (s_volCb) { s_volCb->Release(); s_volCb = nullptr; }
  if (s_sessMgr) { s_sessMgr->Release(); s_sessMgr = nullptr; }
}

// engancha la salida de audio predeterminada; si cambia (otros auriculares, HDMI...) se vuelve a enganchar
static void HookVolume() {
  static const PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* dev = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
  if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
    LPWSTR id = nullptr;
    std::wstring sid;
    if (SUCCEEDED(dev->GetId(&id))) { sid = id; CoTaskMemFree(id); }
    if (!s_vol || sid != s_devId) {
      UnhookVolume();
      s_devId = sid;
      if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&s_vol))) {
        s_volCb = new VolumeCallback;
        s_vol->RegisterControlChangeNotify(s_volCb);
      }
      dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&s_sessMgr);
      s_devName.clear();
      IPropertyStore* ps = nullptr;
      if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) s_devName = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
      }
    }
    dev->Release();
  }
  en->Release();
}

// ---------------------------------------------------------------- utilidades de dibujo
static ID2D1StrokeStyle* RoundCap() {
  static ID2D1StrokeStyle* s = nullptr;
  if (!s) g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &s);
  return s;
}

static void Ring(ID2D1RenderTarget* rt, float cx, float cy, float r, float w, float frac, D2D1_COLOR_F c) {
  rt->DrawEllipse(D2D1::Ellipse({ cx, cy }, r, r), Brush(WithA(c, .25f)), w);
  frac = Clamp(frac, 0, 1);
  if (frac <= .001f) return;
  if (frac >= .999f) { rt->DrawEllipse(D2D1::Ellipse({ cx, cy }, r, r), Brush(c), w); return; }
  float a = frac * 6.28318f;
  auto p = MakePath([&](ID2D1GeometrySink* s) {
    s->BeginFigure({ cx, cy - r }, D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddArc(D2D1::ArcSegment({ cx + r * sinf(a), cy - r * cosf(a) }, { r, r }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                               frac > .5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
  }, false);
  rt->DrawGeometry(p, Brush(c), w, RoundCap());
  p->Release();
}

static std::wstring Clock(double s) {
  if (s < 0 || s > 360000) s = 0;
  int t = (int)s;
  wchar_t b[16];
  swprintf_s(b, L"%d:%02d", t / 60, t % 60);
  return b;
}

static std::wstring Speed(double bps) {
  wchar_t b[32];
  if (bps >= 1024 * 1024) swprintf_s(b, L"%.1f MB/s", bps / 1048576);
  else swprintf_s(b, L"%.0f KB/s", bps / 1024);
  return b;
}

static std::wstring BaseName(const std::wstring& p) {
  std::wstring s = p;
  while (!s.empty() && s.back() == L'\\') s.pop_back();
  size_t k = s.find_last_of(L'\\');
  return k == std::wstring::npos ? s : s.substr(k + 1);
}

static bool IsDir(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---------------------------------------------------------------- arrastrar y soltar
class Island;
class IslandDrop : public IDropTarget {
  LONG ref = 1;
public:
  Island* isl;
  explicit IslandDrop(Island* i) : isl(i) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
    if (id == IID_IUnknown || id == IID_IDropTarget) { *p = this; AddRef(); return S_OK; }
    *p = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override;
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override;
  HRESULT STDMETHODCALLTYPE DragLeave() override;
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) override;
  bool ok = false;
};

class DropSource : public IDropSource {
  LONG ref = 1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
    if (id == IID_IUnknown || id == IID_IDropSource) { *p = this; AddRef(); return S_OK; }
    *p = nullptr; return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
  ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
    if (esc) return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

static IDataObject* FilesDataObject(const std::vector<std::wstring>& paths) {
  std::vector<PIDLIST_ABSOLUTE> ids;
  for (auto& p : paths) { PIDLIST_ABSOLUTE id = nullptr; if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &id, 0, nullptr))) ids.push_back(id); }
  IDataObject* dob = nullptr;
  if (!ids.empty()) {
    IShellItemArray* arr = nullptr;
    if (SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &arr))) {
      arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dob));
      arr->Release();
    }
  }
  for (auto id : ids) CoTaskMemFree(id);
  return dob;
}

// arrastra archivos fuera de la bandeja de la isla: devuelve el efecto aplicado
DWORD DragFilesOut(const std::vector<std::wstring>& paths, DWORD allowed) {
  IDataObject* d = FilesDataObject(paths);
  if (!d) return DROPEFFECT_NONE;
  auto src = new DropSource;
  DWORD eff = 0;
  if (DoDragDrop(d, src, allowed, &eff) != DRAGDROP_S_DROP) eff = DROPEFFECT_NONE;
  src->Release();
  d->Release();
  return eff;
}

static void FilesToClipboard(HWND h, const std::vector<std::wstring>& paths) {
  size_t chars = 1;
  for (auto& p : paths) chars += p.size() + 1;
  HGLOBAL g = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * 2);
  auto df = (DROPFILES*)GlobalLock(g);
  df->pFiles = sizeof(DROPFILES); df->fWide = TRUE;
  wchar_t* w = (wchar_t*)(df + 1);
  for (auto& p : paths) { memcpy(w, p.c_str(), p.size() * 2); w += p.size() + 1; }
  GlobalUnlock(g);
  HGLOBAL e = GlobalAlloc(GHND, sizeof(DWORD));
  *(DWORD*)GlobalLock(e) = DROPEFFECT_COPY;
  GlobalUnlock(e);
  if (OpenClipboard(h)) {
    EmptyClipboard();
    SetClipboardData(CF_HDROP, g);
    SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), e);
    CloseClipboard();
  }
}

static std::wstring PickFolder(const wchar_t* title) {
  IFileOpenDialog* d = nullptr;
  std::wstring r;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return r;
  DWORD o; d->GetOptions(&o);
  d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  d->SetTitle(title);
  if (SUCCEEDED(d->Show(nullptr))) {
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->GetResult(&it))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r = p; CoTaskMemFree(p); }
      it->Release();
    }
  }
  d->Release();
  return r;
}

static bool ShellCopy(const std::vector<std::wstring>& paths, const std::wstring& dst, bool move) {
  std::wstring from;
  for (auto& p : paths) { from += p; from.push_back(0); }
  from.push_back(0);
  std::wstring to = dst; to.push_back(0); to.push_back(0);
  SHFILEOPSTRUCTW op{};
  op.wFunc = move ? FO_MOVE : FO_COPY;
  op.pFrom = from.c_str(); op.pTo = to.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// ---------------------------------------------------------------- perfil de volumen
// Mezclador por app dentro de la isla (sesiones de audio de Windows) y accesos al sonido de Windows.

struct AudioApp {
  std::wstring key, name, exe;  // exe vacio = sonidos del sistema
  std::vector<ISimpleAudioVolume*> vols;  // una app puede tener varias sesiones (Chrome, Discord...)
  float vol = 1;
  bool mute = false;
};

static std::wstring ExeDescription(const std::wstring& exe) {
  std::wstring r;
  DWORD hnd = 0, sz = GetFileVersionInfoSizeW(exe.c_str(), &hnd);
  if (sz) {
    std::vector<BYTE> b(sz);
    struct LC { WORD l, c; }* lc = nullptr;
    UINT n = 0;
    if (GetFileVersionInfoW(exe.c_str(), 0, sz, b.data()) && VerQueryValueW(b.data(), L"\\VarFileInfo\\Translation", (void**)&lc, &n) && n >= sizeof(LC)) {
      wchar_t q[64];
      swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", lc->l, lc->c);
      wchar_t* v = nullptr; UINT vn = 0;
      if (VerQueryValueW(b.data(), q, (void**)&v, &vn) && vn > 1) r = v;
    }
  }
  while (!r.empty() && r.back() == L' ') r.pop_back();
  if (r.empty()) {
    r = BaseName(exe);
    size_t d = r.find_last_of(L'.');
    if (d != std::wstring::npos) r = r.substr(0, d);
    if (!r.empty()) r[0] = towupper(r[0]);
  }
  return r;
}

static std::wstring ProcessExe(DWORD pid) {
  std::wstring r;
  if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t p[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(ph, 0, p, &n)) r = p;
    CloseHandle(ph);
  }
  return r;
}

static std::vector<AudioApp> ReadSessions() {
  std::vector<AudioApp> out;
  if (!s_sessMgr) return out;
  IAudioSessionEnumerator* se = nullptr;
  if (FAILED(s_sessMgr->GetSessionEnumerator(&se))) return out;
  static std::map<std::wstring, std::wstring> names;  // exe -> nombre (leer la version cada segundo no hace falta)
  int n = 0;
  se->GetCount(&n);
  for (int i = 0; i < n; i++) {
    IAudioSessionControl* c = nullptr;
    if (FAILED(se->GetSession(i, &c))) continue;
    IAudioSessionControl2* c2 = nullptr;
    AudioSessionState st;
    if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && SUCCEEDED(c2->GetState(&st)) && st != AudioSessionStateExpired) {
      bool sys = c2->IsSystemSoundsSession() == S_OK;
      DWORD pid = 0;
      c2->GetProcessId(&pid);
      std::wstring exe = sys ? L"" : ProcessExe(pid), key = sys ? L"#system" : Lower(exe);
      ISimpleAudioVolume* sv = nullptr;
      if ((sys || (!exe.empty() && pid != GetCurrentProcessId())) && SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&sv)))) {
        auto it = std::find_if(out.begin(), out.end(), [&](const AudioApp& a) { return a.key == key; });
        if (it == out.end()) {
          AudioApp a;
          a.key = key; a.exe = exe;
          if (sys) a.name = T(L"Sonidos del sistema", L"System sounds");
          else { auto f = names.find(key); a.name = f != names.end() ? f->second : (names[key] = ExeDescription(exe)); }
          sv->GetMasterVolume(&a.vol);
          BOOL m = FALSE; sv->GetMute(&m); a.mute = m != FALSE;
          out.push_back(std::move(a));
          it = out.end() - 1;
        }
        it->vols.push_back(sv);
      }
    }
    if (c2) c2->Release();
    c->Release();
  }
  se->Release();
  std::stable_partition(out.begin(), out.end(), [](const AudioApp& a) { return !a.exe.empty(); });  // sistema al final
  return out;
}

// ---------------------------------------------------------------- isla
enum IMode { IM_IDLE, IM_PLAY, IM_PEEK, IM_ACT, IM_DROP, IM_EXP };
enum IAct { IA_NONE, IA_VOLUME, IA_CAPS, IA_NUM, IA_TRACK, IA_SHELF, IA_ZIP, IA_INFO };
struct IBtn { D2D1_RECT_F r; std::function<void()> act; bool keepOpen; };
static const UINT WM_ISLAND_ZIP = WM_APP + 70;

class Island : public Surface {
public:
  static constexpr float TOP = 8;   // franja oculta sobre el borde (esconde el redondeo superior)
  float cw = 230, ch = 32, vw = 0, vh = 0, tw = 230, th = 32;
  bool hover = false, expanded = false, animating = false, dragOver = false;
  int tab = 0;  // 0 panel, 1 bandeja, 2 volumen, 3 acerca de (oculta: 5 clics en la tarjeta del sistema)
  int secretClicks = 0;
  DWORD secretTick = 0;
  struct ISlider { D2D1_RECT_F r; std::function<void(float)> set; };
  std::vector<ISlider> sliders;
  int dragSlider = -1;
  std::vector<AudioApp> apps;
  float master = 0;
  bool masterMute = false;
  int act = IA_NONE, actValue = 0;
  bool actFlag = false;
  std::wstring actText;
  DWORD actUntil = 0;
  MediaSnap media;
  ID2D1Bitmap* art = nullptr;
  int artVersion = -1;
  std::vector<IBtn> btns;
  int hotBtn = -1;
  bool capsOn = false, numOn = false, capsDown = false, numDown = false;
  std::vector<std::wstring> shelf;
  std::vector<D2D1_RECT_F> shelfRects;
  int pressItem = -1;
  POINT pressPt{};
  float cpu = 0;
  ULONGLONG lastIdle = 0, lastTotal = 0, lastIn = 0, lastOut = 0;
  DWORD lastNet = 0;
  double netIn = 0, netOut = 0;

  IMode Mode() const {
    if (dragOver) return IM_DROP;
    if (expanded) return IM_EXP;
    if (act != IA_NONE && GetTickCount() < actUntil) return IM_ACT;
    if (hover) return IM_PEEK;
    if (media.has && media.playing) return IM_PLAY;
    return IM_IDLE;
  }

  void TargetSize() {
    switch (Mode()) {
    case IM_EXP: tw = 720; th = 268; break;
    case IM_DROP: tw = 460; th = 84; break;
    case IM_PEEK: tw = 460; th = 70; break;
    case IM_PLAY: tw = 310; th = 32; break;
    case IM_ACT: tw = act == IA_TRACK ? 460.f : act == IA_VOLUME ? 360.f : act == IA_INFO ? 420.f : 340.f; th = act == IA_TRACK ? 70.f : 38.f; break;
    default: tw = shelf.empty() ? 230.f : 270.f; th = 32; break;
    }
  }

  // deslizamiento desde el borde superior (0 = visible, 1 = oculta)
  float slide = 0, slideFrom = 0;
  bool hiding = false;
  DWORD slideStart = 0;

  void Place() {
    RECT mon = MonitorRect();
    int ww = Px(cw), hh = Px(ch + TOP);
    int off = (int)lroundf(slide * Px(ch + 10));
    SetBounds((mon.left + mon.right - ww) / 2, mon.top - Px(TOP) - off, ww, hh, nullptr);
  }

  void SlideTo(bool hide) {
    if (hide == hiding) return;
    hiding = hide;
    if (hide) KillTimer(hwnd, 2);
    else if (Mode() == IM_PLAY) SetTimer(hwnd, 2, 80, nullptr);
    slideFrom = slide;
    slideStart = GetTickCount();
    if (!hide && !Visible()) {
      Place();
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetTimer(hwnd, 7, 15, nullptr);
  }

  void SlideStep() {
    float t = Clamp((GetTickCount() - slideStart) / (hiding ? 180.f : 260.f), 0, 1);
    float e = hiding ? t * t * t : 1 - (1 - t) * (1 - t) * (1 - t);
    slide = Lerp(slideFrom, hiding ? 1.f : 0.f, e);
    Place();
    if (t >= 1) {
      KillTimer(hwnd, 7);
      if (hiding) ShowWindow(hwnd, SW_HIDE);
    }
  }

  void Retarget() {
    TargetSize();
    IMode m = Mode();
    // ecualizador animado solo si se ve: escondida (app en uso, juego) no despierta al programa
    if (m == IM_PLAY && !hiding) SetTimer(hwnd, 2, 80, nullptr); else KillTimer(hwnd, 2);
    if (m == IM_EXP || m == IM_PEEK) { Stats(); SetTimer(hwnd, 3, 1000, nullptr); } else SetTimer(hwnd, 3, 60000, nullptr);
    if (!animating) { animating = true; SetTimer(hwnd, 1, 15, nullptr); }
    Invalidate();
  }

  void Step() {
    const float k = .2f, damp = .62f;
    vw = (vw + (tw - cw) * k) * damp; cw += vw;
    vh = (vh + (th - ch) * k) * damp; ch += vh;
    if (fabsf(tw - cw) < .3f && fabsf(vw) < .3f && fabsf(th - ch) < .3f && fabsf(vh) < .3f) {
      cw = tw; ch = th; vw = vh = 0;
      animating = false;
      KillTimer(hwnd, 1);
    }
    Place();
  }

  float ContentAlpha() const { return Clamp(1.f - fabsf(tw - cw) / 70.f - fabsf(th - ch) / 30.f, 0, 1); }

  // colores que siguen el tema (igual que la barra de menu)
  // la isla es negra (como la del iPhone): el contenido siempre va en blanco, con tema claro u oscuro
  D2D1_COLOR_F W(float a, float k = 1) const { return RGBA(255, 255, 255, a * k); }
  D2D1_COLOR_F Soft(float a, float k) const { return RGBA(255, 255, 255, a * k); }

  // ------------------------------------------------------------ dibujo
  void Paint(ID2D1RenderTarget* rt) override {
    btns.clear();
    shelfRects.clear();
    sliders.clear();
    rt->Clear(D2D1::ColorF(0, 0, 0, 1));  // fondo negro solido: sin desenfoque de DWM ni degradados por cuadro
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, TOP) * old);
    float a = ContentAlpha();
    if (a > .01f) {
      switch (Mode()) {
      case IM_IDLE: DrawIdle(rt, a); break;
      case IM_PLAY: DrawPlay(rt, a); break;
      case IM_PEEK: if (media.has) DrawTrack(rt, a); else DrawDay(rt, a); break;
      case IM_ACT: DrawActivity(rt, a); break;
      case IM_DROP: DrawDrop(rt, a); break;
      case IM_EXP: DrawExpanded(rt, a); break;
      }
    }
    rt->SetTransform(old);
  }

  void DrawArt(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float a) {
    if (art) { PushRoundClip(rt, r, rad); DrawBitmapHQ(rt, art, r, a); PopClip(rt); }
    else {
      FillRound(rt, r, rad, Soft(a, .12f));
      DrawSymbol(rt, L"\uE8D6", r, (r.right - r.left) * .42f, W(a, .7f));
    }
  }

  // los rectangulos de botones se guardan en coordenadas de ventana (con la franja TOP)
  void AddBtn(D2D1_RECT_F r, std::function<void()> f, bool keep) { btns.push_back({ { r.left, r.top + TOP, r.right, r.bottom + TOP }, f, keep }); }

  void Btn(ID2D1RenderTarget* rt, D2D1_RECT_F r, const wchar_t* glyph, float size, float a, std::function<void()> f, bool keep = true, bool circle = false) {
    bool hot = hotBtn == (int)btns.size();
    if (circle) rt->FillEllipse(D2D1::Ellipse({ (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, (r.right - r.left) / 2, (r.bottom - r.top) / 2),
                                Brush(Soft(a, hot ? .24f : .12f)));
    else if (hot) FillRound(rt, r, (r.bottom - r.top) / 2, Soft(a, .14f));
    DrawSymbol(rt, glyph, r, size, W(a));
    AddBtn(r, f, keep);
  }

  void Pill(ID2D1RenderTarget* rt, D2D1_RECT_F r, const wchar_t* glyph, const std::wstring& text, float a, std::function<void()> f, bool keep = true, bool on = false) {
    bool hot = hotBtn == (int)btns.size();
    FillRound(rt, r, (r.bottom - r.top) / 2, on ? WithA(g_th.accent, a) : Soft(a, hot ? .2f : .1f));
    D2D1_COLOR_F c = on ? RGBA(255, 255, 255, a) : W(a);
    float tx = r.left + 12;
    if (glyph) { DrawSymbol(rt, glyph, { r.left + 6, r.top, r.left + 28, r.bottom }, 12, c); tx = r.left + 28; }
    DrawText(rt, text, Font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), { tx, r.top, r.right - 8, r.bottom }, c, glyph ? AL_LEFT : AL_CENTER);
    AddBtn(r, f, keep);
  }

  void DrawIdle(ID2D1RenderTarget* rt, float a) {
    WeatherInfo w;
    if (Weather_Info(w)) {
      Weather_Icon(rt, w.code, w.day, 24, ch / 2, 17);
      DrawText(rt, Weather_Temp(w.temp), Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), { 38, 0, 100, ch }, W(a));
    } else DrawSymbol(rt, L"\uE706", { 12, 0, 36, ch }, 13, W(a, .8f));
    float right = cw - 14;
    if (!shelf.empty()) {  // insignia de la bandeja
      std::wstring n = std::to_wstring(shelf.size());
      FillRound(rt, { right - 44, ch / 2 - 10, right, ch / 2 + 10 }, 10, WithA(g_th.accent, a));
      DrawSymbol(rt, L"\uE8B7", { right - 42, ch / 2 - 10, right - 24, ch / 2 + 10 }, 10, RGBA(255, 255, 255, a));
      DrawText(rt, n, Font(11.5f, DWRITE_FONT_WEIGHT_BOLD), { right - 24, ch / 2 - 10, right - 4, ch / 2 + 10 }, RGBA(255, 255, 255, a), AL_CENTER);
      right -= 52;
    }
    SYSTEMTIME st; GetLocalTime(&st);
    std::wstring d = FormatDate(L"ddd d MMM", st);
    d.erase(std::remove(d.begin(), d.end(), L'.'), d.end());
    DrawText(rt, d, Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), { 90, 0, right, ch }, W(a), AL_RIGHT);
  }

  void DrawBars(ID2D1RenderTarget* rt, float x, float cy, float a, float scale = 1) {
    D2D1_COLOR_F c = media.color;
    c = D2D1::ColorF(Lerp(c.r, 1, .35f), Lerp(c.g, 1, .35f), Lerp(c.b, 1, .35f), a);
    float t = GetTickCount() / 1000.f;
    float ph[5] = { 0, 1.7f, .6f, 2.4f, 1.1f }, sp[5] = { 7.3f, 9.1f, 6.2f, 8.4f, 10.2f };
    for (int i = 0; i < 5; i++) {
      float hgt = (3 + 9 * fabsf(sinf(t * sp[i] * .5f + ph[i]))) * scale;
      if (!media.playing) hgt = 3 * scale;
      rt->DrawLine({ x + i * 5 * scale, cy - hgt / 2 }, { x + i * 5 * scale, cy + hgt / 2 }, Brush(c), 2.6f * scale, RoundCap());
    }
  }

  void DrawPlay(ID2D1RenderTarget* rt, float a) {
    DrawArt(rt, { 10, 5, 32, 27 }, 6, a);
    DrawBars(rt, cw - 38, ch / 2, a);
  }

  void DrawTrack(ID2D1RenderTarget* rt, float a) {
    DrawArt(rt, { 12, 10, 62, 60 }, 12, a);
    AddBtn({ 12, 10, cw - 110, 60 }, [] { Media_FocusSource(); }, false);  // caratula y titulo: ir a la app que suena
    DrawText(rt, media.title.empty() ? T(L"Sin título", L"Untitled") : media.title, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), { 74, 12, cw - 110, 34 }, W(a));
    DrawText(rt, media.artist.empty() ? media.app : media.artist, Font(12.5f), { 74, 34, cw - 110, 54 }, W(a, .6f));
    Btn(rt, { cw - 98, 19, cw - 66, 51 }, L"\uE892", 13, a, [] { SendKeys({ VK_MEDIA_PREV_TRACK }); });
    Btn(rt, { cw - 66, 17, cw - 30, 53 }, media.playing ? L"\uE769" : L"\uE768", 17, a, [] { SendKeys({ VK_MEDIA_PLAY_PAUSE }); });
    if (media.playing) DrawBars(rt, cw - 22, 35, a, .7f);
  }

  void DrawDay(ID2D1RenderTarget* rt, float a) {
    SYSTEMTIME st; GetLocalTime(&st);
    std::wstring d = FormatDate(g_es ? L"dddd, d 'de' MMMM" : L"dddd, MMMM d", st);
    if (!d.empty()) d[0] = towupper(d[0]);
    DrawText(rt, d, Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), { 18, 12, cw - 150, 36 }, W(a));
    WeatherInfo w;
    std::wstring sub = T(L"Haz clic para abrir el panel", L"Click to open the panel");
    if (Weather_Info(w)) sub = w.city.substr(0, w.city.find(L',')) + L" · " + Weather_Temp(w.temp) + L" · " + Weather_Text(w.code);
    DrawText(rt, sub, Font(12.5f), { 18, 36, cw - 150, 56 }, W(a, .6f));
    MEMORYSTATUSEX ms{ sizeof ms }; GlobalMemoryStatusEx(&ms);
    Ring(rt, cw - 110, ch / 2, 14, 4, cpu, RGBA(48, 199, 89, a));
    DrawText(rt, L"CPU", Font(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), { cw - 130, ch / 2 - 8, cw - 90, ch / 2 + 8 }, W(a, .8f), AL_CENTER);
    Ring(rt, cw - 50, ch / 2, 14, 4, ms.dwMemoryLoad / 100.f, WithA(g_th.accent, a));
    DrawText(rt, L"RAM", Font(9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), { cw - 70, ch / 2 - 8, cw - 30, ch / 2 + 8 }, W(a, .8f), AL_CENTER);
  }

  void DrawActivity(ID2D1RenderTarget* rt, float a) {
    if (act == IA_TRACK) { DrawTrack(rt, a); return; }
    if (act == IA_VOLUME) {
      const wchar_t* g = actFlag || actValue == 0 ? L"\uE74F" : actValue < 34 ? L"\uE993" : actValue < 67 ? L"\uE994" : L"\uE995";
      DrawSymbol(rt, g, { 12, 0, 40, ch }, 15, W(a));
      float x0 = 50, x1 = cw - 58, cy = ch / 2;
      FillRound(rt, { x0, cy - 3, x1, cy + 3 }, 3, Soft(a, .22f));
      if (!actFlag) FillRound(rt, { x0, cy - 3, x0 + (x1 - x0) * actValue / 100.f, cy + 3 }, 3, W(a));
      DrawText(rt, actFlag ? L"—" : std::to_wstring(actValue), Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), { x1 + 6, 0, cw - 14, ch }, W(a), AL_RIGHT);
      return;
    }
    if (act == IA_SHELF || act == IA_ZIP || act == IA_INFO) {
      DrawSymbol(rt, act == IA_ZIP ? L"\uF012" : act == IA_INFO ? L"\uE946" : L"\uE8B7", { 12, 0, 40, ch }, 14, WithA(g_th.accent, a));
      DrawText(rt, actText, Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), { 44, 0, cw - 14, ch }, W(a));
      return;
    }
    bool on = actFlag;
    DrawText(rt, act == IA_CAPS ? L"\u21EA" : L"\u2460", Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), { 14, 0, 40, ch }, W(a));
    DrawText(rt, act == IA_CAPS ? T(L"Bloq Mayús", L"Caps Lock") : T(L"Bloq Num", L"Num Lock"), Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), { 42, 0, cw - 100, ch }, W(a));
    DrawText(rt, on ? T(L"Activado", L"On") : T(L"Desactivado", L"Off"), Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), { cw - 140, 0, cw - 14, ch },
             on ? RGBA(48, 199, 89, a) : W(a, .55f), AL_RIGHT);
  }

  void DrawDrop(ID2D1RenderTarget* rt, float a) {
    D2D1_RECT_F z{ 10, 8, cw - 10, ch - 8 };
    ID2D1StrokeStyle* dash = nullptr;
    float d[] = { 4, 3 };
    g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                             D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_CUSTOM), d, 2, &dash);
    FillRound(rt, z, 14, WithA(g_th.accent, .12f * a));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(z, 14, 14), Brush(WithA(g_th.accent, .8f * a)), 1.5f, dash);
    if (dash) dash->Release();
    DrawSymbol(rt, L"\uE896", { 24, 8, 64, ch - 8 }, 20, WithA(g_th.accent, a));
    DrawText(rt, T(L"Suelta para guardar en la bandeja", L"Drop to keep in the shelf"), Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), { 70, 14, cw - 20, 40 }, W(a));
    DrawText(rt, T(L"Luego podrás copiarlos, comprimirlos o arrastrarlos a otro lugar", L"Then copy, zip or drag them anywhere"), Font(11.5f), { 70, 38, cw - 20, 60 }, W(a, .6f));
  }

  void Card(ID2D1RenderTarget* rt, D2D1_RECT_F r, float a) { FillRound(rt, r, 18, Soft(a, .075f)); }

  void DrawExpanded(ID2D1RenderTarget* rt, float a) {
    // pestañas
    Pill(rt, { cw / 2 - 180, 8, cw / 2 - 64, 32 }, L"\uE80A", L"Panel", a, [this] { tab = 0; Invalidate(); }, true, tab == 0);
    Pill(rt, { cw / 2 - 58, 8, cw / 2 + 58, 32 }, L"\uE8B7", T(L"Bandeja", L"Shelf") + (shelf.empty() ? L"" : L" (" + std::to_wstring(shelf.size()) + L")"), a,
         [this] { tab = 1; Invalidate(); }, true, tab == 1);
    Pill(rt, { cw / 2 + 64, 8, cw / 2 + 180, 32 }, L"\uE995", T(L"Volumen", L"Volume"), a, [this] { tab = 2; RefreshAudio(); Invalidate(); }, true, tab == 2);
    if (tab == 1) DrawShelf(rt, a); else if (tab == 2) DrawVolume(rt, a); else if (tab == 3) DrawAbout(rt, a); else DrawPanel(rt, a);
  }

  // ------------------------------------------------------------ acerca de (easter egg)
  void SecretClick() {
    DWORD now = GetTickCount();
    secretClicks = now - secretTick < 700 ? secretClicks + 1 : 1;
    secretTick = now;
    if (secretClicks >= 5) { secretClicks = 0; tab = 3; Update_Check(); Invalidate(); }
  }

  void DrawAbout(ID2D1RenderTarget* rt, float a) {
    D2D1_RECT_F c{ 16, 44, cw - 16, 252 };
    Card(rt, c, a);
    DrawAppLogo(rt, { c.left + 28, c.top + 28, c.left + 116, c.top + 116 }, a);
    float x = c.left + 140;
    DrawText(rt, DZ_NAME, Font(26, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), { x, c.top + 22, c.right - 20, c.top + 56 }, W(a));
    DrawText(rt, std::wstring(T(L"Versi\u00F3n ", L"Version ")) + DZ_VERSION, Font(13), { x, c.top + 56, c.right - 20, c.top + 76 }, W(a, .6f));
    DrawText(rt, std::wstring(T(L"Hecho con cari\u00F1o por ", L"Made with love by ")) + DZ_AUTHOR, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), { x, c.top + 84, c.right - 20, c.top + 106 }, W(a));
    DrawText(rt, T(L"C\u00F3digo abierto \u00B7 Licencia MIT", L"Open source \u00B7 MIT License"), Font(12), { x, c.top + 106, c.right - 20, c.top + 124 }, W(a, .55f));
    // estado de actualizacion
    UpdateInfo u = Update_Get();
    const wchar_t* g = L"\uE895";
    std::wstring s = T(L"Buscando actualizaciones\u2026", L"Checking for updates\u2026");
    D2D1_COLOR_F col = W(a, .8f);
    if (u.state == 2) { g = L"\uE73E"; s = T(L"Est\u00E1s al d\u00EDa", L"You're up to date"); col = RGBA(48, 209, 88, a); }
    else if (u.state == 3) { g = L"\uE896"; s = std::wstring(T(L"Hay una versi\u00F3n nueva: ", L"New version available: ")) + u.latest; col = RGBA(255, 159, 10, a); }
    else if (u.state == 4) { g = L"\uE783"; s = T(L"No se pudo comprobar (\u00BFsin internet?)", L"Couldn't check (offline?)"); col = W(a, .6f); }
    D2D1_RECT_F sr{ c.left + 20, c.top + 128, c.right - 20, c.top + 170 };
    FillRound(rt, sr, 14, Soft(a, .06f));
    DrawSymbol(rt, g, { sr.left + 10, sr.top, sr.left + 42, sr.bottom }, 16, col);
    DrawText(rt, s, Font(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), { sr.left + 48, sr.top, sr.right - 200, sr.bottom }, col);
    if (u.state == 3) Pill(rt, { sr.right - 186, sr.top + 7, sr.right - 12, sr.bottom - 7 }, L"\uE896", T(L"Descargar", L"Download"), a, [u] { RunShell(u.url); }, false, true);
    else if (u.state == 4) Pill(rt, { sr.right - 186, sr.top + 7, sr.right - 12, sr.bottom - 7 }, L"\uE72C", T(L"Reintentar", L"Retry"), a, [] { Update_Check(); });
    float by = c.top + 180, bx = c.left + 20;
    Pill(rt, { bx, by - 2, bx + 150, by + 24 }, L"\uE774", T(L"Ver en GitHub", L"View on GitHub"), a, [] { RunShell(DZ_REPO_URL); }, false);
    DrawText(rt, T(L"Clima: MET Norway \u00B7 Lugares: \u00A9 OpenStreetMap", L"Weather: MET Norway \u00B7 Places: \u00A9 OpenStreetMap"), Font(10.5f), { bx + 164, by - 2, c.right - 20, by + 24 }, W(a, .45f), AL_RIGHT);
  }

  // ------------------------------------------------------------ perfil de volumen
  static const wchar_t* VolGlyph(float v, bool mute) {
    return mute || v <= .001f ? L"\uE74F" : v < .34f ? L"\uE993" : v < .67f ? L"\uE994" : L"\uE995";
  }

  void Slider(ID2D1RenderTarget* rt, D2D1_RECT_F r, float v, bool mute, float a, std::function<void(float)> set) {
    float cy = (r.top + r.bottom) / 2, x = r.left + (r.right - r.left) * Clamp(v, 0, 1);
    D2D1_COLOR_F fill = mute ? Soft(a, .35f) : WithA(g_th.accent, a);
    FillRound(rt, { r.left, cy - 3, r.right, cy + 3 }, 3, Soft(a, .2f));
    FillRound(rt, { r.left, cy - 3, std::max(x, r.left + 6), cy + 3 }, 3, fill);
    rt->FillEllipse(D2D1::Ellipse({ x, cy }, 8, 8), Brush(RGBA(0, 0, 0, .18f * a)));
    rt->FillEllipse(D2D1::Ellipse({ x, cy }, 7, 7), Brush(RGBA(255, 255, 255, a)));
    sliders.push_back({ { r.left - 8, r.top + TOP - 6, r.right + 8, r.bottom + TOP + 6 }, set });
  }

  void SetSlider(int i, float mx) {
    auto& s = sliders[i];
    float v = Clamp((mx - (s.r.left + 8)) / (s.r.right - s.r.left - 16), 0, 1);
    v = roundf(v * 100) / 100;
    s.set(v);
    Invalidate();
  }

  void ReleaseApps() {
    for (auto& ap : apps) for (auto* s : ap.vols) s->Release();
    apps.clear();
  }

  void RefreshAudio() {
    if (dragSlider >= 0) return;  // no cambiar la lista mientras se arrastra
    HookVolume();
    if (s_vol) {
      s_vol->GetMasterVolumeLevelScalar(&master);
      BOOL m = FALSE; s_vol->GetMute(&m); masterMute = m != FALSE;
    }
    ReleaseApps();
    apps = ReadSessions();
  }

  void DrawVolume(ID2D1RenderTarget* rt, float a) {
    // ---- salida principal
    D2D1_RECT_F mc{ 16, 44, 452, 108 };
    Card(rt, mc, a);
    float cy = (mc.top + mc.bottom) / 2;
    Btn(rt, { mc.left + 12, cy - 18, mc.left + 48, cy + 18 }, VolGlyph(master, masterMute), 15, a,
        [this] { if (s_vol) { masterMute = !masterMute; s_vol->SetMute(masterMute, nullptr); } }, true, true);
    DrawText(rt, s_devName.empty() ? T(L"Salida de audio", L"Audio output") : s_devName, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), { mc.left + 62, mc.top + 8, mc.right - 64, mc.top + 28 }, W(a));
    Slider(rt, { mc.left + 62, mc.top + 34, mc.right - 64, mc.top + 50 }, master, masterMute, a, [this](float v) {
      if (!s_vol) return;
      master = v;
      s_vol->SetMasterVolumeLevelScalar(v, nullptr);
      if (masterMute && v > 0) { masterMute = false; s_vol->SetMute(FALSE, nullptr); }
    });
    DrawText(rt, masterMute ? T(L"Silencio", L"Muted") : std::to_wstring((int)lroundf(master * 100)) + L"%", Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             { mc.right - 62, mc.top, mc.right - 12, mc.bottom }, masterMute ? W(a, .5f) : W(a), AL_RIGHT);

    // ---- apps que estan usando el sonido
    D2D1_RECT_F ac{ 16, 116, 452, 252 };
    Card(rt, ac, a);
    if (apps.empty()) {
      DrawSymbol(rt, L"\uE8D6", { ac.left, ac.top + 26, ac.right, ac.top + 66 }, 22, W(a, .45f));
      DrawText(rt, T(L"Ninguna app est\u00E1 usando el sonido", L"No app is playing sound"), Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), { ac.left, ac.top + 70, ac.right, ac.top + 90 }, W(a, .8f), AL_CENTER);
      DrawText(rt, T(L"Pon m\u00FAsica o un video y aparecer\u00E1 aqu\u00ED", L"Play music or a video and it shows up here"), Font(11.5f), { ac.left, ac.top + 90, ac.right, ac.top + 108 }, W(a, .5f), AL_CENTER);
    }
    const int maxRows = 4;
    for (int i = 0; i < (int)apps.size() && i < maxRows; i++) {
      auto& ap = apps[i];
      float y = ac.top + 6 + i * 32, ry = y + 16;
      D2D1_RECT_F ir{ ac.left + 14, ry - 11, ac.left + 36, ry + 11 };
      ID2D1Bitmap* bm = ap.exe.empty() ? nullptr : GetIcon(ap.exe, ap.name, Px(22));
      if (bm) DrawBitmapHQ(rt, bm, ir, a);
      else DrawSymbol(rt, ap.exe.empty() ? L"\uE7F3" : L"\uE8D6", ir, 13, W(a, .7f));
      DrawText(rt, ap.name, Font(12), { ac.left + 44, y, ac.left + 160, y + 32 }, W(a, .9f));
      Slider(rt, { ac.left + 170, ry - 8, ac.right - 96, ry + 8 }, ap.vol, ap.mute, a, [this, i](float v) {
        if (i >= (int)apps.size()) return;
        auto& p = apps[i];
        p.vol = v;
        for (auto* s : p.vols) { s->SetMasterVolume(v, nullptr); if (p.mute && v > 0) s->SetMute(FALSE, nullptr); }
        if (v > 0) p.mute = false;
      });
      DrawText(rt, ap.mute ? L"\u2014" : std::to_wstring((int)lroundf(ap.vol * 100)), Font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), { ac.right - 88, y, ac.right - 52, y + 32 },
               W(a, ap.mute ? .5f : 1), AL_RIGHT);
      Btn(rt, { ac.right - 44, ry - 13, ac.right - 18, ry + 13 }, VolGlyph(ap.vol, ap.mute), 12, a, [this, i] {
        if (i >= (int)apps.size()) return;
        auto& p = apps[i];
        p.mute = !p.mute;
        for (auto* s : p.vols) s->SetMute(p.mute, nullptr);
      });
    }
    if ((int)apps.size() > maxRows)
      DrawText(rt, L"+" + std::to_wstring(apps.size() - maxRows) + T(L" m\u00E1s en el mezclador de Windows", L" more in the Windows mixer"), Font(10.5f), { ac.left, ac.bottom - 14, ac.right - 14, ac.bottom - 2 }, W(a, .5f), AL_RIGHT);

    // ---- sonido de Windows
    D2D1_RECT_F ec{ 464, 44, 704, 252 };
    Card(rt, ec, a);
    float ecx = (ec.left + ec.right) / 2;
    D2D1_RECT_F er{ ecx - 22, ec.top + 12, ecx + 22, ec.top + 56 };
    rt->FillEllipse(D2D1::Ellipse({ ecx, (er.top + er.bottom) / 2 }, 22, 22), Brush(WithA(g_th.accent, a)));
    DrawSymbol(rt, L"\uE995", er, 18, RGBA(255, 255, 255, a));
    DrawText(rt, T(L"Sonido de Windows", L"Windows sound"), Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), { ec.left, ec.top + 60, ec.right, ec.top + 80 }, W(a), AL_CENTER);
    DrawText(rt, T(L"Dispositivos y mezclador", L"Devices and mixer"), Font(11), { ec.left, ec.top + 80, ec.right, ec.top + 96 }, W(a, .55f), AL_CENTER);
    float px0 = ec.left + 14, px1 = ec.right - 14;
    Pill(rt, { px0, ec.top + 106, px1, ec.top + 136 }, L"\uE9E9", T(L"Mezclador de volumen", L"Volume mixer"), a,
         [] { RunShell(IsWin11() ? L"ms-settings:apps-volume" : L"sndvol.exe"); }, false, true);
    Pill(rt, { px0, ec.top + 142, px1, ec.top + 170 }, L"\uE7F3", T(L"Configuraci\u00F3n de sonido", L"Sound settings"), a, [] { RunShell(L"ms-settings:sound"); }, false);
    Pill(rt, { px0, ec.top + 176, px1, ec.top + 204 }, L"\uE7F4", T(L"Panel de sonido cl\u00E1sico", L"Classic sound panel"), a, [] { RunShell(L"control.exe", L"mmsys.cpl"); }, false);
  }

  void DrawPanel(ID2D1RenderTarget* rt, float a) {
    // ---- musica
    D2D1_RECT_F mc{ 16, 44, 356, 176 };
    Card(rt, mc, a);
    D2D1_RECT_F ar{ mc.left + 14, mc.top + 14, mc.left + 118, mc.top + 118 };
    DrawArt(rt, ar, 14, a);
    float tx = ar.right + 14, tr = mc.right - 14;
    if (media.has) AddBtn({ ar.left, ar.top, tr, mc.top + 70 }, [] { Media_FocusSource(); }, false);  // ir a Spotify, YouTube Music o la pestaña
    if (media.has) {
      DrawText(rt, media.title.empty() ? T(L"Sin título", L"Untitled") : media.title, Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), { tx, mc.top + 14, tr, mc.top + 36 }, W(a));
      DrawText(rt, media.artist, Font(13), { tx, mc.top + 36, tr, mc.top + 54 }, W(a, .65f));
      DrawText(rt, media.app, Font(11), { tx, mc.top + 54, tr, mc.top + 70 }, W(a, .4f));
      double pos = media.pos + (media.playing ? (GetTickCount() - media.posTick) / 1000.0 : 0);
      if (media.dur > 0) pos = std::min(pos, media.dur);
      float py = mc.top + 78;
      FillRound(rt, { tx, py, tr, py + 4 }, 2, Soft(a, .2f));
      if (media.dur > 0) FillRound(rt, { tx, py, tx + (tr - tx) * (float)(pos / media.dur), py + 4 }, 2, W(a, .9f));
      DrawText(rt, Clock(pos), Font(10.5f), { tx, py + 6, tx + 60, py + 20 }, W(a, .5f));
      if (media.dur > 0) DrawText(rt, L"-" + Clock(media.dur - pos), Font(10.5f), { tr - 60, py + 6, tr, py + 20 }, W(a, .5f), AL_RIGHT);
    } else {
      DrawText(rt, T(L"Nada en reproducción", L"Nothing playing"), Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), { tx, mc.top + 26, tr, mc.top + 48 }, W(a));
      DrawText(rt, T(L"Pon música o un video", L"Play some music or a video"), Font(12.5f), { tx, mc.top + 48, tr, mc.top + 66 }, W(a, .55f));
    }
    float cx = (tx + tr) / 2, by = mc.top + 104;
    Btn(rt, { cx - 56, by, cx - 24, by + 28 }, L"\uE892", 14, a, [] { SendKeys({ VK_MEDIA_PREV_TRACK }); });
    Btn(rt, { cx - 18, by - 2, cx + 18, by + 30 }, media.playing ? L"\uE769" : L"\uE768", 20, a, [] { SendKeys({ VK_MEDIA_PLAY_PAUSE }); });
    Btn(rt, { cx + 24, by, cx + 56, by + 28 }, L"\uE893", 14, a, [] { SendKeys({ VK_MEDIA_NEXT_TRACK }); });

    // ---- clima
    D2D1_RECT_F wc{ 368, 44, 540, 176 };
    Card(rt, wc, a);
    WeatherInfo w;
    if (Weather_Info(w)) {
      Weather_Icon(rt, w.code, w.day, wc.left + 34, wc.top + 32, 34);
      DrawText(rt, Weather_Temp(w.temp), Font(34, DWRITE_FONT_WEIGHT_LIGHT, true), { wc.left + 14, wc.top + 52, wc.right, wc.top + 94 }, W(a), AL_LEFT, false);
      DrawText(rt, w.city.substr(0, w.city.find(L',')), Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), { wc.left + 14, wc.top + 94, wc.right - 10, wc.top + 110 }, W(a));
      DrawText(rt, Weather_Text(w.code), Font(11.5f), { wc.left + 14, wc.top + 109, wc.right - 10, wc.top + 124 }, W(a, .6f));
      DrawText(rt, L"↑" + Weather_Temp(w.tmax) + L" ↓" + Weather_Temp(w.tmin), Font(11.5f), { wc.left + 14, wc.top + 14, wc.right - 12, wc.top + 30 }, W(a, .6f), AL_RIGHT);
    } else {
      DrawSymbol(rt, L"\uE706", { wc.left, wc.top + 20, wc.right, wc.top + 70 }, 26, W(a, .8f));
      DrawText(rt, T(L"Clima", L"Weather"), Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), { wc.left, wc.top + 74, wc.right, wc.top + 94 }, W(a), AL_CENTER);
      DrawText(rt, g_cfg.hasLoc ? T(L"Cargando…", L"Loading…") : T(L"Toca para elegir ciudad", L"Tap to choose a city"), Font(11), { wc.left, wc.top + 94, wc.right, wc.top + 112 }, W(a, .55f), AL_CENTER);
    }
    AddBtn(wc, [] { Settings_Open(false); }, false);

    // ---- calendario (abre la app elegida por el usuario) con progreso del año
    D2D1_RECT_F cc{ 552, 44, 704, 176 };
    Card(rt, cc, a);
    SYSTEMTIME st; GetLocalTime(&st);
    std::wstring wd = FormatDate(L"dddd", st);
    CharUpperBuffW(wd.data(), (DWORD)wd.size());
    DrawText(rt, wd, Font(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), { cc.left + 14, cc.top + 12, cc.right - 10, cc.top + 28 }, RGBA(255, 69, 58, a));
    DrawText(rt, std::to_wstring(st.wDay), Font(40, DWRITE_FONT_WEIGHT_LIGHT, true), { cc.left + 12, cc.top + 26, cc.right, cc.top + 78 }, W(a), AL_LEFT, false);
    DrawText(rt, FormatDate(L"MMMM yyyy", st), Font(12), { cc.left + 14, cc.top + 80, cc.right - 10, cc.top + 96 }, W(a, .7f));
    SYSTEMTIME jan = st; jan.wMonth = 1; jan.wDay = 1; jan.wHour = jan.wMinute = jan.wSecond = jan.wMilliseconds = 0;
    FILETIME f0, f1; SystemTimeToFileTime(&jan, &f0); SystemTimeToFileTime(&st, &f1);
    double days = (((ULONGLONG)f1.dwHighDateTime << 32 | f1.dwLowDateTime) - ((ULONGLONG)f0.dwHighDateTime << 32 | f0.dwLowDateTime)) / 864000000000.0;
    bool leap = (st.wYear % 4 == 0 && st.wYear % 100 != 0) || st.wYear % 400 == 0;
    float yp = (float)(days / (leap ? 366 : 365));
    wchar_t b[48]; swprintf_s(b, T(L"Día %d · %d%% del año", L"Day %d · %d%% of year"), (int)days + 1, (int)(yp * 100));
    DrawText(rt, b, Font(10.5f), { cc.left + 14, cc.top + 100, cc.right - 10, cc.top + 114 }, W(a, .55f));
    FillRound(rt, { cc.left + 14, cc.top + 118, cc.right - 14, cc.top + 122 }, 2, Soft(a, .2f));
    FillRound(rt, { cc.left + 14, cc.top + 118, cc.left + 14 + (cc.right - cc.left - 28) * yp, cc.top + 122 }, 2, RGBA(255, 69, 58, a));
    if (g_cfg.calendar != CAL_NONE) AddBtn(cc, [] { OpenCalendar(); }, false);

    // ---- sistema (5 clics rapidos aqui abren el "Acerca de" escondido)
    D2D1_RECT_F sc{ 16, 188, 452, 252 };
    Card(rt, sc, a);
    AddBtn(sc, [this] { SecretClick(); }, true);
    MEMORYSTATUSEX ms{ sizeof ms }; GlobalMemoryStatusEx(&ms);
    ULARGE_INTEGER fr{}, tot{};
    GetDiskFreeSpaceExW(L"C:\\", nullptr, &tot, &fr);
    float cyc = (sc.top + sc.bottom) / 2;
    Ring(rt, sc.left + 30, cyc, 13, 4, cpu, RGBA(48, 199, 89, a));
    DrawText(rt, std::to_wstring((int)lroundf(cpu * 100)) + L"%", Font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), { sc.left + 50, cyc - 16, sc.left + 104, cyc }, W(a));
    DrawText(rt, L"CPU", Font(10.5f), { sc.left + 50, cyc, sc.left + 104, cyc + 14 }, W(a, .5f));
    Ring(rt, sc.left + 134, cyc, 13, 4, ms.dwMemoryLoad / 100.f, WithA(g_th.accent, a));
    DrawText(rt, std::to_wstring(ms.dwMemoryLoad) + L"%", Font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), { sc.left + 154, cyc - 16, sc.left + 208, cyc }, W(a));
    DrawText(rt, L"RAM", Font(10.5f), { sc.left + 154, cyc, sc.left + 208, cyc + 14 }, W(a, .5f));
    float du = tot.QuadPart ? 1.f - (float)fr.QuadPart / tot.QuadPart : 0;
    Ring(rt, sc.left + 238, cyc, 13, 4, du, RGBA(255, 159, 10, a));
    swprintf_s(b, L"%.0f GB", fr.QuadPart / 1073741824.0);
    DrawText(rt, b, Font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), { sc.left + 258, cyc - 16, sc.left + 330, cyc }, W(a));
    DrawText(rt, T(L"libres en C:", L"free on C:"), Font(10.5f), { sc.left + 258, cyc, sc.left + 330, cyc + 14 }, W(a, .5f));
    DrawText(rt, L"↓ " + Speed(netIn), Font(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), { sc.left + 338, cyc - 16, sc.right - 10, cyc }, W(a));
    DrawText(rt, L"↑ " + Speed(netOut), Font(11.5f), { sc.left + 338, cyc, sc.right - 10, cyc + 14 }, W(a, .6f));

    // ---- energia
    D2D1_RECT_F pc{ 464, 188, 704, 252 };
    Card(rt, pc, a);
    struct { const wchar_t* g; std::function<void()> f; } pw[5] = {
      { L"\uE7F4", [] { SendKeys({ VK_LWIN, 'D' }); } },  // mostrar el escritorio (otra vez: vuelven las ventanas)
      { L"\uE72E", [] { LockWorkStation(); } },
      { L"\uE708", [] { SetSuspendState(FALSE, FALSE, FALSE); } },
      { L"\uE777", [] { if (ConfirmBox(T(L"¿Reiniciar ahora?", L"Restart now?"))) RunShell(L"shutdown.exe", L"/r /t 0"); } },
      { L"\uE7E8", [] { if (ConfirmBox(T(L"¿Apagar el equipo ahora?", L"Shut down now?"))) RunShell(L"shutdown.exe", L"/s /t 0"); } },
    };
    for (int i = 0; i < 5; i++) {
      float x = pc.left + 24 + i * 48, y = (pc.top + pc.bottom) / 2;
      Btn(rt, { x - 18, y - 18, x + 18, y + 18 }, pw[i].g, 14, a, pw[i].f, false, true);
    }
  }

  void DrawShelf(ID2D1RenderTarget* rt, float a) {
    D2D1_RECT_F area{ 16, 44, cw - 16, 204 };
    Card(rt, area, a);
    if (shelf.empty()) {
      DrawSymbol(rt, L"\uE896", { area.left, area.top + 30, area.right, area.top + 80 }, 30, W(a, .5f));
      DrawText(rt, T(L"Arrastra archivos o carpetas hasta la isla", L"Drag files or folders onto the island"), Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), { area.left, area.top + 84, area.right, area.top + 106 }, W(a), AL_CENTER);
      DrawText(rt, T(L"Se guardan aquí temporalmente para copiarlos, comprimirlos o llevarlos a otra carpeta", L"They're kept here for a while so you can copy, zip or move them"), Font(12), { area.left, area.top + 106, area.right, area.top + 126 }, W(a, .55f), AL_CENTER);
      return;
    }
    const float iw = 82, ih = 72;
    int cols = (int)((area.right - area.left - 16) / iw);
    for (int i = 0; i < (int)shelf.size() && i < cols * 2; i++) {
      float x = area.left + 8 + (i % cols) * iw, y = area.top + 8 + (i / cols) * ih;
      D2D1_RECT_F cell{ x, y, x + iw, y + ih };
      bool hot = false;
      POINT p; GetCursorPos(&p);
      float mx = Dip(p.x - this->x), my = Dip(p.y - this->y) - TOP;
      if (mx >= cell.left && mx < cell.right && my >= cell.top && my < cell.bottom) hot = true;
      if (hot) FillRound(rt, { cell.left + 2, cell.top, cell.right - 2, cell.bottom }, 10, Soft(a, .1f));
      D2D1_RECT_F ir{ x + iw / 2 - 20, y + 6, x + iw / 2 + 20, y + 46 };
      if (IsDir(shelf[i])) DrawFolderIcon(rt, ir, a);
      else if (ID2D1Bitmap* bm = GetIcon(shelf[i], BaseName(shelf[i]), Px(40))) DrawBitmapHQ(rt, bm, ir, a);
      DrawText(rt, BaseName(shelf[i]), Font(10.5f), { x + 4, y + 50, x + iw - 4, y + 66 }, W(a, .85f), AL_CENTER);
      shelfRects.push_back({ cell.left, cell.top + TOP, cell.right, cell.bottom + TOP });
    }
    if ((int)shelf.size() > cols * 2)
      DrawText(rt, L"+" + std::to_wstring(shelf.size() - cols * 2) + T(L" más", L" more"), Font(11), { area.right - 90, area.bottom - 20, area.right - 10, area.bottom - 4 }, W(a, .6f), AL_RIGHT);
    // acciones
    float y = 214, x = 16, h = 30;
    auto act = [&](const wchar_t* g, const wchar_t* t, float w, std::function<void()> f) { Pill(rt, { x, y, x + w, y + h }, g, t, a, f); x += w + 8; };
    act(L"\uE8C8", T(L"Copiar", L"Copy"), 96, [this] { FilesToClipboard(hwnd, shelf); ShowActivity(IA_SHELF, 0, false, 1600, T(L"Copiado: pégalo donde quieras (Ctrl+V)", L"Copied: paste anywhere (Ctrl+V)")); });
    act(L"\uF012", T(L"Comprimir .zip", L"Zip"), 138, [this] { Zip(); });
    act(L"\uE8DE", T(L"Copiar a…", L"Copy to…"), 112, [this] { CopyTo(false); });
    act(L"\uE8DE", T(L"Mover a…", L"Move to…"), 106, [this] { CopyTo(true); });
    act(L"\uE74D", T(L"Vaciar", L"Clear"), 92, [this] { shelf.clear(); ReleaseIcons(Px(40)); Retarget(); });
    DrawText(rt, T(L"Arrástralos fuera", L"Drag them out"), Font(11), { x, y, cw - 16, y + h }, W(a, .5f), AL_RIGHT);
  }

  // ------------------------------------------------------------ bandeja
  void AddFiles(const std::vector<std::wstring>& files) {
    int added = 0;
    for (auto& f : files)
      if (std::find(shelf.begin(), shelf.end(), f) == shelf.end()) { shelf.push_back(f); added++; }
    tab = 1;
    std::wstring t = std::to_wstring(added) + (added == 1 ? T(L" elemento guardado en la bandeja", L" item added to the shelf") : T(L" elementos guardados en la bandeja", L" items added to the shelf"));
    ShowActivity(IA_SHELF, 0, false, 1800, t);
  }

  void Prune() {
    shelf.erase(std::remove_if(shelf.begin(), shelf.end(), [](const std::wstring& p) { return !Exists(p); }), shelf.end());
  }

  void CopyTo(bool move) {
    std::wstring dst = PickFolder(move ? T(L"Mover los archivos de la bandeja a…", L"Move shelf files to…") : T(L"Copiar los archivos de la bandeja a…", L"Copy shelf files to…"));
    if (dst.empty()) return;
    auto items = shelf;
    if (ShellCopy(items, dst, move)) {
      if (move) shelf.clear();
      ShowActivity(IA_SHELF, 0, false, 1800, (move ? T(L"Movido a ", L"Moved to ") : T(L"Copiado a ", L"Copied to ")) + BaseName(dst));
    }
    Prune();
    Retarget();
  }

  void Zip() {
    if (shelf.empty()) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, T(L"\\Bandeja %04d-%02d-%02d %02d.%02d.zip", L"\\Shelf %04d-%02d-%02d %02d.%02d.zip"), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    std::wstring out = DownloadsPath() + name;
    std::wstring list;
    for (auto& p : shelf) {
      std::wstring q = p;
      for (size_t i = 0; (i = q.find(L'\'', i)) != std::wstring::npos; i += 2) q.insert(i, L"'");
      list += (list.empty() ? L"" : L",") + std::wstring(L"'") + q + L"'";
    }
    std::wstring o = out;
    for (size_t i = 0; (i = o.find(L'\'', i)) != std::wstring::npos; i += 2) o.insert(i, L"'");
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command \"Compress-Archive -LiteralPath " + list +
                       L" -DestinationPath '" + o + L"' -Force\"";
    ShowActivity(IA_ZIP, 0, false, 60000, T(L"Comprimiendo…", L"Zipping…"));
    HWND h = hwnd;
    std::thread([cmd, out, h] {
      STARTUPINFOW si{ sizeof si };
      si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi{};
      std::wstring c = cmd;
      bool ok = false;
      if (CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        ok = GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES;
      }
      PostMessageW(h, WM_ISLAND_ZIP, ok, 0);
    }).detach();
  }

  void DragOut(int i) {
    std::vector<std::wstring> items = { shelf[i] };
    IDataObject* d = FilesDataObject(items);
    if (!d) return;
    auto src = new DropSource;
    DWORD eff = 0;
    DoDragDrop(d, src, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
    src->Release();
    d->Release();
    Prune();
    Retarget();
  }

  int ShelfAt(float mx, float my) {
    for (size_t i = 0; i < shelfRects.size(); i++)
      if (mx >= shelfRects[i].left && mx < shelfRects[i].right && my >= shelfRects[i].top && my < shelfRects[i].bottom) return (int)i;
    return -1;
  }

  // ------------------------------------------------------------ datos
  void Stats() {
    FILETIME i, k, u;
    GetSystemTimes(&i, &k, &u);
    auto q = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    ULONGLONG idle = q(i), total = q(k) + q(u);
    if (lastTotal && total > lastTotal) cpu = 1.f - (float)(idle - lastIdle) / (total - lastTotal);
    lastIdle = idle; lastTotal = total;
    MIB_IF_TABLE2* t = nullptr;
    if (GetIfTable2(&t) == NO_ERROR) {
      ULONGLONG in = 0, out = 0;
      for (ULONG n = 0; n < t->NumEntries; n++) {
        auto& r = t->Table[n];
        if (r.OperStatus != IfOperStatusUp || r.Type == IF_TYPE_SOFTWARE_LOOPBACK || !r.InterfaceAndOperStatusFlags.HardwareInterface) continue;
        in += r.InOctets; out += r.OutOctets;
      }
      FreeMibTable(t);
      DWORD now = GetTickCount();
      if (lastNet && now > lastNet) {
        double dt = (now - lastNet) / 1000.0;
        netIn = (in - lastIn) / dt; netOut = (out - lastOut) / dt;
      }
      lastIn = in; lastOut = out; lastNet = now;
    }
  }

  void UpdateArt() {
    if (media.version == artVersion) return;
    artVersion = media.version;
    if (art) { art->Release(); art = nullptr; }
    if (media.artPx) {
      auto bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
      g_rt->CreateBitmap(D2D1::SizeU(media.artPx, media.artPx), media.art.data(), media.artPx * 4, bp, &art);
    }
    media.art.clear();
    media.art.shrink_to_fit();
  }

  void ShowActivity(int kind, int value, bool flag, DWORD ms, const std::wstring& text = L"") {
    act = kind; actValue = value; actFlag = flag; actText = text;
    actUntil = GetTickCount() + ms;
    SetTimer(hwnd, 4, ms + 20, nullptr);
    Retarget();
    TopBars_Refresh();  // si la barra esta oculta, la isla aparece un momento para el aviso
  }

  bool CursorInside() {
    POINT p; GetCursorPos(&p);
    RECT r; GetWindowRect(hwnd, &r);
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom + Px(4);
  }

  bool Busy() { return hover || expanded || dragOver || pressItem >= 0 || dragSlider >= 0 || (act != IA_NONE && GetTickCount() < actUntil); }

  // ------------------------------------------------------------ mensajes
  LRESULT Handle(UINT m, WPARAM wp, LPARAM lp) override {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, hwnd };
      TrackMouseEvent(&t);
      KillTimer(hwnd, 5);
      if (!hover) { hover = true; Retarget(); }
      float mx = Dip(GET_X_LPARAM(lp)), my = Dip(GET_Y_LPARAM(lp));
      if (dragSlider >= 0) { if (dragSlider < (int)sliders.size()) SetSlider(dragSlider, mx); return 0; }
      if (pressItem >= 0 && (wp & MK_LBUTTON)) {
        POINT p; GetCursorPos(&p);
        if (abs(p.x - pressPt.x) > Px(5) || abs(p.y - pressPt.y) > Px(5)) { int i = pressItem; pressItem = -1; DragOut(i); }
        return 0;
      }
      int h = -1;
      for (size_t i = 0; i < btns.size(); i++)
        if (mx >= btns[i].r.left && mx < btns[i].r.right && my >= btns[i].r.top && my < btns[i].r.bottom) h = (int)i;
      if (h != hotBtn) hotBtn = h;
      if (expanded) Invalidate();
      return 0;
    }
    case WM_MOUSELEAVE:
      SetTimer(hwnd, 5, expanded ? 650 : 220, nullptr);
      return 0;
    case WM_LBUTTONDOWN: {
      float mx = Dip(GET_X_LPARAM(lp)), my = Dip(GET_Y_LPARAM(lp));
      pressItem = (expanded && tab == 1) ? ShelfAt(mx, my) : -1;
      GetCursorPos(&pressPt);
      if (expanded && tab == 2)
        for (size_t i = 0; i < sliders.size(); i++)
          if (mx >= sliders[i].r.left && mx < sliders[i].r.right && my >= sliders[i].r.top && my < sliders[i].r.bottom) {
            dragSlider = (int)i;
            SetCapture(hwnd);
            SetSlider(dragSlider, mx);
            break;
          }
      return 0;
    }
    case WM_LBUTTONUP: {
      float mx = Dip(GET_X_LPARAM(lp)), my = Dip(GET_Y_LPARAM(lp));
      if (dragSlider >= 0) { dragSlider = -1; ReleaseCapture(); RefreshAudio(); Invalidate(); return 0; }
      if (pressItem >= 0) {  // clic sin arrastrar: abrir
        std::wstring p = shelf[pressItem];
        pressItem = -1;
        if (IsDir(p)) OpenFolder(p); else RunShell(p);
        return 0;
      }
      for (auto& b : btns)
        if (mx >= b.r.left && mx < b.r.right && my >= b.r.top && my < b.r.bottom) {
          auto f = b.act;
          if (!b.keepOpen) { expanded = false; Retarget(); }
          f();
          Invalidate();
          return 0;
        }
      if (!expanded) { expanded = true; Retarget(); }
      return 0;
    }
    case WM_RBUTTONUP: {
      float mx = Dip(GET_X_LPARAM(lp)), my = Dip(GET_Y_LPARAM(lp));
      std::vector<MenuItem> v;
      int i = (expanded && tab == 1) ? ShelfAt(mx, my) : -1;
      if (i >= 0) {
        std::wstring p = shelf[i];
        MenuItem o; o.text = T(L"Abrir", L"Open"); o.action = [p] { if (IsDir(p)) OpenFolder(p); else RunShell(p); }; v.push_back(o);
        MenuItem f; f.text = T(L"Mostrar en el Explorador", L"Show in Explorer"); f.action = [p] { ShowInFolder(p); }; v.push_back(f);
        MenuItem q; q.text = T(L"Quitar de la bandeja", L"Remove from shelf"); q.action = [this, i] { if (i < (int)shelf.size()) shelf.erase(shelf.begin() + i); Retarget(); }; v.push_back(q);
      } else {
        MenuItem c; c.text = T(L"Configuración…", L"Settings…"); c.action = [] { Settings_Open(false); }; v.push_back(c);
        if (g_cfg.calendar != CAL_NONE) { MenuItem g; g.text = T(L"Abrir calendario", L"Open calendar"); g.action = [] { OpenCalendar(); }; v.push_back(g); }
        MenuItem vt; vt.text = T(L"Perfil de volumen", L"Volume profile"); vt.action = [this] { tab = 2; RefreshAudio(); expanded = true; Retarget(); }; v.push_back(vt);
        MenuItem mx; mx.text = T(L"Mezclador de Windows", L"Windows volume mixer"); mx.action = [] { RunShell(IsWin11() ? L"ms-settings:apps-volume" : L"sndvol.exe"); }; v.push_back(mx);
        v.push_back(MenuItem::Sep());
        MenuItem h; h.text = T(L"Salir de Dinamic Zaur", L"Quit Dinamic Zaur"); h.action = [] { PostMessageW(g_msgWnd, WM_CLOSE, 0, 0); }; v.push_back(h);
      }
      POINT p; GetCursorPos(&p);
      ShowMenu(v, p.x, p.y);
      return 0;
    }
    case WM_INPUT: {  // Bloq Mayus / Bloq Num
      RAWINPUT ri; UINT sz = sizeof ri;
      if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEKEYBOARD) {
        USHORT vk = ri.data.keyboard.VKey;
        bool up = ri.data.keyboard.Flags & RI_KEY_BREAK;
        if (vk == VK_CAPITAL) {
          if (!up && !capsDown) { capsOn = !capsOn; ShowActivity(IA_CAPS, 0, capsOn, 1500); }
          capsDown = !up;
        } else if (vk == VK_NUMLOCK) {
          if (!up && !numDown) { numOn = !numOn; ShowActivity(IA_NUM, 0, numOn, 1500); }
          numDown = !up;
        }
      }
      return DefWindowProcW(hwnd, m, wp, lp);
    }
    case WM_CAPTURECHANGED:
      if (dragSlider >= 0 && (HWND)lp != hwnd) { dragSlider = -1; Invalidate(); }
      return 0;
    case WM_ISLAND_ZIP:
      ShowActivity(IA_ZIP, 0, false, 2200, wp ? T(L"ZIP guardado en Descargas", L"ZIP saved to Downloads") : T(L"No se pudo comprimir", L"Couldn't create the ZIP"));
      if (wp) OpenFolder(DownloadsPath());
      return 0;
    case WM_TIMER:
      switch (wp) {
      case 1: Step(); break;
      case 2: Invalidate(); break;
      case 3: Stats(); if (expanded && tab == 2) RefreshAudio(); Invalidate(); break;
      case 4: KillTimer(hwnd, 4); act = IA_NONE; Retarget(); TopBars_Refresh(); break;
      case 5:
        if (!CursorInside() && !MenuOpen() && dragSlider < 0) { KillTimer(hwnd, 5); ReleaseApps(); hover = false; expanded = false; hotBtn = -1; pressItem = -1; Retarget(); TopBars_Refresh(); }
        break;
      case 6: Weather_Fetch(); break;
      case 7: SlideStep(); break;
      }
      return 0;
    }
    return Surface::Handle(m, wp, lp);
  }
};

// ---------------------------------------------------------------- destino de arrastre
static bool HasFiles(IDataObject* d) {
  FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
  return d && d->QueryGetData(&fe) == S_OK;
}

HRESULT IslandDrop::DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) {
  ok = HasFiles(d);
  *eff = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  if (ok) { isl->dragOver = true; isl->Retarget(); }
  return S_OK;
}
HRESULT IslandDrop::DragOver(DWORD, POINTL, DWORD* eff) { *eff = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
HRESULT IslandDrop::DragLeave() { isl->dragOver = false; isl->Retarget(); return S_OK; }
HRESULT IslandDrop::Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) {
  isl->dragOver = false;
  std::vector<std::wstring> files;
  FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
  STGMEDIUM sm{};
  if (SUCCEEDED(d->GetData(&fe, &sm))) {
    HDROP hd = (HDROP)GlobalLock(sm.hGlobal);
    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; i++) {
      wchar_t p[MAX_PATH * 4];
      if (DragQueryFileW(hd, i, p, MAX_PATH * 4)) files.push_back(p);
    }
    GlobalUnlock(sm.hGlobal);
    ReleaseStgMedium(&sm);
  }
  *eff = files.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
  if (!files.empty()) isl->AddFiles(files);
  else isl->Retarget();
  return S_OK;
}

// ---------------------------------------------------------------- API
static Island* s_island;
static bool s_islandWanted = true;

void Island_Create() {
  s_island = new Island;
  Island& i = *s_island;
  // Windows 11: esquinas suavizadas y sombra del sistema. Windows 10: se recorta con una region redondeada.
  i.dwmRound = IsWin11();
  i.radius = i.dwmRound ? 8.f : 12.f;
  i.Create(WS_EX_TOPMOST | WS_EX_NOACTIVATE, false, false);  // opaca: DWM no tiene que desenfocar lo de atras
  i.capsOn = GetKeyState(VK_CAPITAL) & 1;
  i.numOn = GetKeyState(VK_NUMLOCK) & 1;
  RAWINPUTDEVICE rid{ 0x01, 0x06, RIDEV_INPUTSINK, i.hwnd };
  RegisterRawInputDevices(&rid, 1, sizeof rid);
  RegisterDragDrop(i.hwnd, new IslandDrop(&i));
  i.TargetSize();
  i.cw = i.tw; i.ch = i.th;
  i.Place();
  i.Show(true);
  SetTimer(i.hwnd, 3, 60000, nullptr);
  SetTimer(i.hwnd, 6, 30 * 60 * 1000, nullptr);
  HookVolume();
  Media_Start();
  Weather_Fetch();
}

void Island_Show(bool show) {
  if (!s_island) return;
  if (show) s_island->SlideTo(false);
  else if (!s_island->Busy()) s_island->SlideTo(true);
}

bool Island_Busy() { return s_island && s_island->Visible() && s_island->Busy(); }

void Island_OnMedia() {
  if (!s_island) return;
  Island& i = *s_island;
  MediaSnap n = Media_Get();
  bool changed = n.trackChanged && n.has && !n.title.empty();
  i.media = std::move(n);
  i.UpdateArt();
  if (changed && !i.expanded && !i.hover) i.ShowActivity(IA_TRACK, 0, false, 2600);
  else i.Retarget();
}

void Island_OnVolume(int level, bool muted) {
  if (!s_island || s_island->expanded) return;
  s_island->ShowActivity(IA_VOLUME, level, muted, 1600);
}

void Island_Invalidate() { if (s_island) s_island->Invalidate(); }

void Island_OnUpdate() { if (s_island && s_island->tab == 3) s_island->Invalidate(); }

// cambio de resolucion, de monitor o de escala: recoloca la isla arriba al centro
void Island_Relayout() {
  if (!s_island) return;
  s_island->Place();
  s_island->Invalidate();
}
