#include "dock.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <cmath>
#include <filesystem>

namespace {
inline constexpr auto EditorClass = L"Vetra.Editor";

Text trim(const Text& value) {
    auto start = value.find_first_not_of(L" \t\r\n");
    return start == Text::npos ? Text() : value.substr(start, value.find_last_not_of(L" \t\r\n") - start + 1);
}

Text clipboardRead(HWND owner) {
    Text out;
    if (!OpenClipboard(owner)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* ptr = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = ptr;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void clipboardWrite(HWND owner, const Text& text) {
    if (text.empty() || !OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (auto* dst = static_cast<wchar_t*>(GlobalLock(mem))) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else GlobalFree(mem);
    }
    CloseClipboard();
}

struct ModalState {
    Dock& dock;
    Text title, error;
    std::vector<Field> fields;
    bool canDelete;
    const std::function<Text(const std::vector<Field>&)>& validate;
    View view;
    std::vector<Box> fieldBoxes;
    int active = 0, caret = 0, anchor = 0;
    float scrollX = 0, scrollY = 0;
    bool caretOn = true, draggingText = false, draggingWin = false, done = false;
    POINT dragCursor{}, dragOrigin{};
    POINT openFrom{}, openDest{};
    double openAnimMs = 0.0, lastHoverMs = 0.0;
    int result = 0;

    float contentHeight() const {
        float y = 78.f;
        for (const auto& f : fields) {
            const float bh = f.multiline ? 136.f : (f.choices.empty() ? 42.f : 38.f);
            y += 24.f + bh + 16.f;
        }
        if (!error.empty()) y += 44.f;
        return y + 66.f;
    }

    int firstTextField(int start = 0, int step = 1) const {
        const int n = static_cast<int>(fields.size());
        for (int k = 0; k < n; ++k) {
            const int i = ((start + k * step) % n + n) % n;
            if (fields[i].choices.empty()) return i;
        }
        return -1;
    }

    IDWriteTextLayout* buildLayout(int index, float innerW) const {
        const auto& f = fields[index];
        Text disp = f.password ? Text(f.value.size(), L'•') : f.value;
        IDWriteTextLayout* layout{};
        if (FAILED(Draw::writing->CreateTextLayout(disp.c_str(), static_cast<UINT32>(disp.size()),
                Draw::textFormats[Body], f.multiline ? innerW : 10000.f, f.multiline ? 4000.f : 32.f, &layout)))
            return nullptr;
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(f.multiline ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layout->SetWordWrapping(f.multiline ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        return layout;
    }

    void clampCaret() {
        if (active < 0 || active >= static_cast<int>(fields.size()) || !fields[active].choices.empty()) return;
        const int len = static_cast<int>(fields[active].value.size());
        caret = std::clamp(caret, 0, len);
        anchor = std::clamp(anchor, 0, len);
    }

    void ensureCaretVisible() {
        if (active < 0 || active >= static_cast<int>(fieldBoxes.size()) || !fields[active].choices.empty()) return;
        const Box fb = fieldBoxes[active];
        const float innerW = width(fb) - 24.f, innerH = height(fb) - 16.f;
        auto* layout = buildLayout(active, innerW);
        if (!layout) return;
        float cx = 0, cy = 0;
        DWRITE_HIT_TEST_METRICS m{};
        layout->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &cx, &cy, &m);
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        layout->Release();
        if (fields[active].multiline) {
            scrollX = 0;
            if (cy < scrollY) scrollY = cy;
            else if (cy + m.height > scrollY + innerH) scrollY = cy + m.height - innerH;
            scrollY = std::clamp(scrollY, 0.f, std::max(0.f, tm.height - innerH));
        } else {
            scrollY = 0;
            if (cx < scrollX) scrollX = cx;
            else if (cx + 4 > scrollX + innerW) scrollX = cx + 4 - innerW;
            scrollX = std::clamp(scrollX, 0.f, std::max(0.f, tm.width - innerW));
        }
    }

    void replaceSelection(const Text& insert) {
        if (active < 0 || !fields[active].choices.empty()) return;
        auto& val = fields[active].value;
        clampCaret();
        const int s0 = std::min(caret, anchor), s1 = std::max(caret, anchor);
        const int limit = fields[active].multiline ? 4096 : 1024;
        Text clean = insert;
        if (!fields[active].multiline) {
            clean.erase(std::remove_if(clean.begin(), clean.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }), clean.end());
        } else {
            Text norm;
            for (size_t i = 0; i < clean.size(); ++i) {
                if (clean[i] == L'\r') {
                    norm += L'\n';
                    if (i + 1 < clean.size() && clean[i + 1] == L'\n') ++i;
                } else norm += clean[i];
            }
            clean = std::move(norm);
        }
        const int room = std::max(0, limit - static_cast<int>(val.size() - (s1 - s0)));
        if (static_cast<int>(clean.size()) > room) clean.resize(room);
        val.replace(s0, s1 - s0, clean);
        caret = anchor = s0 + static_cast<int>(clean.size());
        caretOn = true;
        ensureCaretVisible();
    }

    void selectField(int index, bool selectAll = false) {
        if (index < 0 || index >= static_cast<int>(fields.size())) return;
        active = index;
        scrollX = scrollY = 0;
        if (fields[index].choices.empty()) {
            const int len = static_cast<int>(fields[index].value.size());
            caret = len;
            anchor = selectAll && !fields[index].multiline ? 0 : len;
            ensureCaretVisible();
        }
        caretOn = true;
    }

    int hitTextPosition(int index, float dipX, float dipY) {
        if (index < 0 || index >= static_cast<int>(fieldBoxes.size())) return 0;
        const Box fb = fieldBoxes[index];
        const float innerW = width(fb) - 24.f;
        auto* layout = buildLayout(index, innerW);
        if (!layout) return 0;
        const float lx = dipX - (fb.left + 12.f) + (index == active ? scrollX : 0.f);
        const float ly = dipY - (fb.top + (fields[index].multiline ? 8.f : 5.f)) + (index == active ? scrollY : 0.f);
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        layout->HitTestPoint(lx, ly, &trailing, &inside, &m);
        layout->Release();
        return static_cast<int>(m.textPosition + (trailing ? 1 : 0));
    }

    void resizeToFit() {
        const float nextH = contentHeight();
        if (std::fabs(nextH - view.h) < 0.5f) return;
        view.h = nextH;
        const int pw = static_cast<int>(std::lround(view.w * view.scale));
        const int ph = static_cast<int>(std::lround(view.h * view.scale));
        RECT wr{};
        GetWindowRect(view.hwnd, &wr);
        const int x = std::clamp(static_cast<int>(wr.left), static_cast<int>(dock.work.left + 10),
            std::max<int>(dock.work.left + 10, dock.work.right - pw - 10));
        const int y = std::clamp(static_cast<int>(wr.top), static_cast<int>(dock.work.top + 10),
            std::max<int>(dock.work.top + 10, dock.work.bottom - ph - 10));
        SetWindowPos(view.hwnd, nullptr, x, y, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void submit() {
        auto entered = fields;
        for (auto& f : entered) {
            if (!f.choices.empty()) continue;
            if (!f.multiline && !f.password) f.value = trim(f.value);
        }
        const Text err = validate ? validate(entered) : Text{};
        if (!err.empty()) {
            error = err;
            resizeToFit();
            paint();
            return;
        }
        fields = std::move(entered);
        result = 1;
        done = true;
    }

    void paint() {
        auto& d = view.d;
        const int pw = static_cast<int>(std::lround(view.w * view.scale));
        const int ph = static_cast<int>(std::lround(view.h * view.scale));
        if (!view.hwnd || !d.begin(view.hwnd, pw, ph, view.scale)) return;
        view.hits.clear();
        fieldBoxes.assign(fields.size(), {});
        const Palette p(dock.model);
        const Box surface = box(6, 4, view.w - 12, view.h - 12);
        p.surface(d, surface, 22);

        d.text(title, box(28, 21, view.w - 100, 32), Value, tint(p.ink));
        dock.iconButton(view, p, box(view.w - 58, 21, 32, 32), Close, [this] { result = 0; done = true; });
        d.line(28, 63, view.w - 28, 63, tint(p.ink, .085f));

        float y = 78.f;
        for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
            const auto& f = fields[i];
            d.text(f.label, box(29, y, view.w - 58, 20), Caption, tint(p.muted));
            const float bw = view.w - 56.f;
            if (!f.choices.empty()) {
                const Box track = box(28, y + 24, bw, 38);
                fieldBoxes[i] = track;
                d.fill(track, 11, tint(p.ink, p.light ? .06f : .08f));
                d.outline(track, 11, tint(p.ink, .10f), .85f);
                const int n = static_cast<int>(f.choices.size());
                const int sel = std::clamp(_wtoi(f.value.c_str()) - 1, 0, n - 1);
                const float segW = (bw - 6.f) / n;
                for (int c = 0; c < n; ++c) {
                    const Box seg = box(31.f + c * segW, y + 27.f, segW, 32.f);
                    const bool hot = dock.hit(view, seg, [this, i, c] {
                        fields[i].value = num(c + 1);
                        active = i;
                        paint();
                    });
                    if (c == sel) {
                        d.fill(seg, 13.f, tint(p.high, .96f));
                        d.gradientOutline({seg.left + .4f, seg.top + .4f, seg.right - .4f, seg.bottom - .4f},
                            8.f, tint(0xFFFFFF, p.light ? .85f : .26f), tint(0xFFFFFF, .06f), .8f);
                    } else if (hot) {
                        d.fill(seg, 8.5f, tint(p.ink, .06f));
                    }
                    d.text(f.choices[c], seg, c == sel ? Strong : Body, tint(c == sel ? p.ink : p.muted), Center);
                }
                y += 24.f + 38.f + 16.f;
            } else {
                const float bh = f.multiline ? 136.f : 42.f;
                const Box fb = box(28, y + 24, bw, bh);
                fieldBoxes[i] = fb;
                const bool isActive = (active == i);
                d.fill(fb, 14, tint(p.high, isActive ? .94f : .68f));
                d.outline(fb, 12, tint(isActive ? p.accent : p.ink, isActive ? .92f : (p.light ? .16f : .14f)), isActive ? 1.5f : .9f);

                const float padX = 12.f, padY = f.multiline ? 8.f : 5.f;
                const Box inner = box(fb.left + padX, fb.top + padY, bw - 2 * padX, bh - 2 * padY);
                if (auto* layout = buildLayout(i, width(inner))) {
                    d.clip(inner);
                    const float ox = inner.left - (isActive ? scrollX : 0.f);
                    const float oy = inner.top - (isActive ? scrollY : 0.f);
                    if (isActive && caret != anchor) {
                        const int s0 = std::min(caret, anchor), s1 = std::max(caret, anchor);
                        UINT32 count = 0;
                        layout->HitTestTextRange(static_cast<UINT32>(s0), static_cast<UINT32>(s1 - s0), 0, 0, nullptr, 0, &count);
                        if (count) {
                            std::vector<DWRITE_HIT_TEST_METRICS> rects(count);
                            if (SUCCEEDED(layout->HitTestTextRange(static_cast<UINT32>(s0), static_cast<UINT32>(s1 - s0), 0, 0, rects.data(), count, &count))) {
                                for (UINT32 k = 0; k < count; ++k) {
                                    const auto& r = rects[k];
                                    d.fill(box(ox + r.left, oy + r.top, std::max(4.f, r.width), r.height), 4.f, tint(p.accent, .34f));
                                }
                            }
                        }
                    }
                    const Color inkCol = tint(p.ink);
                    d.brush->SetColor({inkCol.GetR() / 255.f, inkCol.GetG() / 255.f, inkCol.GetB() / 255.f, inkCol.GetA() / 255.f});
                    d.target->DrawTextLayout({ox, oy}, layout, d.brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    if (isActive && caretOn) {
                        float cx = 0, cy = 0;
                        DWRITE_HIT_TEST_METRICS cm{};
                        if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &cx, &cy, &cm))) {
                            const float ch = std::max(16.f, cm.height);
                            d.fill(box(ox + cx - .5f, oy + cy, 1.8f, ch), .9f, tint(p.accent));
                        }
                    }
                    d.unclip();
                    layout->Release();
                }
                y += 24.f + bh + 16.f;
            }
        }

        if (!error.empty()) {
            const Box eb = box(28, y, view.w - 56, 34);
            d.fill(eb, 10, tint(0xFF453A, .16f));
            d.outline(eb, 10, tint(0xFF453A, .42f), .9f);
            d.text(error, box(eb.left + 12, eb.top, width(eb) - 24, height(eb)), Caption, tint(p.light ? 0xC62828 : 0xFF7A72));
            y += 44.f;
        }

        const float fy = y + 4.f;
        if (canDelete) {
            const Box db = box(28, fy, 84, 38);
            const bool hot = dock.hit(view, db, [this] { result = 2; done = true; });
            d.fill(db, 11, tint(0xFF453A, hot ? .26f : .16f));
            d.outline(db, 11, tint(0xFF453A, .36f), .85f);
            d.text(L"Sil", db, Body, tint(p.light ? 0xC62828 : 0xFF6E66), Center);
        }
        dock.button(view, p, box(view.w - 234, fy, 96, 38), L"Vazgeç", [this] { result = 0; done = true; }, false, Body);
        dock.button(view, p, box(view.w - 130, fy, 102, 38), L"Kaydet", [this] { submit(); }, true, Body);
        d.end();
    }
};

static LRESULT CALLBACK modalProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<ModalState*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        s = static_cast<ModalState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        return TRUE;
    }
    if (!s) return DefWindowProcW(w, msg, wp, lp);
    auto moveCaret = [&](int next, bool extend) {
        s->caret = next;
        if (!extend) s->anchor = next;
        s->clampCaret();
        s->caretOn = true;
        s->ensureCaretVisible();
        s->paint();
    };
    switch (msg) {
    case WM_TIMER:
        if (wp == 1) { s->caretOn = !s->caretOn; s->paint(); }
        if (wp == 2) {
            const double nowMs = animNowMs();
            const float dt = s->lastHoverMs > 0.0 ? std::clamp(static_cast<float>((nowMs - s->lastHoverMs) / 1000.0), 0.001f, 0.05f) : 0.016f;
            s->lastHoverMs = nowMs;
            bool active = false;
            if (s->openAnimMs > 0.0) {
                const float t = std::clamp(static_cast<float>((nowMs - s->openAnimMs) / 155.0), 0.f, 1.f);
                const float posE = easeOutQuint(t);
                const float alphaE = easeOutCubic(std::min(1.f, t * 1.4f));
                s->view.d.layerOpacity = static_cast<BYTE>(std::lround(255.f * (0.08f + 0.92f * alphaE)));
                const POINT pos{
                    static_cast<int>(std::lround(s->openFrom.x + (s->openDest.x - s->openFrom.x) * posE)),
                    static_cast<int>(std::lround(s->openFrom.y + (s->openDest.y - s->openFrom.y) * posE))
                };
                s->view.d.present(&pos);
                if (t >= 1.f) s->openAnimMs = 0.0;
                else active = true;
            }
            if (s->dock.advanceViewMotion(s->view, dt)) {
                s->paint();
                active = true;
            }
            if (!active) KillTimer(w, 2);
        }
        return 0;
    case WM_MOUSEMOVE: {
        const float dx = GET_X_LPARAM(lp) / s->view.scale, dy = GET_Y_LPARAM(lp) / s->view.scale;
        if (s->draggingWin) {
            POINT cur{};
            GetCursorPos(&cur);
            SetWindowPos(w, nullptr, s->dragOrigin.x + (cur.x - s->dragCursor.x),
                s->dragOrigin.y + (cur.y - s->dragCursor.y), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (s->draggingText && s->active >= 0 && s->fields[s->active].choices.empty()) {
            s->caret = s->hitTextPosition(s->active, dx, dy);
            s->clampCaret();
            s->ensureCaretVisible();
            s->paint();
            return 0;
        }
        const int hitIdx = s->view.at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        bool overText = false;
        for (int i = 0; i < static_cast<int>(s->fieldBoxes.size()); ++i) {
            const auto& fb = s->fieldBoxes[i];
            if (s->fields[i].choices.empty() && dx >= fb.left && dx < fb.right && dy >= fb.top && dy < fb.bottom) {
                overText = true;
                break;
            }
        }
        SetCursor(LoadCursorW(nullptr, overText ? IDC_IBEAM : hitIdx >= 0 ? IDC_HAND : IDC_ARROW));
        if (s->view.hover != hitIdx) {
            s->view.hover = hitIdx;
            if (s->dock.motionEnabled()) {
                s->lastHoverMs = animNowMs();
                SetTimer(w, 2, s->dock.animIntervalMs, nullptr);
            }
            s->paint();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const float dx = GET_X_LPARAM(lp) / s->view.scale, dy = GET_Y_LPARAM(lp) / s->view.scale;
        for (int i = 0; i < static_cast<int>(s->fieldBoxes.size()); ++i) {
            const auto& fb = s->fieldBoxes[i];
            if (s->fields[i].choices.empty() && dx >= fb.left && dx < fb.right && dy >= fb.top && dy < fb.bottom) {
                if (s->active != i) { s->active = i; s->scrollX = s->scrollY = 0; }
                s->caret = s->anchor = s->hitTextPosition(i, dx, dy);
                s->clampCaret();
                s->caretOn = true;
                s->draggingText = true;
                SetCapture(w);
                s->ensureCaretVisible();
                s->paint();
                return 0;
            }
        }
        if (dy < 64.f && s->view.at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) < 0) {
            RECT wr{};
            GetWindowRect(w, &wr);
            GetCursorPos(&s->dragCursor);
            s->dragOrigin = {wr.left, wr.top};
            s->draggingWin = true;
            SetCapture(w);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (s->draggingWin || s->draggingText) {
            s->draggingWin = s->draggingText = false;
            ReleaseCapture();
            return 0;
        }
        if (const int idx = s->view.at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); idx >= 0) {
            auto act = s->view.hits[idx].action;
            act();
        }
        return 0;
    case WM_CAPTURECHANGED:
        s->draggingWin = s->draggingText = false;
        return 0;
    case WM_MOUSEWHEEL:
        if (s->active >= 0 && s->fields[s->active].multiline) {
            s->scrollY = std::max(0.f, s->scrollY - (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 24.f : -24.f));
            s->paint();
        }
        return 0;
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wp == VK_ESCAPE) { s->result = 0; s->done = true; return 0; }
        if (wp == VK_TAB) {
            const int next = s->firstTextField(s->active + (shift ? -1 : 1), shift ? -1 : 1);
            if (next >= 0) { s->selectField(next, true); s->paint(); }
            return 0;
        }
        if (wp == VK_RETURN && (ctrl || s->active < 0 || !s->fields[s->active].multiline)) {
            s->submit();
            return 0;
        }
        if (s->active < 0 || !s->fields[s->active].choices.empty()) return 0;
        auto& val = s->fields[s->active].value;
        if (ctrl && wp == 'A') { s->anchor = 0; s->caret = static_cast<int>(val.size()); s->paint(); return 0; }
        if (ctrl && (wp == 'C' || wp == 'X')) {
            const int s0 = std::min(s->caret, s->anchor), s1 = std::max(s->caret, s->anchor);
            if (s1 > s0 && !s->fields[s->active].password) {
                clipboardWrite(w, val.substr(s0, s1 - s0));
                if (wp == 'X') { s->replaceSelection(L""); s->paint(); }
            }
            return 0;
        }
        if (ctrl && wp == 'V') { s->replaceSelection(clipboardRead(w)); s->paint(); return 0; }
        if (wp == VK_LEFT) { moveCaret(s->caret - 1, shift); return 0; }
        if (wp == VK_RIGHT) { moveCaret(s->caret + 1, shift); return 0; }
        if (wp == VK_HOME) { moveCaret(0, shift); return 0; }
        if (wp == VK_END) { moveCaret(static_cast<int>(val.size()), shift); return 0; }
        if (wp == VK_BACK) {
            if (s->caret != s->anchor) s->replaceSelection(L"");
            else if (s->caret > 0) { val.erase(s->caret - 1, 1); --s->caret; s->anchor = s->caret; s->ensureCaretVisible(); }
            s->caretOn = true; s->paint(); return 0;
        }
        if (wp == VK_DELETE) {
            if (s->caret != s->anchor) s->replaceSelection(L"");
            else if (s->caret < static_cast<int>(val.size())) val.erase(s->caret, 1);
            s->caretOn = true; s->paint(); return 0;
        }
        if ((wp == VK_UP || wp == VK_DOWN) && s->fields[s->active].multiline && s->active < static_cast<int>(s->fieldBoxes.size())) {
            const Box fb = s->fieldBoxes[s->active];
            const float innerW = width(fb) - 24.f;
            if (auto* layout = s->buildLayout(s->active, innerW)) {
                float cx = 0, cy = 0;
                DWRITE_HIT_TEST_METRICS cm{}, hm{};
                layout->HitTestTextPosition(static_cast<UINT32>(s->caret), FALSE, &cx, &cy, &cm);
                BOOL trailing = FALSE, inside = FALSE;
                layout->HitTestPoint(cx, cy + (wp == VK_UP ? -std::max(14.f, cm.height) : std::max(14.f, cm.height)), &trailing, &inside, &hm);
                layout->Release();
                moveCaret(static_cast<int>(hm.textPosition + (trailing ? 1 : 0)), shift);
            }
            return 0;
        }
        return 0;
    }
    case WM_CHAR: {
        if (s->active < 0 || !s->fields[s->active].choices.empty()) return 0;
        const wchar_t ch = static_cast<wchar_t>(wp);
        if (ch == L'\r' && s->fields[s->active].multiline) {
            s->replaceSelection(L"\n");
            s->paint();
        } else if (ch >= 32 && ch != 127) {
            s->replaceSelection(Text(1, ch));
            s->paint();
        }
        return 0;
    }
    case WM_CLOSE:
        s->result = 0; s->done = true;
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
long long integer(const Text& value, long long max) {
    if (value.empty() || value.size() > 14 || value.find_first_not_of(L"0123456789") != Text::npos) return -1;
    const auto result = _wtoi64(value.c_str());
    return result <= max ? result : -1;
}
int parseDay(const Text& text) {
    if (text.size() != 10 || text[4] != L'-' || text[7] != L'-') return 0;
    Date d{static_cast<int>(integer(text.substr(0, 4), 9999)), static_cast<int>(integer(text.substr(5, 2), 12)),
        static_cast<int>(integer(text.substr(8, 2), 31))};
    return d.year >= 1601 && d.month > 0 && d.day > 0 && d.day <= d.length() ? d.key() : 0;
}
Text isoDay(int day) {
    const Date d = Date::fromKey(day);
    return num(d.year) + L"-" + two(d.month) + L"-" + two(d.day);
}
long long cents(const Text& text) {
    auto found = text.find_first_of(L",.");
    auto lira = integer(text.substr(0, found), 999999999);
    if (lira < 0) return -1;
    if (found == Text::npos) return lira * 100;
    Text decimal = text.substr(found + 1);
    if (decimal.size() == 1) decimal += L"0";
    auto fraction = decimal.size() == 2 ? integer(decimal, 99) : -1;
    return fraction < 0 ? -1 : lira * 100 + fraction;
}
}
int Dock::edit(const Text& title, std::vector<Field>& fields, bool remove, const std::function<Text(const std::vector<Field>&)>& validate) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.lpfnWndProc = modalProc;
        wc.lpszClassName = EditorClass;
        wc.hIcon = appIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) { report(L"Düzenleyici açılamadı."); return -1; }
        registered = true;
    }
    ModalState state{*this, title, {}, fields, remove, validate};
    state.view.w = 456.f;
    state.view.h = state.contentHeight();
    const float dpi = GetDpiForWindow(panel ? panel : hwnd) / 96.0f;
    state.view.scale = std::clamp(dpi, 0.75f,
        std::min((work.right - work.left - 30.f) / state.view.w, (work.bottom - work.top - 30.f) / state.view.h));
    const int pw = static_cast<int>(std::lround(state.view.w * state.view.scale));
    const int ph = static_cast<int>(std::lround(state.view.h * state.view.scale));
    RECT parentRect = work;
    if (panel) GetWindowRect(panel, &parentRect);
    const int x = std::clamp(static_cast<int>((parentRect.left + parentRect.right - pw) / 2),
        static_cast<int>(work.left + 10), std::max<int>(work.left + 10, work.right - pw - 10));
    const int y = std::clamp(static_cast<int>((parentRect.top + parentRect.bottom - ph) / 2),
        static_cast<int>(work.top + 10), std::max<int>(work.top + 10, work.bottom - ph - 10));

    HWND owner = panel ? panel : hwnd;
    modal = true;
    HWND w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | (model.topmost ? WS_EX_TOPMOST : 0),
        EditorClass, title.c_str(), WS_POPUP, x, y, pw, ph, owner, nullptr, instance, &state);
    if (!w) {
        modal = false;
        report(L"Düzenleyici açılamadı.");
        return -1;
    }
    state.view.hwnd = w;
    state.selectField(state.firstTextField(), true);
    state.openDest = {x, y};
    if (motionEnabled()) {
        const int slide = static_cast<int>(std::lround(10.f * state.view.scale));
        state.openFrom = {x, y + slide};
        state.view.d.layerOpacity = 20;
        state.openAnimMs = animNowMs();
        state.lastHoverMs = state.openAnimMs;
        SetWindowPos(w, nullptr, state.openFrom.x, state.openFrom.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    state.paint();
    EnableWindow(owner, FALSE);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    SetTimer(w, 1, 530, nullptr);
    if (state.openAnimMs > 0.0) SetTimer(w, 2, animIntervalMs, nullptr);

    MSG msg{};
    while (!state.done && nextMessage(msg) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
    KillTimer(w, 2);
    KillTimer(w, 1);
    EnableWindow(owner, TRUE);
    state.view.d.release();
    DestroyWindow(w);
    if (panel) SetForegroundWindow(panel);
    modal = false;
    if (state.result == 1) fields = std::move(state.fields);
    return state.result;
}
void Dock::editEvent(int index) {
    if (index < 0 && model.events.size() >= 2000) { report(L"Takvim kayıt sınırı doldu."); return; }
    Event entry = index < 0 ? Event{selected.key(), 0, L"09:00", L""} : model.events[index];
    std::vector<Field> fields{{L"Etkinlik adı", entry.title}, {L"Tarih · YYYY-AA-GG", isoDay(entry.day)}, {L"Saat · SS:DD", entry.time},
        {L"Tür", num(entry.kind + 1), false, {EventKinds[0], EventKinds[1], EventKinds[2]}}};
    int result = edit(index < 0 ? L"Yeni etkinlik" : L"Etkinliği düzenle", fields, index >= 0, [](const auto& f) -> Text {
        if (f[0].value.empty()) return L"Bir etkinlik adı gir.";
        if (!parseDay(f[1].value)) return L"Geçerli bir tarih gir. Örnek 2026-11-17.";
        const auto& time = f[2].value;
        if (time.size() != 5 || time[2] != L':' || integer(time.substr(0, 2), 23) < 0 || integer(time.substr(3), 59) < 0) return L"Saat 00:00 ile 23:59 arasında olmalı.";
        if (integer(f[3].value, 3) < 1) return L"Etkinlik türü için 1, 2 veya 3 gir.";
        return {};
    });
    if (result == 1) {
        entry = {parseDay(fields[1].value), static_cast<int>(integer(fields[3].value, 3)) - 1, fields[2].value, fields[0].value};
        if (index < 0) model.events.push_back(entry); else model.events[index] = entry;
        selected = month = Date::fromKey(entry.day); listOffset = 0;
        changed();
    } else if (result == 2 && index >= 0) { model.events.erase(model.events.begin() + index); changed(); }
}
void Dock::editTask(int index) {
    if (index < 0 && model.tasks.size() >= 2000) { report(L"Görev sınırı doldu."); return; }
    std::vector<Field> fields{{L"Görev adı", index < 0 ? L"" : model.tasks[index].title}};
    int result = edit(index < 0 ? L"Yeni görev" : L"Görevi düzenle", fields, index >= 0, [](const auto& f) -> Text {
        return f[0].value.empty() ? L"Bir görev adı gir." : L"";
    });
    if (result == 1) {
        if (index < 0) {
            model.tasks.push_back({model.profile, false, fields[0].value});
            listOffset = 2000;
        } else model.tasks[index].title = fields[0].value;
        changed();
    } else if (result == 2 && index >= 0) { model.tasks.erase(model.tasks.begin() + index); changed(); }
}
void Dock::editShortcut(int index) {
    if (index < 0 && model.shortcuts.size() >= MaxShortcuts) { report(L"Dock’a en fazla " + num(MaxShortcuts) + L" kısayol eklenebilir."); return; }
    Shortcut shortcut = index < 0 ? Shortcut{} : model.shortcuts[index];
    std::vector<Field> fields{{L"Görünen ad", shortcut.label}, {L"Uygulama, dosya yolu veya web adresi", shortcut.target}};
    auto result = edit(index < 0 ? L"Kısayol ekle" : L"Kısayolu düzenle", fields, index >= 0, [](const auto& f) -> Text {
        return f[0].value.empty() || f[1].value.empty() ? L"Bir ad ve hedef gir." : L"";
    });
    if (result == 1) {
        const Text oldKey = index >= 0 ? appKey(shortcut) : Text();
        const Text oldTarget = index >= 0 ? shortcut.target : Text();
        shortcut = {fields[0].value, fields[1].value, fields[1].value == shortcut.target ? shortcut.identity : Text()};
        if (index < 0) {
            model.shortcuts.push_back(shortcut);
        } else {
            model.shortcuts[index] = shortcut;
            const Text newKey = appKey(shortcut);
            for (int p = 0; p < ProfileCount; ++p) {
                for (auto& slot : model.dockOrder[p]) {
                    if (slot.isApp && (CompareStringOrdinal(slot.appKey.c_str(), -1, oldKey.c_str(), -1, TRUE) == CSTR_EQUAL ||
                        CompareStringOrdinal(slot.appKey.c_str(), -1, oldTarget.c_str(), -1, TRUE) == CSTR_EQUAL))
                        slot.appKey = newKey;
                }
                for (auto& item : liveOrder[p]) {
                    if (item.kind == OrderKind::Shortcut && (CompareStringOrdinal(item.appKey.c_str(), -1, oldKey.c_str(), -1, TRUE) == CSTR_EQUAL ||
                        CompareStringOrdinal(item.appKey.c_str(), -1, oldTarget.c_str(), -1, TRUE) == CSTR_EQUAL))
                        item.appKey = newKey;
                }
            }
        }
        refreshAppImages();
        for (int p = 0; p < ProfileCount; ++p) ensureDockOrder(p);
        changed(true);
    } else if (result == 2 && index >= 0) {
        const Text oldKey = appKey(shortcut);
        const Text oldTarget = shortcut.target;
        model.shortcuts.erase(model.shortcuts.begin() + index);
        for (int p = 0; p < ProfileCount; ++p) {
            std::erase_if(model.dockOrder[p], [&](const DockSlot& slot) {
                return slot.isApp && (CompareStringOrdinal(slot.appKey.c_str(), -1, oldKey.c_str(), -1, TRUE) == CSTR_EQUAL ||
                    CompareStringOrdinal(slot.appKey.c_str(), -1, oldTarget.c_str(), -1, TRUE) == CSTR_EQUAL);
            });
            std::erase_if(liveOrder[p], [&](const LiveDockItem& item) {
                return item.kind == OrderKind::Shortcut && (CompareStringOrdinal(item.appKey.c_str(), -1, oldKey.c_str(), -1, TRUE) == CSTR_EQUAL ||
                    CompareStringOrdinal(item.appKey.c_str(), -1, oldTarget.c_str(), -1, TRUE) == CSTR_EQUAL);
            });
        }
        refreshAppImages();
        changed(true);
    }
}
void Dock::editMetric(int widget, int index) {
    if (index < 0 && model.metrics.size() >= 2000) { report(L"Ölçüm kayıt sınırı doldu."); return; }
    const bool sale = widget == Sales;
    auto m = index < 0 ? Metric{Date::from(now()).key(), widget, 0, 0} : model.metrics[index];
    auto amount = sale ? num(m.first / 100) + L"," + two(static_cast<int>(m.first % 100)) : num(m.first);
    std::vector<Field> fields{{L"Tarih · YYYY-AA-GG", isoDay(m.day)}, {sale ? L"Gelir (TL)" : L"Ziyaretçi", amount}, {sale ? L"Sipariş" : L"Kayıt", num(m.second)}};
    auto result = edit(sale ? L"Satış kaydı" : L"Büyüme kaydı", fields, index >= 0, [sale](const auto& f) -> Text {
        if (!parseDay(f[0].value)) return L"Geçerli bir tarih gir. Örnek 2026-11-17.";
        auto first = sale ? cents(f[1].value) : integer(f[1].value, 999999999);
        auto second = integer(f[2].value, 999999999);
        if (first < 0 || second < 0) return L"Geçerli, pozitif veya sıfır değerler gir.";
        return {};
    });
    if (result == 1) {
        m = {parseDay(fields[0].value), widget, sale ? cents(fields[1].value) : integer(fields[1].value, 999999999), integer(fields[2].value, 999999999)};
        if (index < 0) model.metrics.push_back(m); else model.metrics[index] = m;
        listOffset = 0; changed();
    } else if (result == 2 && index >= 0) { model.metrics.erase(model.metrics.begin() + index); changed(); }
}
void Dock::editRepo() {
    std::vector<Field> fields{{L"Git çalışma klasörünün tam yolu", model.repo}};
    if (edit(L"Yerel Git deposu", fields, false, [](const auto& f) -> Text {
        if (!f[0].value.empty() && currentBranch(f[0].value).empty()) return L"Bu klasörde okunabilir bir Git deposu yok.";
        return {};
    }) == 1) {
        model.repo = fields[0].value;
        branch = currentBranch(model.repo);
        changed();
    }
}
void Dock::editNote() {
    std::vector<Field> fields{{L"Not", model.notes[model.profile], true}};
    if (edit(L"Hızlı not", fields) == 1) { model.notes[model.profile] = fields[0].value; changed(); }
}
void Dock::selectCity(int city) {
    cityPicker = false;
    sheet.key = sheet.hover = -1;
    if (page == World) model.worldCity = city;
    else {
        model.weatherCity = city;
        extra.forecast.reset(); extra.weatherAt = 0; extra.weatherError = false;
        requestWeather(true);
    }
    changed();
}
