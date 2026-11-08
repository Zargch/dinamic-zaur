#include "common.h"
#include <thread>
#include <condition_variable>
#include <UIAutomation.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#pragma comment(lib, "windowsapp.lib")

// "Ahora suena": lee la sesion multimedia actual de Windows (Spotify, navegadores, reproductores)
// en un hilo propio. Solo trabaja cuando Windows avisa un cambio; la caratula se decodifica una
// vez por cancion a 128 px.
using namespace winrt;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;

static MediaSnap s_snap;
static std::mutex s_mx;
static std::condition_variable s_cv;
static bool s_dirty = true;

static void Signal() {
  { std::lock_guard<std::mutex> lk(s_mx); s_dirty = true; }
  s_cv.notify_one();
}

static void DecodeArt(const std::vector<BYTE>& bytes, MediaSnap& out) {
  out.art.clear(); out.artPx = 0;
  if (bytes.empty()) return;
  IWICStream* st = nullptr;
  if (FAILED(g_wic->CreateStream(&st))) return;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* fr = nullptr;
  if (SUCCEEDED(st->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())) &&
      SUCCEEDED(g_wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
      SUCCEEDED(dec->GetFrame(0, &fr))) {
    UINT w, h;
    fr->GetSize(&w, &h);
    // recorte cuadrado centrado (algunas apps entregan la caratula con bandas)
    UINT side = std::min(w, h);
    IWICBitmapClipper* cl = nullptr;
    g_wic->CreateBitmapClipper(&cl);
    WICRect rc{ (INT)(w - side) / 2, (INT)(h - side) / 2, (INT)side, (INT)side };
    cl->Initialize(fr, &rc);
    const int px = 128;
    IWICBitmapScaler* sc = nullptr;
    g_wic->CreateBitmapScaler(&sc);
    sc->Initialize(cl, px, px, WICBitmapInterpolationModeHighQualityCubic);
    IWICFormatConverter* cv = nullptr;
    g_wic->CreateFormatConverter(&cv);
    cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    out.art.resize(px * px * 4);
    if (SUCCEEDED(cv->CopyPixels(nullptr, px * 4, (UINT)out.art.size(), out.art.data()))) {
      out.artPx = px;
      // color dominante (con saturacion) para teñir la isla, como hace iOS
      double r = 0, g = 0, b = 0, ws = 0;
      for (size_t i = 0; i < out.art.size(); i += 16) {
        double B = out.art[i], G = out.art[i + 1], R = out.art[i + 2];
        double mx = std::max({ R, G, B }), mn = std::min({ R, G, B });
        double wt = (mx > 0 ? (mx - mn) / mx : 0) * (mx / 255.0) + .02;
        r += R * wt; g += G * wt; b += B * wt; ws += wt;
      }
      out.color = D2D1::ColorF((float)(r / ws / 255), (float)(g / ws / 255), (float)(b / ws / 255));
    } else out.art.clear();
    cv->Release(); sc->Release(); cl->Release();
  }
  if (fr) fr->Release();
  if (dec) dec->Release();
  st->Release();
}

static void Worker() {
  init_apartment(apartment_type::multi_threaded);
  GlobalSystemMediaTransportControlsSessionManager mgr{ nullptr };
  try { mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get(); }
  catch (...) { return; }
  GlobalSystemMediaTransportControlsSession session{ nullptr };
  event_token tProps{}, tPlay{}, tTime{};
  auto hook = [&]() {
    if (session) {
      try {
        session.MediaPropertiesChanged(tProps);
        session.PlaybackInfoChanged(tPlay);
        session.TimelinePropertiesChanged(tTime);
      } catch (...) {}
    }
    session = mgr.GetCurrentSession();
    if (session) {
      tProps = session.MediaPropertiesChanged([](auto&&, auto&&) { Signal(); });
      tPlay = session.PlaybackInfoChanged([](auto&&, auto&&) { Signal(); });
      tTime = session.TimelinePropertiesChanged([](auto&&, auto&&) { Signal(); });
    }
  };
  mgr.CurrentSessionChanged([](auto&&, auto&&) { Signal(); });
  bool needHook = true;
  std::wstring lastKey;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(s_mx);
      s_cv.wait_for(lk, std::chrono::seconds(30), [] { return s_dirty; });
      s_dirty = false;
    }
    try {
      auto cur = mgr.GetCurrentSession();
      if (needHook || cur != session) { hook(); needHook = false; }
      MediaSnap n;
      { std::lock_guard<std::mutex> lk(s_mx); n = s_snap; }
      if (!session) {
        n.has = false; n.playing = false; n.title.clear(); n.artist.clear(); n.art.clear(); n.artPx = 0;
        lastKey.clear();
      } else {
        auto props = session.TryGetMediaPropertiesAsync().get();
        auto pb = session.GetPlaybackInfo();
        auto tl = session.GetTimelineProperties();
        // mientras la app cambia de cancion o se cierra, Windows puede devolver estos datos vacios:
        // usarlos asi cerraba el programa (acceso a memoria nula, que catch(...) no atrapa)
        if (!props || !pb || !tl) {
          needHook = true;
          Sleep(500);
          Signal();
          continue;
        }
        n.has = true;
        n.playing = pb.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        n.title = props.Title().c_str();
        n.artist = props.Artist().c_str();
        std::wstring aumid = session.SourceAppUserModelId().c_str();
        n.aumid = aumid;
        n.app = aumid.empty() ? L"" : AppDisplayName(aumid);
        n.dur = std::chrono::duration<double>(tl.EndTime() - tl.StartTime()).count();
        double pos = std::chrono::duration<double>(tl.Position() - tl.StartTime()).count();
        if (n.playing) {  // la posicion se informa con retraso: la llevamos hasta "ahora"
          double since = std::chrono::duration<double>(winrt::clock::now() - tl.LastUpdatedTime()).count();
          if (since > 0 && since < 3600) pos += since;
        }
        n.pos = std::max(0.0, std::min(pos, n.dur > 0 ? n.dur : pos));
        n.posTick = GetTickCount();
        std::wstring key = n.title + L"|" + n.artist;
        if (key != lastKey) {
          lastKey = key;
          std::vector<BYTE> bytes;
          if (auto thumb = props.Thumbnail()) {
            try {
              auto stream = thumb.OpenReadAsync().get();
              uint32_t size = (uint32_t)stream.Size();
              if (size && size < 8 * 1024 * 1024) {
                Buffer buf(size);
                auto res = stream.ReadAsync(buf, size, InputStreamOptions::None).get();
                bytes.assign(res.data(), res.data() + res.Length());
              }
            } catch (...) {}
          }
          DecodeArt(bytes, n);
          n.version++;
          n.trackChanged = true;
        }
      }
      { std::lock_guard<std::mutex> lk(s_mx); s_snap = n; }
      PostMessageW(g_msgWnd, WM_APP_MEDIA, 0, 0);
    } catch (...) {
      needHook = true;
    }
  }
}

void Media_Start() { std::thread(Worker).detach(); }

MediaSnap Media_Get() {
  std::lock_guard<std::mutex> lk(s_mx);
  MediaSnap c = s_snap;
  s_snap.trackChanged = false;
  return c;
}

// ---------------------------------------------------------------- ir a la app que suena
static bool IContainsStr(const std::wstring& hay, const std::wstring& needle) {
  if (needle.empty()) return false;
  return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

// pestaña del navegador que suena: la que tiene el titulo de la cancion o el aviso de audio.
// Se busca con UI Automation (las pestañas de Chrome/Edge/Brave/Firefox son TabItem seleccionables).
static bool SelectPlayingTab(HWND w, const std::wstring& title) {
  IUIAutomation* ua = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ua)))) return false;
  bool done = false;
  IUIAutomationElement* root = nullptr;
  IUIAutomationCondition* cond = nullptr;
  IUIAutomationElementArray* tabs = nullptr;
  VARIANT v; v.vt = VT_I4; v.lVal = UIA_TabItemControlTypeId;
  if (SUCCEEDED(ua->ElementFromHandle(w, &root)) && root && SUCCEEDED(ua->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) &&
      SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &tabs)) && tabs) {
    int n = 0;
    tabs->get_Length(&n);
    IUIAutomationElement* best = nullptr;
    int bestScore = 0;
    for (int i = 0; i < n; i++) {
      IUIAutomationElement* e = nullptr;
      if (FAILED(tabs->GetElement(i, &e)) || !e) continue;
      BSTR nm = nullptr;
      e->get_CurrentName(&nm);
      std::wstring name = nm ? nm : L"";
      SysFreeString(nm);
      int score = 0;
      if (IContainsStr(name, title)) score += 2;
      for (auto k : { L"reproduciendo audio", L"audio playing", L"playing audio", L"reproduciendo contenido multimedia" })
        if (IContainsStr(name, k)) score += 1;
      if (score > bestScore) { if (best) best->Release(); best = e; bestScore = score; }
      else e->Release();
    }
    if (best) {
      IUIAutomationSelectionItemPattern* sp = nullptr;
      if (SUCCEEDED(best->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&sp))) && sp) { done = SUCCEEDED(sp->Select()); sp->Release(); }
      if (!done) {
        IUIAutomationLegacyIAccessiblePattern* lp = nullptr;
        if (SUCCEEDED(best->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&lp))) && lp) { done = SUCCEEDED(lp->DoDefaultAction()); lp->Release(); }
      }
      best->Release();
    }
  }
  if (tabs) tabs->Release();
  if (cond) cond->Release();
  if (root) root->Release();
  ua->Release();
  return done;
}

void Media_FocusSource() {
  std::wstring aumid, title;
  { std::lock_guard<std::mutex> lk(s_mx); aumid = Lower(s_snap.aumid); title = s_snap.title; }
  if (aumid.empty()) return;
  bool isExe = aumid.size() > 4 && aumid.compare(aumid.size() - 4, 4, L".exe") == 0;
  // navegadores: su id de sesion es el nombre corto del programa (Firefox usa un hash fijo)
  std::wstring browserExe;
  static const std::pair<const wchar_t*, const wchar_t*> browsers[] = {
    { L"chrome", L"chrome.exe" }, { L"msedge", L"msedge.exe" }, { L"brave", L"brave.exe" }, { L"opera", L"opera.exe" },
    { L"vivaldi", L"vivaldi.exe" }, { L"308046b0af4a39cb", L"firefox.exe" }, { L"firefox", L"firefox.exe" } };
  for (auto& b : browsers) if (aumid == b.first) browserExe = b.second;
  auto base = [](const std::wstring& exe) { return Lower(exe.substr(exe.find_last_of(L'\\') + 1)); };
  std::vector<HWND> exact, same;
  for (auto& r : EnumRunning()) {
    if (Lower(r.aumid) == aumid) exact.push_back(r.hwnd);                     // app de la Store o PWA (YouTube Music instalada)
    else if ((isExe && base(r.exe) == aumid) || (!browserExe.empty() && base(r.exe) == browserExe)) {
      if (IContainsStr(r.title, title)) same.insert(same.begin(), r.hwnd);  // la ventana con la pestaña ya visible primero
      else same.push_back(r.hwnd);
    }
  }
  if (!exact.empty()) { ActivateWindow(exact.front()); return; }
  if (!same.empty()) {
    if (!browserExe.empty())
      for (HWND w : same)
        if (SelectPlayingTab(w, title)) { ActivateWindow(w); return; }
    ActivateWindow(same.front());
    return;
  }
  // sin ventana (p. ej. Spotify cerrado a la bandeja): abrir la app
  if (!isExe && browserExe.empty()) RunShell(L"shell:AppsFolder\\" + aumid);
}
