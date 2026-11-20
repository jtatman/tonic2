// Generic minimal plugin UI for tonic2 patches (DPF + NanoVG).
//
//   MAIN    up to 8 large knobs (tonic::ui::buildSurface: Main-placed params, macros, CC-bound params),
//           each with its value, a CC tag when bound, and a dot for macros. Empty slots stay visible,
//           so the surface matches an 8-knob MIDI controller.
//   DETAIL  every visible parameter as a slider.
//
// Mouse: drag a knob vertically (Shift = fine), drag or click a slider, scroll to nudge, double-click or
// right-click to reset to the default. Header buttons: LEARN (MIDI learn) and DETAIL/MAIN (pages).
// MIDI learn: in LEARN mode click a control, then move a knob on your MIDI controller; right-click a control
// to clear its CC. Bindings are saved with the project (plugin state "ccmap"). When the UI shares the
// plugin's process (direct access) it shows learned bindings live.
// For screenshots: TONIC2_UI_PAGE=detail opens the detail page, TONIC2_UI_PAGE=learn shows learn mode.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "DistrhoUI.hpp"
#include "TonicDpfPlugin.hpp"
#include "tonic/ui/ccmap.hpp"
#include "tonic/dsl/patch.hpp"
#include "tonic/ui/surface.hpp"

START_NAMESPACE_DISTRHO

class TonicDpfUI : public UI {
 public:
  using Build = std::function<void(tonic::dsl::Patch&)>;
  static constexpr int kW = 720, kH = 380;

  TonicDpfUI(const Build& build, const char* title) : UI(kW, kH), title_(title) {
    loadSharedResources();
    tonic::dsl::Patch p;
    build(p);
    layout_ = p.compile(48000.0, 256).layout;
    surface_ = tonic::ui::buildSurface(layout_);
    for (const auto& s : layout_.params) values_.push_back(s.def);
    ccLive_ = tonic::ui::emptyCcMap();
    for (const auto& b : layout_.ccBindings) if (b.cc >= 0 && b.cc < 128) ccLive_[static_cast<size_t>(b.cc)] = b.param;
    if (const char* page = std::getenv("TONIC2_UI_PAGE")) {
      detail_ = std::string(page) == "detail";
      if (std::string(page) == "learn") { learnMode_ = true; armed_ = surface_.main.empty() ? -1 : surface_.main[0]; }
    }
    const double s = getScaleFactor();
    if (s != 1.0) setSize(static_cast<uint>(kW * s), static_cast<uint>(kH * s));
    setGeometryConstraints(static_cast<uint>(kW * s), static_cast<uint>(kH * s), true);
  }

 protected:
  void parameterChanged(uint32_t index, float value) override {
    if (index < values_.size()) { values_[index] = value; repaint(); }
  }

  void onNanoDisplay() override {
    const float s = static_cast<float>(getScaleFactor());
    save();
    scale(s, s);
    layoutControls();
    box(0, 0, kW, kH, kBg, 0);
    fontFace(NANOVG_DEJAVU_SANS_TTF);
    label(24, 32, title_, 20, kInk, ALIGN_LEFT | ALIGN_MIDDLE);
    const char* sub = learnMode_ ? (armed_ >= 0 ? "now move a knob on your MIDI controller" : "click a control to learn its CC, right-click to clear")
                                 : (detail_ ? "all parameters" : "macros and main controls");
    label(24, 52, sub, 11, learnMode_ ? kAccent[0] : kMute, ALIGN_LEFT | ALIGN_MIDDLE);
    box(kW - 120, 16, 96, 30, hoverButton_ ? kInk : kPanel, 6);
    label(kW - 72, 31, detail_ ? "MAIN" : "DETAIL", 12, hoverButton_ ? kPanel : kInk, ALIGN_CENTER | ALIGN_MIDDLE);
    box(kW - 228, 16, 96, 30, learnMode_ ? kAccent[0] : (hoverLearn_ ? kInk : kPanel), 6);
    label(kW - 180, 31, "LEARN", 12, learnMode_ || hoverLearn_ ? kPanel : kInk, ALIGN_CENTER | ALIGN_MIDDLE);
    const int show = active_ >= 0 ? active_ : hover_;
    if (!learnMode_ && show >= 0 && ctls_[show].param >= 0) {
      const auto& spec = layout_.params[static_cast<size_t>(ctls_[show].param)];
      const std::string t = upper(spec.name) + "  " + tonic::ui::formatValue(spec, values_[static_cast<size_t>(ctls_[show].param)]);
      label(kW - 244, 31, t, 12, kAccent[0], ALIGN_RIGHT | ALIGN_MIDDLE);
    }
    for (size_t i = 0; i < ctls_.size(); ++i) {
      const bool hot = static_cast<int>(i) == active_ || static_cast<int>(i) == hover_;
      if (ctls_[i].knob) drawKnob(ctls_[i], hot);
      else drawSlider(ctls_[i], hot);
    }
    restore();
  }

  bool onMouse(const MouseEvent& ev) override {
    const float s = static_cast<float>(getScaleFactor());
    const float mx = static_cast<float>(ev.pos.getX()) / s, my = static_cast<float>(ev.pos.getY()) / s;
    if (ev.press && inButton(mx, my)) { detail_ = !detail_; hover_ = active_ = -1; repaint(); return true; }
    if (ev.press && inLearnButton(mx, my)) {
      learnMode_ = !learnMode_;
      if (!learnMode_ && armed_ >= 0) { armed_ = -1; setState("learn", "-1"); }
      repaint();
      return true;
    }
    const int i = hit(mx, my);
    if (learnMode_) {  // learn mode: left click arms MIDI learn, right click clears the control's CCs
      if (!ev.press || i < 0 || ctls_[i].param < 0) return i >= 0;
      const int p = ctls_[i].param;
      if (ev.button == 1) {
        armed_ = p;
        setState("learn", std::to_string(p).c_str());
      } else if (ev.button == 3) {
        for (auto& c : ccLive_) if (c == p) c = -1;
        if (auto* pl = plugin()) for (int cc = 0; cc < 128; ++cc) if (pl->ccMapping(cc) == p) pl->mapCc(cc, -1);
        setState("ccmap", tonic::ui::serializeCcMap(ccLive_).c_str());
      }
      repaint();
      return true;
    }
    if (ev.button == 3 && ev.press) {  // right click: reset
      if (i >= 0 && ctls_[i].param >= 0) commit(ctls_[i].param, layout_.params[static_cast<size_t>(ctls_[i].param)].def);
      return i >= 0;
    }
    if (ev.button != 1) return false;
    if (ev.press) {
      if (i < 0 || ctls_[i].param < 0) return false;
      const int p = ctls_[i].param;
      if (i == lastClick_ && ev.time - lastClickTime_ < 350) {  // double click: reset
        commit(p, layout_.params[static_cast<size_t>(p)].def);
        lastClick_ = -1;
        return true;
      }
      lastClick_ = i;
      lastClickTime_ = ev.time;
      active_ = i;
      dragY_ = my;
      dragStart_ = norm(p);
      fine_ = (ev.mod & kModifierShift) != 0;
      editParameter(static_cast<uint32_t>(p), true);
      if (!ctls_[i].knob) setFromSlider(ctls_[i], mx);
      repaint();
      return true;
    }
    if (active_ >= 0) {
      editParameter(static_cast<uint32_t>(ctls_[active_].param), false);
      active_ = -1;
      repaint();
      return true;
    }
    return false;
  }

  bool onMotion(const MotionEvent& ev) override {
    const float s = static_cast<float>(getScaleFactor());
    const float mx = static_cast<float>(ev.pos.getX()) / s, my = static_cast<float>(ev.pos.getY()) / s;
    if (active_ < 0) {
      const int h = hit(mx, my);
      const bool hb = inButton(mx, my), hl = inLearnButton(mx, my);
      if (h != hover_ || hb != hoverButton_ || hl != hoverLearn_) { hover_ = h; hoverButton_ = hb; hoverLearn_ = hl; repaint(); }
      return false;
    }
    const Ctl& c = ctls_[active_];
    if (!c.knob) { setFromSlider(c, mx); return true; }
    const float t = dragStart_ + (dragY_ - my) / (fine_ ? 600.f : 150.f);
    setValue(c.param, tonic::ui::fromNormalized(spec(c.param), t));
    return true;
  }

  // Poll the plugin (direct access) for learned bindings; persist them once a learn completes.
  void uiIdle() override {
    auto* pl = plugin();
    if (!pl) return;
    bool changed = false;
    for (int cc = 0; cc < 128; ++cc) {
      const int p = pl->ccMapping(cc);
      if (ccLive_[static_cast<size_t>(cc)] != p) { ccLive_[static_cast<size_t>(cc)] = p; changed = true; }
    }
    if (armed_ >= 0 && !pl->isLearning() && !screenshotMode()) {
      armed_ = -1;
      setState("ccmap", tonic::ui::serializeCcMap(ccLive_).c_str());  // host saves it with the project
      changed = true;
    }
    if (changed) repaint();
  }

  void stateChanged(const char* key, const char* value) override {
    if (std::string(key) == "ccmap") { ccLive_ = tonic::ui::parseCcMap(value, static_cast<int>(layout_.params.size())); repaint(); }
  }

  bool onScroll(const ScrollEvent& ev) override {
    const float s = static_cast<float>(getScaleFactor());
    const int i = hit(static_cast<float>(ev.pos.getX()) / s, static_cast<float>(ev.pos.getY()) / s);
    if (i < 0 || ctls_[i].param < 0) return false;
    const int p = ctls_[i].param;
    commit(p, tonic::ui::fromNormalized(spec(p), norm(p) + static_cast<float>(ev.delta.getY()) * 0.02f));
    return true;
  }

 private:
  struct Col { int r, g, b; };
  struct Ctl { int param; float x, y, w, h; bool knob; int slot; };
  static constexpr Col kBg{232, 231, 226}, kPanel{246, 245, 241}, kInk{28, 28, 30}, kMute{150, 148, 142},
                       kLine{205, 203, 196}, kEmpty{222, 220, 213};
  static constexpr Col kAccent[4] = {{255, 98, 20}, {40, 110, 255}, {0, 168, 112}, {60, 60, 64}};

  const tonic::engine::ParamSpec& spec(int p) const { return layout_.params[static_cast<size_t>(p)]; }
  float norm(int p) const { return tonic::ui::toNormalized(spec(p), values_[static_cast<size_t>(p)]); }
  static std::string upper(std::string s) { for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return s; }

  void layoutControls() {
    ctls_.clear();
    if (!detail_) {  // 4 x 2 knob grid; empty slots have param -1
      for (int slot = 0; slot < 8; ++slot) {
        const int p = slot < static_cast<int>(surface_.main.size()) ? surface_.main[static_cast<size_t>(slot)] : -1;
        ctls_.push_back({p, 20.f + (slot % 4) * 170.f, 74.f + (slot / 4) * 150.f, 170.f, 150.f, true, slot});
      }
      return;
    }
    std::vector<int> all = surface_.main;
    all.insert(all.end(), surface_.detail.begin(), surface_.detail.end());
    for (size_t k = 0; k < all.size(); ++k) {
      const int col = static_cast<int>(k) / 8, row = static_cast<int>(k) % 8;
      if (col > 1) break;  // two columns of eight
      ctls_.push_back({all[k], 24.f + col * 344.f, 80.f + row * 36.f, 328.f, 30.f, false, static_cast<int>(k)});
    }
  }

  void drawKnob(const Ctl& c, bool hot) {
    const float cx = c.x + c.w * 0.5f, cy = c.y + 54.f, r = 36.f;
    const float a0 = 0.75f * static_cast<float>(M_PI), sweep = 1.5f * static_cast<float>(M_PI);
    if (c.param < 0) {  // empty slot
      beginPath(); circle(cx, cy, r); strokeColor(rgb(kLine)); strokeWidth(2.f); stroke();
      label(cx, cy, "-", 14, kLine, ALIGN_CENTER | ALIGN_MIDDLE);
      return;
    }
    const Col accent = kAccent[c.slot % 4];
    const float t = norm(c.param);
    beginPath(); circle(cx, cy, r + 8.f); fillColor(rgb(hot ? kPanel : kBg)); fill();
    beginPath(); arc(cx, cy, r, a0, a0 + sweep, CW); strokeColor(rgb(kEmpty)); strokeWidth(7.f); lineCap(ROUND); stroke();
    if (t > 0.001f) { beginPath(); arc(cx, cy, r, a0, a0 + sweep * t, CW); strokeColor(rgb(accent)); strokeWidth(7.f); stroke(); }
    beginPath(); circle(cx, cy, r - 9.f); fillColor(rgb(kInk)); fill();
    const float a = a0 + sweep * t;
    beginPath(); moveTo(cx + std::cos(a) * (r - 22.f), cy + std::sin(a) * (r - 22.f));
    lineTo(cx + std::cos(a) * (r - 11.f), cy + std::sin(a) * (r - 11.f));
    strokeColor(rgb(kPanel)); strokeWidth(3.f); stroke();
    if (tonic::ui::isMacro(layout_, c.param)) { beginPath(); circle(cx + r + 4.f, cy - r + 2.f, 4.f); fillColor(rgb(accent)); fill(); }
    label(cx, c.y + 108.f, upper(spec(c.param).name), 12, kInk, ALIGN_CENTER | ALIGN_MIDDLE);
    label(cx, c.y + 124.f, tonic::ui::formatValue(spec(c.param), values_[static_cast<size_t>(c.param)]), 11, kMute,
          ALIGN_CENTER | ALIGN_MIDDLE);
    const std::string ccs = ccLabel(c.param);
    if (!ccs.empty()) label(cx, c.y + 139.f, ccs, 9, accent, ALIGN_CENTER | ALIGN_MIDDLE);
    if (learnMode_ && c.param == armed_) {  // armed for MIDI learn
      beginPath(); circle(cx, cy, r + 12.f); strokeColor(rgb(kAccent[0])); strokeWidth(2.5f); stroke();
      label(cx, c.y + 139.f, "MOVE A KNOB", 9, kAccent[0], ALIGN_CENTER | ALIGN_MIDDLE);
    }
  }

  void drawSlider(const Ctl& c, bool hot) {
    const Col accent = kAccent[(c.slot < 8 ? c.slot : 3) % 4];
    box(c.x, c.y, c.w, c.h, hot ? kPanel : kBg, 4);
    label(c.x + 8.f, c.y + c.h * 0.5f, upper(spec(c.param).name), 11, kInk, ALIGN_LEFT | ALIGN_MIDDLE);
    const float bx = c.x + 120.f, bw = 120.f, by = c.y + c.h * 0.5f - 3.f;
    box(bx, by, bw, 6.f, kEmpty, 3);
    box(bx, by, bw * norm(c.param), 6.f, accent, 3);
    label(c.x + c.w - 8.f, c.y + c.h * 0.5f, tonic::ui::formatValue(spec(c.param), values_[static_cast<size_t>(c.param)]), 11,
          kMute, ALIGN_RIGHT | ALIGN_MIDDLE);
  }

  void setFromSlider(const Ctl& c, float mx) {
    setValue(c.param, tonic::ui::fromNormalized(spec(c.param), (mx - (c.x + 120.f)) / 120.f));
  }
  void setValue(int p, float v) {
    values_[static_cast<size_t>(p)] = v;
    setParameterValue(static_cast<uint32_t>(p), v);
    repaint();
  }
  void commit(int p, float v) {
    editParameter(static_cast<uint32_t>(p), true);
    setValue(p, v);
    editParameter(static_cast<uint32_t>(p), false);
  }
  int hit(float x, float y) const {
    for (size_t i = 0; i < ctls_.size(); ++i)
      if (x >= ctls_[i].x && x < ctls_[i].x + ctls_[i].w && y >= ctls_[i].y && y < ctls_[i].y + ctls_[i].h) return static_cast<int>(i);
    return -1;
  }
  static bool inButton(float x, float y) { return x >= kW - 120 && x < kW - 24 && y >= 16 && y < 46; }
  static bool inLearnButton(float x, float y) { return x >= kW - 228 && x < kW - 132 && y >= 16 && y < 46; }
  static bool screenshotMode() { const char* p = std::getenv("TONIC2_UI_PAGE"); return p && std::string(p) == "learn"; }
  TonicDpfPlugin* plugin() const {
#if DISTRHO_PLUGIN_WANT_DIRECT_ACCESS
    return static_cast<TonicDpfPlugin*>(getPluginInstancePointer());
#else
    return nullptr;
#endif
  }
  // "CC 74" (or "CC 74 +1" when several CCs drive the parameter), from the live map.
  std::string ccLabel(int param) const {
    int first = -1, more = 0;
    for (int cc = 0; cc < 128; ++cc)
      if (ccLive_[static_cast<size_t>(cc)] == param) { if (first < 0) first = cc; else ++more; }
    if (first < 0) return {};
    return "CC " + std::to_string(first) + (more ? " +" + std::to_string(more) : "");
  }

  Color rgb(const Col& c) const { return Color(c.r, c.g, c.b); }
  void box(float x, float y, float w, float h, const Col& c, float radius) {
    beginPath();
    if (radius > 0.f) roundedRect(x, y, w, h, radius); else rect(x, y, w, h);
    fillColor(rgb(c));
    fill();
  }
  void label(float x, float y, const std::string& s, float size, const Col& c, int align) {
    fontSize(size);
    fillColor(rgb(c));
    textAlign(align);
    text(x, y, s.c_str(), nullptr);
  }

  std::string title_;
  tonic::engine::Layout layout_;
  tonic::ui::Surface surface_;
  std::vector<float> values_;
  std::vector<Ctl> ctls_;
  bool detail_ = false, fine_ = false, hoverButton_ = false, hoverLearn_ = false, learnMode_ = false;
  int armed_ = -1;
  tonic::ui::CcMap ccLive_{};
  int active_ = -1, hover_ = -1, lastClick_ = -1;
  uint lastClickTime_ = 0;
  float dragY_ = 0.f, dragStart_ = 0.f;

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TonicDpfUI)
};

END_NAMESPACE_DISTRHO
