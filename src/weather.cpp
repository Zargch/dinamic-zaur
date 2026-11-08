#include "common.h"
#include <thread>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>
#pragma comment(lib, "windowsapp.lib")

// Clima para todo el mundo con servicios libres y sin clave:
//  - Pronostico: MET Norway (api.met.no, Instituto Meteorologico de Noruega). Datos con licencia
//    abierta (CC BY 4.0 / NLOD): se puede usar en apps libres o comerciales citando la fuente.
//  - Buscar ciudades y nombrar la ubicacion: Nominatim / OpenStreetMap (ODbL, citando la fuente).
//    Su politica pide identificarse (User-Agent) y no mas de una consulta por segundo: solo se
//    consulta cuando el usuario busca o al detectar la ubicacion.
//  - Ubicacion automatica: el servicio de ubicacion de Windows (si el usuario lo permite).
// Los iconos y codigos internos siguen la tabla WMO (0 despejado ... 95 tormenta).

struct Weather {
  bool ok = false;
  std::wstring city, err;
  double temp = 0, tmax = 0, tmin = 0;
  int code = 0; bool day = true;
};
static Weather s_weather;
static std::mutex s_wmx;

static std::wstring Lang() { return g_es ? L"es" : L"en"; }

// "partlycloudy_day" -> codigo WMO aproximado
static int CodeFromSymbol(const std::string& s) {
  auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
  if (has("thunder")) return 95;
  if (has("snow")) return has("heavy") ? 75 : 71;
  if (has("sleet")) return 66;
  if (has("rain")) {
    if (has("showers")) return has("heavy") ? 82 : 80;
    return has("light") ? 51 : has("heavy") ? 65 : 63;
  }
  if (has("fog")) return 45;
  if (s.rfind("clearsky", 0) == 0) return 0;
  if (s.rfind("fair", 0) == 0) return 1;
  if (s.rfind("partlycloudy", 0) == 0) return 2;
  return 3;
}

static void FetchWeather() {
  std::thread([] {
    Weather w;
    if (!g_cfg.hasLoc) w.err = T(L"Elige tu ciudad en Configuración", L"Pick your city in Settings");
    else {
      wchar_t path[200];
      swprintf_s(path, L"/weatherapi/locationforecast/2.0/compact?lat=%.4f&lon=%.4f", g_cfg.lat, g_cfg.lon);
      std::string j = HttpGet(L"api.met.no", path);
      size_t ts = j.find("\"timeseries\"");
      if (ts == std::string::npos) w.err = T(L"Sin conexión", L"Offline");
      else {
        w.ok = true;
        w.city = g_cfg.weatherCity;
        // la primera entrada es la hora actual; se recorren 24 h para maxima y minima
        size_t p = ts;
        double mx = -1e9, mn = 1e9;
        for (int i = 0; i < 24; i++) {
          p = j.find("\"time\":\"", p + 1);
          if (p == std::string::npos) break;
          double t = JNum(j, "air_temperature", p);
          mx = std::max(mx, t); mn = std::min(mn, t);
          if (i == 0) {
            w.temp = t;
            size_t sc = j.find("\"symbol_code\":\"", p);
            size_t next = j.find("\"time\":\"", p + 1);
            std::string sym;
            if (sc != std::string::npos && (next == std::string::npos || sc < next)) sym = j.substr(sc + 15, j.find('"', sc + 15) - sc - 15);
            w.code = CodeFromSymbol(sym);
            if (sym.find("_night") != std::string::npos) w.day = false;
            else if (sym.find("_day") != std::string::npos || sym.find("_polartwilight") != std::string::npos) w.day = true;
            else { SYSTEMTIME st; GetLocalTime(&st); w.day = st.wHour >= 6 && st.wHour < 18; }
          }
        }
        w.tmax = mx > -1e8 ? mx : w.temp;
        w.tmin = mn < 1e8 ? mn : w.temp;
      }
    }
    { std::lock_guard<std::mutex> lk(s_wmx); s_weather = w; }
    PostMessageW(g_msgWnd, WM_APP_WEATHER, 0, 0);
  }).detach();
}

// ---------------------------------------------------------------- buscar ciudades (OpenStreetMap)
static std::wstring PlaceLabel(const std::string& j, size_t from, size_t to, std::wstring& name) {
  std::string obj = j.substr(from, to == std::string::npos ? std::string::npos : to - from);
  name = JStr(obj, "name");
  std::wstring disp = JStr(obj, "display_name");
  if (name.empty()) name = disp.substr(0, disp.find(L','));
  size_t a = obj.find("\"address\"");
  std::wstring state, country;
  if (a != std::string::npos) { state = JStr(obj, "state", a); country = JStr(obj, "country", a); }
  std::wstring label = name;
  if (!state.empty() && state != name) label += L", " + state;
  if (!country.empty()) label += L", " + country;
  return label;
}

std::vector<Place> Weather_Search(const std::wstring& query) {
  std::vector<Place> out;
  std::string j = HttpGet(L"nominatim.openstreetmap.org",
                          L"/search?format=jsonv2&addressdetails=1&limit=8&accept-language=" + Lang() + L"&q=" + UrlEnc(query));
  size_t p = j.find("{\"place_id\"");
  while (p != std::string::npos) {
    size_t next = j.find("{\"place_id\"", p + 1);
    Place pl;
    pl.label = PlaceLabel(j, p, next, pl.name);
    pl.lat = JNum(j, "lat", p);
    pl.lon = JNum(j, "lon", p);
    if (!pl.name.empty()) out.push_back(pl);
    p = next;
  }
  return out;
}

// ---------------------------------------------------------------- ubicacion de Windows
bool Weather_Locate(Place& out) {
  using namespace winrt::Windows::Devices::Geolocation;
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (...) {}
  try {
    if (Geolocator::RequestAccessAsync().get() != GeolocationAccessStatus::Allowed) return false;
    Geolocator g;
    auto pos = g.GetGeopositionAsync(std::chrono::minutes(30), std::chrono::seconds(10)).get();
    auto c = pos.Coordinate().Point().Position();
    out.lat = c.Latitude; out.lon = c.Longitude;
  } catch (...) { return false; }
  wchar_t path[200];
  swprintf_s(path, L"/reverse?format=jsonv2&zoom=10&addressdetails=1&lat=%.4f&lon=%.4f&accept-language=", out.lat, out.lon);
  std::string j = HttpGet(L"nominatim.openstreetmap.org", path + Lang());
  if (j.find("\"place_id\"") != std::string::npos) out.label = PlaceLabel(j, 0, std::string::npos, out.name);
  if (out.name.empty()) {
    wchar_t b[64]; swprintf_s(b, L"%.2f, %.2f", out.lat, out.lon);
    out.name = out.label = b;
  }
  return true;
}

// ---------------------------------------------------------------- textos e iconos
static const wchar_t* WeatherText(int c) {
  if (c == 0) return T(L"Despejado", L"Clear");
  if (c == 1) return T(L"Mayormente despejado", L"Mostly clear");
  if (c == 2) return T(L"Parcialmente nublado", L"Partly cloudy");
  if (c == 3) return T(L"Nublado", L"Cloudy");
  if (c == 45 || c == 48) return T(L"Niebla", L"Fog");
  if (c >= 51 && c <= 57) return T(L"Llovizna", L"Drizzle");
  if (c == 66 || c == 67) return T(L"Aguanieve", L"Sleet");
  if (c >= 80 && c <= 82) return T(L"Chubascos", L"Showers");
  if (c >= 61 && c <= 65) return T(L"Lluvia", L"Rain");
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return T(L"Nieve", L"Snow");
  if (c >= 95) return T(L"Tormenta", L"Thunderstorm");
  return L"—";
}

static ID2D1StrokeStyle* RoundCaps() {
  static ID2D1StrokeStyle* s = nullptr;
  if (!s) g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &s);
  return s;
}

static void Cloud(ID2D1RenderTarget* rt, float cx, float cy, float s, D2D1_COLOR_F c) {
  auto b = Brush(c);
  rt->FillEllipse(D2D1::Ellipse({ cx - s * .16f, cy + s * .02f }, s * .2f, s * .2f), b);
  rt->FillEllipse(D2D1::Ellipse({ cx + s * .08f, cy - s * .08f }, s * .26f, s * .26f), b);
  rt->FillRoundedRectangle(D2D1::RoundedRect({ cx - s * .38f, cy + s * .02f, cx + s * .4f, cy + s * .22f }, s * .1f, s * .1f), b);
}

static void WeatherIcon(ID2D1RenderTarget* rt, int code, bool day, float cx, float cy, float s) {
  D2D1_COLOR_F sun = RGBA(255, 204, 0), white = RGBA(255, 255, 255), moon = RGBA(236, 236, 245);
  auto Sun = [&](float x, float y, float k) {
    if (day) {
      rt->FillEllipse(D2D1::Ellipse({ x, y }, s * .2f * k, s * .2f * k), Brush(sun));
      for (int i = 0; i < 8; i++) {
        float a = i * 0.785f;
        rt->DrawLine({ x + cosf(a) * s * .28f * k, y + sinf(a) * s * .28f * k }, { x + cosf(a) * s * .38f * k, y + sinf(a) * s * .38f * k }, Brush(sun), s * .05f * k, RoundCaps());
      }
    } else {
      ID2D1EllipseGeometry *a, *b;
      g_d2d->CreateEllipseGeometry(D2D1::Ellipse({ x, y }, s * .24f * k, s * .24f * k), &a);
      g_d2d->CreateEllipseGeometry(D2D1::Ellipse({ x + s * .13f * k, y - s * .1f * k }, s * .2f * k, s * .2f * k), &b);
      ID2D1PathGeometry* p = MakePath([&](ID2D1GeometrySink* sk) { a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, sk); });
      rt->FillGeometry(p, Brush(moon));
      p->Release(); a->Release(); b->Release();
    }
  };
  if (code == 0) { Sun(cx, cy, 1.1f); return; }
  if (code <= 2) { Sun(cx - s * .14f, cy - s * .14f, .8f); Cloud(rt, cx + s * .06f, cy + s * .04f, s * .85f, white); return; }
  if (code == 45 || code == 48) {
    for (int i = 0; i < 3; i++) rt->DrawLine({ cx - s * .34f + i * s * .05f, cy - s * .14f + i * s * .14f }, { cx + s * .34f - i * s * .05f, cy - s * .14f + i * s * .14f }, Brush(white), s * .07f, RoundCaps());
    return;
  }
  Cloud(rt, cx, cy - s * .1f, s, code == 3 ? RGBA(235, 238, 245) : white);
  float y0 = cy + s * .2f;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
    for (int i = -1; i <= 1; i++) rt->DrawLine({ cx + i * s * .18f + s * .04f, y0 }, { cx + i * s * .18f - s * .04f, y0 + s * .18f }, Brush(RGBA(90, 200, 255)), s * .06f, RoundCaps());
  else if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    for (int i = -1; i <= 1; i++) rt->FillEllipse(D2D1::Ellipse({ cx + i * s * .18f, y0 + s * .1f }, s * .045f, s * .045f), Brush(white));
  else if (code >= 95) {
    auto bolt = MakePath([&](ID2D1GeometrySink* k) {
      k->BeginFigure({ cx + s * .04f, y0 - s * .02f }, D2D1_FIGURE_BEGIN_FILLED);
      k->AddLine({ cx - s * .1f, y0 + s * .14f }); k->AddLine({ cx, y0 + s * .14f });
      k->AddLine({ cx - s * .06f, y0 + s * .3f }); k->AddLine({ cx + s * .12f, y0 + s * .08f });
      k->AddLine({ cx + s * .02f, y0 + s * .08f });
      k->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    rt->FillGeometry(bolt, Brush(sun));
    bolt->Release();
  }
}

// ---------------------------------------------------------------- API
bool Weather_Info(WeatherInfo& o) {
  std::lock_guard<std::mutex> lk(s_wmx);
  o.ok = s_weather.ok; o.city = s_weather.city; o.err = s_weather.err;
  o.temp = s_weather.temp; o.tmax = s_weather.tmax; o.tmin = s_weather.tmin;
  o.code = s_weather.code; o.day = s_weather.day;
  return o.ok;
}

void Weather_Fetch() { FetchWeather(); }
void Weather_Icon(ID2D1RenderTarget* rt, int code, bool day, float cx, float cy, float s) { WeatherIcon(rt, code, day, cx, cy, s); }
const wchar_t* Weather_Text(int code) { return WeatherText(code); }

std::wstring Weather_Temp(double c, bool unit) {
  double v = g_cfg.fahrenheit ? c * 9 / 5 + 32 : c;
  wchar_t b[16];
  swprintf_s(b, unit ? (g_cfg.fahrenheit ? L"%.0f°F" : L"%.0f°C") : L"%.0f°", v);
  return b;
}
