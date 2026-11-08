#include "common.h"
#include <thread>

// Busca la ultima version publicada en GitHub Releases (sin cuenta ni clave) y la compara con la
// version de este ejecutable. Solo se consulta cuando el usuario abre la pantalla "Acerca de".

static UpdateInfo s_info;
static std::mutex s_mx;

// "v1.2.10" -> {1, 2, 10}
static std::vector<int> Parse(const std::wstring& v) {
  std::vector<int> r;
  int cur = -1;
  for (wchar_t c : v) {
    if (iswdigit(c)) cur = (cur < 0 ? 0 : cur * 10) + (c - L'0');
    else if (cur >= 0) { r.push_back(cur); cur = -1; if (c != L'.') break; }
  }
  if (cur >= 0) r.push_back(cur);
  while (r.size() < 3) r.push_back(0);
  return r;
}

UpdateInfo Update_Get() {
  std::lock_guard<std::mutex> lk(s_mx);
  return s_info;
}

void Update_Check() {
  {
    std::lock_guard<std::mutex> lk(s_mx);
    if (s_info.state == 1) return;
    s_info.state = 1;
  }
  std::thread([] {
    UpdateInfo u;
    DWORD status = 0;
    std::string j = HttpGet(L"api.github.com", L"/repos/" DZ_REPO_OWNER L"/" DZ_REPO_NAME L"/releases/latest", &status);
    std::wstring tag = JStr(j, "tag_name");
    if (status == 200 && !tag.empty()) {
      u.latest = tag;
      u.url = JStr(j, "html_url");
      if (u.url.empty()) u.url = DZ_REPO_URL L"/releases";
      u.state = Parse(tag) > Parse(DZ_VERSION) ? 3 : 2;
    } else if (status == 404) {  // todavia no hay ninguna version publicada: esta es la mas nueva
      u.state = 2;
      u.url = DZ_REPO_URL L"/releases";
    } else {
      u.state = 4;
      u.url = DZ_REPO_URL L"/releases";
    }
    { std::lock_guard<std::mutex> lk(s_mx); s_info = u; }
    PostMessageW(g_msgWnd, WM_APP_UPDATE, 0, 0);
  }).detach();
}
