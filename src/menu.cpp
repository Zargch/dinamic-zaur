#include "common.h"

// Menu emergente de vidrio. No toma el foco (WS_EX_NOACTIVATE) para que los atajos que envia
// lleguen a la app activa; los clics afuera y el teclado se detectan con hooks de bajo nivel
// que solo existen mientras el menu esta abierto.
class PopupMenu : public Surface {
public:
  std::vector<MenuItem> items;
  std::function<void()> onClose;
  int hover = -1;
  const float pad = 5, itemH = 24, sepH = 9, headH = 22, gutter = 22;

  float ItemHeight(const MenuItem& m) const { return m.sep ? sepH : m.header ? headH : itemH; }

  int HitTest(float my) const {
    float y = pad;
    for (size_t i = 0; i < items.size(); i++) {
      float ih = ItemHeight(items[i]);
      if (my >= y && my < y + ih) return (items[i].sep || items[i].header || items[i].disabled) ? -1 : (int)i;
      y += ih;
    }
    return -1;
  }

  void Paint(ID2D1RenderTarget* rt) override {
    D2D1_RECT_F all{ 0, 0, Wd(), Hd() };
    DrawGlass(rt, all, radius, .15f);
    float y = pad;
    auto f = Font(13.5f), fb = Font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    for (size_t i = 0; i < items.size(); i++) {
      auto& m = items[i];
      float ih = ItemHeight(m);
      if (m.sep) {
        rt->FillRectangle({ 14, y + ih / 2 - .5f, Wd() - 14, y + ih / 2 + .5f }, Brush(g_th.sep));
      } else {
        bool hot = (int)i == hover;
        if (hot) FillRound(rt, { pad, y, Wd() - pad, y + ih }, 6, g_th.accent);
        D2D1_COLOR_F c = m.header ? g_th.textDim : m.disabled ? g_th.textDim : hot ? RGBA(255, 255, 255) : g_th.text;
        if (m.checked) DrawText(rt, L"✓", f, { pad + 4, y, pad + gutter, y + ih }, c);
        DrawText(rt, m.text, m.header ? fb : f, { gutter + 4, y, Wd() - 14, y + ih }, c);
        if (!m.shortcut.empty())
          DrawText(rt, m.shortcut, f, { gutter, y, Wd() - 14, y + ih }, hot ? RGBA(255, 255, 255, .8f) : g_th.textDim, AL_RIGHT);
      }
      y += ih;
    }
  }

  void Activate(int i) {
    if (i < 0 || i >= (int)items.size() || items[i].sep || items[i].disabled || items[i].header) return;
    auto act = items[i].action;
    CloseMenu();
    if (act) act();
  }

  void Move(int dir) {
    int n = (int)items.size(), i = hover;
    for (int k = 0; k < n; k++) {
      i = (i + dir + n) % n;
      if (!items[i].sep && !items[i].disabled && !items[i].header) { hover = i; Invalidate(); return; }
    }
  }

  LRESULT Handle(UINT m, WPARAM wp, LPARAM lp) override {
    switch (m) {
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, hwnd };
      TrackMouseEvent(&t);
      int hv = HitTest(Dip(GET_Y_LPARAM(lp)));
      if (hv != hover) { hover = hv; Invalidate(); }
      return 0;
    }
    case WM_MOUSELEAVE: hover = -1; Invalidate(); return 0;
    case WM_LBUTTONUP: Activate(HitTest(Dip(GET_Y_LPARAM(lp)))); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return Surface::Handle(m, wp, lp);
  }
};

static PopupMenu* s_menu = nullptr;
static HHOOK s_mouseHook, s_keyHook;

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && s_menu && s_menu->Visible() &&
      (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
    POINT pt = ((MSLLHOOKSTRUCT*)lp)->pt;
    RECT r;
    GetWindowRect(s_menu->hwnd, &r);
    HWND under = WindowFromPoint(pt);
    // los clics sobre nuestras propias ventanas (barra de menu, dock) los resuelven ellas
    if (!PtInRect(&r, pt) && !(under && IsOwnWindow(under))) PostMessageW(g_msgWnd, WM_APP_RENDER, 1, 0);
  }
  return CallNextHookEx(s_mouseHook, code, wp, lp);
}

static LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && s_menu && s_menu->Visible() && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
    DWORD vk = ((KBDLLHOOKSTRUCT*)lp)->vkCode;
    switch (vk) {
    case VK_ESCAPE: PostMessageW(g_msgWnd, WM_APP_RENDER, 1, 0); return 1;
    case VK_UP: s_menu->Move(-1); return 1;
    case VK_DOWN: s_menu->Move(1); return 1;
    case VK_RETURN: PostMessageW(g_msgWnd, WM_APP_RENDER, 2, 0); return 1;
    }
  }
  return CallNextHookEx(s_keyHook, code, wp, lp);
}

// llamado desde la ventana de mensajes (para no cerrar el menu dentro del hook)
void Menu_OnMessage(WPARAM what) {
  if (!s_menu) return;
  if (what == 1) CloseMenu();
  else if (what == 2) s_menu->Activate(s_menu->hover);
}

void ShowMenu(std::vector<MenuItem> items, int x, int y, bool above, std::function<void()> onClose) {
  CloseMenu();
  if (!s_menu) {
    s_menu = new PopupMenu;
    s_menu->radius = 8;
    s_menu->dwmRound = true;
    s_menu->Create(WS_EX_TOPMOST | WS_EX_NOACTIVATE, false, true);
  }
  PopupMenu& m = *s_menu;
  m.items = std::move(items);
  m.onClose = onClose;
  m.hover = -1;
  float wmax = 180, hsum = m.pad * 2;
  for (auto& it : m.items) {
    hsum += m.ItemHeight(it);
    if (!it.sep) wmax = std::max(wmax, m.gutter + 4 + TextWidth(it.text, Font(13.5f)) + (it.shortcut.empty() ? 0 : 36 + TextWidth(it.shortcut, Font(13.5f))) + 16);
  }
  int w = Px(wmax), h = Px(hsum);
  RECT mon = MonitorRect();
  if (above) y -= h;
  x = std::max((int)mon.left + 4, std::min(x, (int)mon.right - w - 4));
  y = std::max((int)mon.top, std::min(y, (int)mon.bottom - h - 4));
  m.SetBounds(x, y, w, h, HWND_TOPMOST);
  m.Show(true);
  if (!s_mouseHook) s_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_inst, 0);
  if (!s_keyHook) s_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, g_inst, 0);
}

void CloseMenu() {
  if (s_mouseHook) { UnhookWindowsHookEx(s_mouseHook); s_mouseHook = nullptr; }
  if (s_keyHook) { UnhookWindowsHookEx(s_keyHook); s_keyHook = nullptr; }
  if (!s_menu || !s_menu->Visible()) return;
  s_menu->Show(false);
  auto cb = s_menu->onClose;
  s_menu->onClose = nullptr;
  if (cb) cb();
}

bool MenuOpen() { return s_menu && s_menu->Visible(); }
HWND MenuHwnd() { return s_menu ? s_menu->hwnd : nullptr; }
