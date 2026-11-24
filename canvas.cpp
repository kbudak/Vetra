#include "canvas.h"
#include <tuple>
#include "design.h"
#include <cmath>

using namespace Gdiplus;

ULONG_PTR Draw::token = 0;
ID2D1Factory* Draw::factory = nullptr;
IDWriteFactory* Draw::writing = nullptr;
IDWriteRenderingParams* Draw::textParams = nullptr;
ID2D1StrokeStyle* Draw::roundStroke = nullptr;
IDWriteTextFormat* Draw::textFormats[FontCount]{};

namespace {
template<class T> void drop(T*& value) { if (value) { value->Release(); value = nullptr; } }
D2D1_RECT_F d2dRect(Box b) { return {b.left, b.top, b.right, b.bottom}; }
D2D1_COLOR_F d2dColor(Color c) { return {c.GetR() / 255.f, c.GetG() / 255.f, c.GetB() / 255.f, c.GetA() / 255.f}; }
D2D1_ROUNDED_RECT roundedRect(Box b, float r) {
    r = std::max(0.f, std::min({r, width(b) / 2.f, height(b) / 2.f}));
    return D2D1::RoundedRect(d2dRect(b), r, r);
}

// 4-way set-associative cache (64 sets x 4 ways = 256 entries) for G2-continuous smooth rounded rectangles.
// Geometries are stored at origin (0,0)-(w,h) and drawn via render target translation to eliminate per-frame COM allocations.
struct SmoothGeoEntry {
    int wq = 0, hq = 0, rq = 0;
    ID2D1PathGeometry* geo = nullptr;
    uint64_t stamp = 0;
};
constexpr size_t SmoothSets = 64, SmoothWays = 4;
SmoothGeoEntry gSmoothCache[SmoothSets * SmoothWays]{};
uint64_t gSmoothClock = 0;

ID2D1PathGeometry* gLogoVGeo = nullptr;
ID2D1PathGeometry* gCloudGeo = nullptr;
ID2D1PathGeometry* gRainCloudGeo = nullptr;
ID2D1PathGeometry* gPlaySolidGeo = nullptr;
ID2D1PathGeometry* gSkipSolidGeo = nullptr;

struct ArcGeoEntry {
    int rxQ = 0, ryQ = 0, startQ = 0, sweepQ = 0;
    ID2D1PathGeometry* geo = nullptr;
    uint64_t stamp = 0;
};
constexpr size_t ArcCacheSize = 32;
ArcGeoEntry gArcCache[ArcCacheSize]{};
uint64_t gArcClock = 0;

struct ChartGeoEntry {
    uint64_t dataHash = 0;
    int wQ = 0, hQ = 0, progQ = 0;
    bool area = false;
    ID2D1PathGeometry* fillGeo = nullptr;
    ID2D1PathGeometry* strokeGeo = nullptr;
    D2D1_POINT_2F endPt{};
    uint64_t stamp = 0;
};
constexpr size_t ChartCacheSize = 16;
ChartGeoEntry gChartCache[ChartCacheSize]{};
uint64_t gChartClock = 0;

int gTextAlignCache[FontCount];

struct FitCacheEntry {
    ::Font preferred = Small, fallback = Small, result = Small;
    int availQ = -1;
    Text text;
    uint64_t stamp = 0;
};
constexpr size_t FitCacheSize = 64;
FitCacheEntry gFitCache[FitCacheSize]{};
uint64_t gFitClock = 0;

void clearStaticCaches() {
    for (auto& entry : gSmoothCache) {
        drop(entry.geo);
        entry.wq = entry.hq = entry.rq = 0;
        entry.stamp = 0;
    }
    for (auto& entry : gArcCache) {
        drop(entry.geo);
        entry.rxQ = entry.ryQ = entry.startQ = entry.sweepQ = 0;
        entry.stamp = 0;
    }
    for (auto& entry : gChartCache) {
        drop(entry.fillGeo);
        drop(entry.strokeGeo);
        entry.dataHash = 0;
        entry.wQ = entry.hQ = entry.progQ = 0;
        entry.stamp = 0;
    }
    drop(gLogoVGeo);
    drop(gCloudGeo);
    drop(gRainCloudGeo);
    drop(gPlaySolidGeo);
    drop(gSkipSolidGeo);
    for (auto& entry : gFitCache) {
        entry.text.clear();
        entry.availQ = -1;
        entry.stamp = 0;
    }
}

ID2D1PathGeometry* buildPlayTriangle(ID2D1Factory* factory, bool skip) {
    ID2D1PathGeometry* geo{};
    ID2D1GeometrySink* sink{};
    if (SUCCEEDED(factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
        sink->BeginFigure({skip ? 5.f : 8.f, 4.f}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine({skip ? 16.f : 20.f, 12.f});
        sink->AddLine({skip ? 5.f : 8.f, 20.f});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) drop(geo);
    }
    drop(sink);
    return geo;
}

ID2D1PathGeometry* cachedArcGeo(ID2D1Factory* factory, float rx, float ry, float start, float sweep) {
    const int rxQ = static_cast<int>(std::lround(rx * 8.f));
    const int ryQ = static_cast<int>(std::lround(ry * 8.f));
    const int startQ = static_cast<int>(std::lround(start * 2.f));
    const int sweepQ = static_cast<int>(std::lround(sweep * 2.f));
    size_t victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < ArcCacheSize; ++i) {
        auto& e = gArcCache[i];
        if (e.geo && e.rxQ == rxQ && e.ryQ == ryQ && e.startQ == startQ && e.sweepQ == sweepQ) {
            e.stamp = ++gArcClock;
            return e.geo;
        }
        if (!e.geo) { victim = i; oldest = 0; }
        else if (e.stamp < oldest) { oldest = e.stamp; victim = i; }
    }
    constexpr float pi = 3.14159265f;
    const float qRx = rxQ / 8.f, qRy = ryQ / 8.f;
    const float sRad = (startQ / 2.f) * pi / 180.f;
    const float eRad = ((startQ + sweepQ) / 2.f) * pi / 180.f;
    ID2D1PathGeometry* geo{};
    ID2D1GeometrySink* sink{};
    if (SUCCEEDED(factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
        sink->BeginFigure({qRx * std::cos(sRad), qRy * std::sin(sRad)}, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc({{qRx * std::cos(eRad), qRy * std::sin(eRad)}, {qRx, qRy}, 0.f,
            D2D1_SWEEP_DIRECTION_CLOCKWISE, (sweepQ / 2.f) > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) drop(geo);
    }
    drop(sink);
    if (!geo) return nullptr;
    auto& slot = gArcCache[victim];
    drop(slot.geo);
    slot = {rxQ, ryQ, startQ, sweepQ, geo, ++gArcClock};
    return geo;
}

uint64_t hashChartData(const void* data, size_t bytes, uint64_t seed) {
    uint64_t h = 14695981039346656037ULL ^ seed;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

ID2D1PathGeometry* buildSmoothRectOrigin(ID2D1Factory* factory, float w, float h, float radius) {
    const float minSide = std::min(w, h);
    if (minSide <= 30.f || radius < 8.f) return nullptr;
    constexpr float A = 1.528665f;
    constexpr float B = 1.088493f;
    constexpr float C = 0.868407f;
    constexpr float D = 0.631494f;
    constexpr float E = 0.074911f;
    constexpr float F = 0.372824f;
    constexpr float G = 0.169060f;
    const float s = std::min(radius, (minSide / 2.f) / A);
    if (s < 3.f) return nullptr;

    ID2D1PathGeometry* geo{};
    ID2D1GeometrySink* sink{};
    if (FAILED(factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) {
        drop(geo);
        return nullptr;
    }
    const float r = w, bot = h;
    sink->BeginFigure({r - A * s, 0.f}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier({{r - B * s, 0.f}, {r - C * s, 0.f}, {r - D * s, E * s}});
    sink->AddBezier({{r - F * s, G * s}, {r - G * s, F * s}, {r - E * s, D * s}});
    sink->AddBezier({{r, C * s}, {r, B * s}, {r, A * s}});
    if (bot - A * s > A * s + .01f) sink->AddLine({r, bot - A * s});
    sink->AddBezier({{r, bot - B * s}, {r, bot - C * s}, {r - E * s, bot - D * s}});
    sink->AddBezier({{r - G * s, bot - F * s}, {r - F * s, bot - G * s}, {r - D * s, bot - E * s}});
    sink->AddBezier({{r - C * s, bot}, {r - B * s, bot}, {r - A * s, bot}});
    if (r - A * s > A * s + .01f) sink->AddLine({A * s, bot});
    sink->AddBezier({{B * s, bot}, {C * s, bot}, {D * s, bot - E * s}});
    sink->AddBezier({{F * s, bot - G * s}, {G * s, bot - F * s}, {E * s, bot - D * s}});
    sink->AddBezier({{0.f, bot - C * s}, {0.f, bot - B * s}, {0.f, bot - A * s}});
    if (bot - A * s > A * s + .01f) sink->AddLine({0.f, A * s});
    sink->AddBezier({{0.f, B * s}, {0.f, C * s}, {E * s, D * s}});
    sink->AddBezier({{G * s, F * s}, {F * s, G * s}, {D * s, E * s}});
    sink->AddBezier({{C * s, 0.f}, {B * s, 0.f}, {A * s, 0.f}});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) {
        drop(sink);
        drop(geo);
        return nullptr;
    }
    drop(sink);
    return geo;
}

ID2D1PathGeometry* cachedSmoothRect(ID2D1Factory* factory, float w, float h, float radius) {
    if (std::min(w, h) <= 30.f || radius < 8.f) return nullptr;
    const int wq = static_cast<int>(std::lround(w * 16.f));
    const int hq = static_cast<int>(std::lround(h * 16.f));
    const int rq = static_cast<int>(std::lround(radius * 16.f));
    const size_t hash = (static_cast<size_t>(wq) * 2654435761u) ^ (static_cast<size_t>(hq) * 2246822519u) ^ (static_cast<size_t>(rq) * 3266489917u);
    const size_t base = (hash % SmoothSets) * SmoothWays;
    size_t victim = base;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < SmoothWays; ++i) {
        auto& e = gSmoothCache[base + i];
        if (e.geo && e.wq == wq && e.hq == hq && e.rq == rq) {
            e.stamp = ++gSmoothClock;
            return e.geo;
        }
        if (!e.geo) { victim = base + i; oldest = 0; }
        else if (e.stamp < oldest) { oldest = e.stamp; victim = base + i; }
    }
    ID2D1PathGeometry* built = buildSmoothRectOrigin(factory, wq / 16.f, hq / 16.f, rq / 16.f);
    if (!built) return nullptr;
    auto& slot = gSmoothCache[victim];
    drop(slot.geo);
    slot.wq = wq; slot.hq = hq; slot.rq = rq;
    slot.geo = built;
    slot.stamp = ++gSmoothClock;
    return built;
}

ID2D1PathGeometry* buildCloudPath(ID2D1Factory* factory, float base) {
    ID2D1PathGeometry* cloud{};
    ID2D1GeometrySink* sink{};
    if (SUCCEEDED(factory->CreatePathGeometry(&cloud)) && SUCCEEDED(cloud->Open(&sink))) {
        sink->BeginFigure({6.f, base}, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddBezier({{.4f, base}, {1.f, base - 8}, {7.f, base - 8}});
        sink->AddBezier({{8.f, base - 15}, {17.f, base - 15}, {18.f, base - 8}});
        sink->AddBezier({{24.f, base - 8}, {24.f, base}, {18.f, base}});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) drop(cloud);
    }
    drop(sink);
    return cloud;
}

ID2D1PathGeometry* buildLogoVPath(ID2D1Factory* factory) {
    ID2D1PathGeometry* geo{};
    ID2D1GeometrySink* sink{};
    const D2D1_POINT_2F pts[] = {
        {5.8f, 6.2f}, {8.9f, 6.2f}, {12.f, 11.7f}, {15.1f, 6.2f}, {18.2f, 6.2f}, {12.f, 15.8f}
    };
    if (SUCCEEDED(factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 1; i < ARRAYSIZE(pts); ++i) sink->AddLine(pts[i]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) drop(geo);
    }
    drop(sink);
    return geo;
}

}

bool Draw::initialize() {
    GdiplusStartupInput startup;
    if (!token && GdiplusStartup(&token, &startup, nullptr) != Ok) return false;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&writing))))
        return false;
    const auto strokeProps = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory->CreateStrokeStyle(strokeProps, nullptr, 0, &roundStroke);
    writing->CreateCustomRenderingParams(1.8f, 1.15f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE_NATURAL, &textParams);
    const float sizes[] = {Design::Small, Design::Caption, Design::Body, Design::Strong, Design::Value, 29, 48, 34, 9.5f, 18.5f, 24.f, 13.f};
    const DWRITE_FONT_WEIGHT weights[] = {
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_WEIGHT_SEMI_BOLD
    };
    for (int i = 0; i < FontCount; ++i) {
        gTextAlignCache[i] = -1;
        if (FAILED(writing->CreateTextFormat(L"Segoe UI", nullptr, weights[i], DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                sizes[i], L"tr-TR", &textFormats[i])))
            return false;
        textFormats[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        textFormats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING options{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* ellipsis{};
        if (SUCCEEDED(writing->CreateEllipsisTrimmingSign(textFormats[i], &ellipsis))) {
            textFormats[i]->SetTrimming(&options, ellipsis);
            ellipsis->Release();
        }
    }
    gLogoVGeo = buildLogoVPath(factory);
    gCloudGeo = buildCloudPath(factory, 18.0f);
    gRainCloudGeo = buildCloudPath(factory, 15.0f);
    gPlaySolidGeo = buildPlayTriangle(factory, false);
    gSkipSolidGeo = buildPlayTriangle(factory, true);
    return true;
}
void Draw::shutdown() {
    clearStaticCaches();
    for (auto& f : textFormats) drop(f);
    drop(textParams);
    drop(roundStroke);
    drop(writing);
    drop(factory);
}
void Draw::release() {
    images.clear();
    gradientBrushes.clear();
    rimBrushes.clear();
    drop(layer);
    drop(brush);
    drop(target);
    if (original) SelectObject(dc, original);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    original = nullptr; bitmap = nullptr; dc = nullptr; pixels = nullptr; w = h = 0;
    pixelsNeedUpload = true;
}
bool Draw::begin(HWND hwnd, int pixelWidth, int pixelHeight, float factor) {
    if (pixelWidth <= 0 || pixelHeight <= 0) return false;
    if (w != pixelWidth || h != pixelHeight || !target) {
        release();
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = pixelWidth;
        info.bmiHeader.biHeight = -pixelHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dc || !bitmap) { release(); return false; }
        original = SelectObject(dc, bitmap);
        w = pixelWidth; h = pixelHeight;
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        RECT rect{0, 0, w, h};
        if (FAILED(factory->CreateDCRenderTarget(&props, &target)) || FAILED(target->BindDC(dc, &rect)) ||
            FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0), &brush))) {
            release();
            return false;
        }
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (textParams) target->SetTextRenderingParams(textParams);
    }
    if (window != hwnd) {
        window = hwnd;
        regionSet = false;
        pixelsNeedUpload = true;
    }
    scale = factor;
    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    target->Clear(D2D1::ColorF(0, 0.f));
    return true;
}
void Draw::end(const POINT* origin) {
    if (FAILED(target->EndDraw())) { release(); return; }
    pixelsNeedUpload = true;
    present(origin);
}

void Draw::swap(Draw& other) noexcept {
    const auto state = [](Draw& d) {
        return std::tie(d.window, d.dc, d.bitmap, d.original, d.pixels, d.target, d.brush, d.layer,
            d.w, d.h, d.scale, d.localClip, d.appliedClip, d.layerOpacity, d.presentedOpacity,
            d.clipped, d.regionSet, d.pixelsNeedUpload, d.images, d.gradientBrushes, d.rimBrushes, d.brushClock);
    };
    auto theirs = state(other);
    state(*this).swap(theirs);
}

void Draw::present(const POINT* origin) {
    if (!dc) return;
    if (clipped || regionSet) {
        if (clipped != regionSet || !EqualRect(&localClip, &appliedClip)) {
            HRGN region = clipped ? CreateRectRgnIndirect(&localClip) : nullptr;
            if ((!clipped || region) && SetWindowRgn(window, region, FALSE)) {
                regionSet = clipped;
                appliedClip = localClip;
            } else if (region) {
                DeleteObject(region);
            }
        }
    }
    if (!pixelsNeedUpload && presentedOpacity == layerOpacity) {
        // Translate the compositor's existing surface when its pixels and opacity are unchanged.
        if (!origin || SetWindowPos(window, nullptr, origin->x, origin->y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) return;
    }
    POINT source{};
    SIZE size{w, h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, layerOpacity, AC_SRC_ALPHA};
    POINT destination = origin ? *origin : POINT{};
    if (UpdateLayeredWindow(window, nullptr, origin ? &destination : nullptr, &size, dc, &source, 0, &blend, ULW_ALPHA)) {
        presentedOpacity = layerOpacity;
        pixelsNeedUpload = false;
    }
}

void Draw::clip(Box b) { target->PushAxisAlignedClip(d2dRect(b), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE); }
void Draw::unclip() { target->PopAxisAlignedClip(); }

Draw::MotionScope Draw::pushMotion(Box clipBounds, float opacity, float dx, float dy) {
    MotionScope scope{};
    const bool hasFade = opacity < 0.999f;
    const bool hasShift = std::abs(dx) > 0.05f || std::abs(dy) > 0.05f;
    if (!hasFade && !hasShift) return scope;
    if (hasFade) {
        if (!layer) target->CreateLayer(nullptr, &layer);
        if (layer) {
            target->PushLayer(D2D1::LayerParameters(d2dRect(clipBounds), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(), std::clamp(opacity, 0.f, 1.f)), layer);
            scope.pushedLayer = true;
        }
    }
    if (!scope.pushedLayer && width(clipBounds) > 0 && height(clipBounds) > 0) {
        clip(clipBounds);
        scope.pushedClip = true;
    }
    if (hasShift) {
        target->GetTransform(&scope.prevTransform);
        target->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy) * scope.prevTransform);
        scope.pushedTransform = true;
    }
    return scope;
}

void Draw::popMotion(const MotionScope& scope) {
    if (scope.pushedTransform) target->SetTransform(scope.prevTransform);
    if (scope.pushedClip) unclip();
    if (scope.pushedLayer) target->PopLayer();
}

ID2D1LinearGradientBrush* Draw::getGradientBrush(Color first, Color last) {
    const uint32_t k0 = first.GetValue(), k1 = last.GetValue();
    size_t victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < gradientBrushes.size(); ++i) {
        auto& entry = gradientBrushes[i];
        if (entry.first == k0 && entry.last == k1 && entry.brush) {
            entry.stamp = ++brushClock;
            return entry.brush.Get();
        }
        if (entry.stamp < oldest) { oldest = entry.stamp; victim = i; }
    }
    D2D1_GRADIENT_STOP stops[] = {{0.f, d2dColor(first)}, {1.f, d2dColor(last)}};
    ID2D1GradientStopCollection* collection{};
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> created;
    if (SUCCEEDED(target->CreateGradientStopCollection(stops, 2, &collection)) &&
        SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0.f, 0.f}, {0.f, 1.f}), collection, &created))) {
        drop(collection);
        ID2D1LinearGradientBrush* raw = created.Get();
        if (gradientBrushes.size() < 48) {
            gradientBrushes.push_back({k0, k1, std::move(created), ++brushClock});
        } else {
            gradientBrushes[victim] = {k0, k1, std::move(created), ++brushClock};
        }
        return raw;
    }
    drop(collection);
    return nullptr;
}

ID2D1LinearGradientBrush* Draw::getRimBrush(float strength) {
    const int key = static_cast<int>(std::lround(std::clamp(strength, 0.f, 1.5f) * 64.f));
    size_t victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < rimBrushes.size(); ++i) {
        auto& entry = rimBrushes[i];
        if (entry.key == key && entry.brush) {
            entry.stamp = ++brushClock;
            return entry.brush.Get();
        }
        if (entry.stamp < oldest) { oldest = entry.stamp; victim = i; }
    }
    const float s = key / 64.f;
    const D2D1_GRADIENT_STOP stops[] = {
        {0.f, d2dColor(tint(0xFFFFFF, s * .50f))},
        {.20f, d2dColor(tint(0xEAECEF, s * .32f))},
        {.50f, d2dColor(tint(0xE2E5E9, s * .26f))},
        {.80f, d2dColor(tint(0xE4E7EB, s * .28f))},
        {1.f, d2dColor(tint(0xF0F2F5, s * .34f))}
    };
    ID2D1GradientStopCollection* collection{};
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> created;
    if (SUCCEEDED(target->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &collection)) &&
        SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0.f, 0.f}, {0.f, 1.f}), collection, &created))) {
        drop(collection);
        ID2D1LinearGradientBrush* raw = created.Get();
        if (rimBrushes.size() < 64) {
            rimBrushes.push_back({key, std::move(created), ++brushClock});
        } else {
            rimBrushes[victim] = {key, std::move(created), ++brushClock};
        }
        return raw;
    }
    drop(collection);
    return nullptr;
}

void Draw::fill(Box b, float r, Color c) {
    const float bw = width(b), bh = height(b);
    if (bw <= 0 || bh <= 0) return;
    brush->SetColor(d2dColor(c));
    if (auto* geo = cachedSmoothRect(factory, bw, bh, r)) {
        D2D1::Matrix3x2F prev{};
        target->GetTransform(&prev);
        target->SetTransform(D2D1::Matrix3x2F::Translation(b.left, b.top) * prev);
        target->FillGeometry(geo, brush);
        target->SetTransform(prev);
    } else {
        target->FillRoundedRectangle(roundedRect(b, r), brush);
    }
}
void Draw::outline(Box b, float r, Color c, float stroke) {
    const float bw = width(b), bh = height(b);
    if (bw <= 0 || bh <= 0) return;
    brush->SetColor(d2dColor(c));
    if (auto* geo = cachedSmoothRect(factory, bw, bh, r)) {
        D2D1::Matrix3x2F prev{};
        target->GetTransform(&prev);
        target->SetTransform(D2D1::Matrix3x2F::Translation(b.left, b.top) * prev);
        target->DrawGeometry(geo, brush, stroke);
        target->SetTransform(prev);
    } else {
        target->DrawRoundedRectangle(roundedRect(b, r), brush, stroke);
    }
}
void Draw::gradient(Box b, float r, Color first, Color last, bool horizontal) {
    const float bw = width(b), bh = height(b);
    if (bw <= 0 || bh <= 0) return;
    if (auto* gradientBrush = getGradientBrush(first, last)) {
        if (auto* geo = cachedSmoothRect(factory, bw, bh, r)) {
            gradientBrush->SetStartPoint({0.f, 0.f});
            gradientBrush->SetEndPoint(horizontal ? D2D1_POINT_2F{bw, 0.f} : D2D1_POINT_2F{0.f, bh});
            D2D1::Matrix3x2F prev{};
            target->GetTransform(&prev);
            target->SetTransform(D2D1::Matrix3x2F::Translation(b.left, b.top) * prev);
            target->FillGeometry(geo, gradientBrush);
            target->SetTransform(prev);
        } else {
            gradientBrush->SetStartPoint({b.left, b.top});
            gradientBrush->SetEndPoint(horizontal ? D2D1_POINT_2F{b.right, b.top} : D2D1_POINT_2F{b.left, b.bottom});
            target->FillRoundedRectangle(roundedRect(b, r), gradientBrush);
        }
    } else fill(b, r, first);
}
void Draw::gradientOutline(Box b, float r, Color first, Color last, float stroke) {
    const float bw = width(b), bh = height(b);
    if (bw <= 0 || bh <= 0) return;
    if (auto* gradientBrush = getGradientBrush(first, last)) {
        if (auto* geo = cachedSmoothRect(factory, bw, bh, r)) {
            gradientBrush->SetStartPoint({0.f, 0.f});
            gradientBrush->SetEndPoint({0.f, bh});
            D2D1::Matrix3x2F prev{};
            target->GetTransform(&prev);
            target->SetTransform(D2D1::Matrix3x2F::Translation(b.left, b.top) * prev);
            target->DrawGeometry(geo, gradientBrush, stroke);
            target->SetTransform(prev);
        } else {
            gradientBrush->SetStartPoint({b.left, b.top});
            gradientBrush->SetEndPoint({b.left, b.bottom});
            target->DrawRoundedRectangle(roundedRect(b, r), gradientBrush, stroke);
        }
    } else outline(b, r, first, stroke);
}

void Draw::glassRim(Box b, float radius, float strength, bool) {
    const Box snapped = snap(b);
    const float edge = .5f / scale;
    const Box rim{snapped.left + edge, snapped.top + edge, snapped.right - edge, snapped.bottom - edge};
    const float rw = width(rim), rh = height(rim);
    if (rw <= 0 || rh <= 0) return;
    const float rimRadius = std::max(0.f, radius - edge);
    if (auto* reflection = getRimBrush(strength)) {
        if (auto* geo = cachedSmoothRect(factory, rw, rh, rimRadius)) {
            reflection->SetStartPoint({0.f, 0.f});
            reflection->SetEndPoint({0.f, rh});
            D2D1::Matrix3x2F prev{};
            target->GetTransform(&prev);
            target->SetTransform(D2D1::Matrix3x2F::Translation(rim.left, rim.top) * prev);
            target->DrawGeometry(geo, reflection, 1.f / scale);
            target->SetTransform(prev);
        } else {
            reflection->SetStartPoint({rim.left, rim.top});
            reflection->SetEndPoint({rim.left, rim.bottom});
            target->DrawRoundedRectangle(roundedRect(rim, rimRadius), reflection, 1.f / scale);
        }
    } else {
        outline(rim, rimRadius, tint(0xE2E5E9, strength * .32f), 1.f / scale);
    }
}

Box Draw::snap(Box b) const {
    const auto pixel = [this](float value) { return std::round(value * scale) / scale; };
    return {pixel(b.left), pixel(b.top), pixel(b.right), pixel(b.bottom)};
}
::Font Draw::fit(const Text& value, float available, ::Font preferred, ::Font fallback) {
    const int availQ = static_cast<int>(std::lround(available * 2.f));
    size_t victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < FitCacheSize; ++i) {
        auto& e = gFitCache[i];
        if (e.availQ == availQ && e.preferred == preferred && e.fallback == fallback &&
            e.text.size() == value.size() && e.text == value) {
            e.stamp = ++gFitClock;
            return e.result;
        }
        if (e.stamp < oldest) { oldest = e.stamp; victim = i; }
    }
    ::Font res = fallback;
    IDWriteTextLayout* layout{};
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(writing->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), textFormats[preferred], 10000.f, 100.f, &layout))) {
        layout->GetMetrics(&metrics);
        layout->Release();
        res = metrics.width + 4 <= available ? preferred : fallback;
    }
    gFitCache[victim] = {preferred, fallback, res, availQ, value, ++gFitClock};
    return res;
}
void Draw::graph(Box b, const std::array<long long, 7>& values, unsigned color, bool area) {
    const auto peak = *std::max_element(values.begin(), values.end());
    if (peak <= 0) {
        line(b.left, b.bottom, b.right, b.bottom, tint(color, .27f));
        return;
    }
    const float bw = width(b), bh = height(b);
    const int wQ = static_cast<int>(std::lround(bw * 4.f));
    const int hQ = static_cast<int>(std::lround(bh * 4.f));
    const uint64_t dataHash = hashChartData(values.data(), sizeof(values), 0x1111u);
    ChartGeoEntry* hit = nullptr;
    size_t victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < ChartCacheSize; ++i) {
        auto& e = gChartCache[i];
        if (e.strokeGeo && e.dataHash == dataHash && e.wQ == wQ && e.hQ == hQ && e.progQ == 1000 && e.area == area) {
            e.stamp = ++gChartClock;
            hit = &e;
            break;
        }
        if (!e.strokeGeo) { victim = i; oldest = 0; }
        else if (e.stamp < oldest) { oldest = e.stamp; victim = i; }
    }
    if (!hit) {
        const float qw = wQ / 4.f, qh = hQ / 4.f;
        std::array<D2D1_POINT_2F, 7> points;
        for (size_t i = 0; i < points.size(); ++i)
            points[i] = {qw * i / 6.f, qh - qh * static_cast<float>(values[i]) / static_cast<float>(peak)};
        ID2D1PathGeometry* fillGeo{};
        ID2D1PathGeometry* strokeGeo{};
        ID2D1GeometrySink* sink{};
        if (area && SUCCEEDED(factory->CreatePathGeometry(&fillGeo)) && SUCCEEDED(fillGeo->Open(&sink))) {
            sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 1; i < points.size(); ++i) sink->AddLine(points[i]);
            sink->AddLine({qw, qh + 1.f});
            sink->AddLine({0.f, qh + 1.f});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) drop(fillGeo);
            drop(sink);
        }
        if (SUCCEEDED(factory->CreatePathGeometry(&strokeGeo)) && SUCCEEDED(strokeGeo->Open(&sink))) {
            sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_HOLLOW);
            for (size_t i = 1; i < points.size(); ++i) sink->AddLine(points[i]);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(sink->Close())) drop(strokeGeo);
            drop(sink);
        }
        auto& slot = gChartCache[victim];
        drop(slot.fillGeo);
        drop(slot.strokeGeo);
        slot = {dataHash, wQ, hQ, 1000, area, fillGeo, strokeGeo, points.back(), ++gChartClock};
        hit = &slot;
    }
    D2D1::Matrix3x2F prev{};
    target->GetTransform(&prev);
    target->SetTransform(D2D1::Matrix3x2F::Translation(b.left, b.top) * prev);
    if (hit->fillGeo) {
        if (auto* grad = getGradientBrush(tint(color, .29f), tint(color, .02f))) {
            grad->SetStartPoint({0.f, 0.f});
            grad->SetEndPoint({0.f, bh + 2.f});
            target->FillGeometry(hit->fillGeo, grad);
        }
    }
    if (hit->strokeGeo) {
        brush->SetColor(d2dColor(tint(color)));
        target->DrawGeometry(hit->strokeGeo, brush, 1.65f, roundStroke);
    }
    target->SetTransform(prev);
    circle(b.left + hit->endPt.x, b.top + hit->endPt.y, 1.65f, tint(color));
}
void Draw::sparkline(Box b, std::span<const double> values, unsigned color, float progress, bool area) {
    if (values.size() < 2 || progress <= 0) return;
    const float bw = width(b), bh = height(b);
    const int wQ = static_cast<int>(std::lround(bw * 4.f));
    const int hQ = static_cast<int>(std::lround(bh * 4.f));
    const int progQ = static_cast<int>(std::lround(std::min(progress, 1.f) * 200.f));
    const uint64_t dataHash = hashChartData(values.data(), values.size() * sizeof(double), 0x2222u ^ values.size());

    ChartGeoEntry* hit = nullptr;
    size_t victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < ChartCacheSize; ++i) {
        auto& e = gChartCache[i];
        if (e.strokeGeo && e.dataHash == dataHash && e.wQ == wQ && e.hQ == hQ && e.progQ == progQ && e.area == area) {
            e.stamp = ++gChartClock;
            hit = &e;
            break;
        }
        if (!e.strokeGeo) { victim = i; oldest = 0; }
        else if (e.stamp < oldest) { oldest = e.stamp; victim = i; }
    }
    if (!hit) {
        const float qw = wQ / 4.f, qh = hQ / 4.f;
        const auto [low, high] = std::minmax_element(values.begin(), values.end());
        const double range = *high - *low;
        auto point = [&](size_t i) -> D2D1_POINT_2F {
            const float ratio = range > 0 ? static_cast<float>((values[i] - *low) / range) : .5f;
            return {qw * i / (values.size() - 1), qh - qh * (.05f + .9f * ratio)};
        };
        const float fraction = (progQ / 200.f) * (values.size() - 1);
        const auto complete = static_cast<size_t>(fraction);
        auto end = point(complete);
        if (complete + 1 < values.size()) {
            const auto next = point(complete + 1);
            end = {std::lerp(end.x, next.x, fraction - complete), std::lerp(end.y, next.y, fraction - complete)};
        }
        auto buildTrace = [&](bool fill) -> ID2D1PathGeometry* {
            ID2D1PathGeometry* geometry{};
            ID2D1GeometrySink* sink{};
            if (SUCCEEDED(factory->CreatePathGeometry(&geometry)) && SUCCEEDED(geometry->Open(&sink))) {
                sink->BeginFigure(point(0), fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                for (size_t i = 1; i <= complete; ++i) sink->AddLine(point(i));
                if (fraction > complete) sink->AddLine(end);
                if (fill) { sink->AddLine({end.x, qh}); sink->AddLine({0.f, qh}); }
                sink->EndFigure(fill ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
                if (FAILED(sink->Close())) drop(geometry);
            }
            drop(sink);
            return geometry;
        };
        ID2D1PathGeometry* fillGeo = area ? buildTrace(true) : nullptr;
        ID2D1PathGeometry* strokeGeo = buildTrace(false);
        auto& slot = gChartCache[victim];
        drop(slot.fillGeo);
        drop(slot.strokeGeo);
        slot = {dataHash, wQ, hQ, progQ, area, fillGeo, strokeGeo, end, ++gChartClock};
        hit = &slot;
    }
    D2D1::Matrix3x2F prev{};
    target->GetTransform(&prev);
    target->SetTransform(D2D1::Matrix3x2F::Translation(b.left, b.top) * prev);
    if (hit->fillGeo) {
        if (auto* grad = getGradientBrush(tint(color, .33f), tint(color, .03f))) {
            grad->SetStartPoint({0.f, 0.f});
            grad->SetEndPoint({0.f, bh});
            target->FillGeometry(hit->fillGeo, grad);
        }
    }
    if (hit->strokeGeo) {
        brush->SetColor(d2dColor(tint(color)));
        target->DrawGeometry(hit->strokeGeo, brush, 1.8f, roundStroke);
    }
    target->SetTransform(prev);
    circle(b.left + hit->endPt.x, b.top + hit->endPt.y, 2.f, tint(color));
}
void Draw::text(const Text& value, Box b, ::Font font, Color color, Alignment alignment) {
    if (value.empty() || width(b) < 1 || height(b) < 1) return;
    b = snap(b);
    constexpr DWRITE_TEXT_ALIGNMENT map[] = {DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_TRAILING};
    if (gTextAlignCache[font] != static_cast<int>(alignment)) {
        textFormats[font]->SetTextAlignment(map[alignment]);
        gTextAlignCache[font] = static_cast<int>(alignment);
    }
    auto end = value.find_first_of(L"\r\n");
    UINT32 length = static_cast<UINT32>(std::min(value.size(), end));
    brush->SetColor(d2dColor(color));
    target->DrawText(value.c_str(), length, textFormats[font], d2dRect(b), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
void Draw::line(float x, float y, float toX, float toY, Color c, float stroke) {
    brush->SetColor(d2dColor(c));
    target->DrawLine({x, y}, {toX, toY}, brush, stroke, roundStroke);
}
void Draw::circle(float x, float y, float radius, Color c, bool solid, float stroke) {
    brush->SetColor(d2dColor(c));
    D2D1_ELLIPSE ellipse{{x, y}, radius, radius};
    if (solid) target->FillEllipse(ellipse, brush);
    else target->DrawEllipse(ellipse, brush, stroke);
}
void Draw::rimProgress(Box b, float radius, float portion, Color color, float stroke) {
    if (portion <= 0.f || width(b) <= 0.f || height(b) <= 0.f) return;
    radius = std::min({radius, width(b) / 2, height(b) / 2});
    constexpr float quarter = 1.5707963268f;
    const float corner = quarter * radius;
    const float length = 2 * (width(b) + height(b) - 4 * radius) + 4 * corner;
    float left = length * std::min(1.f, portion);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return;
    D2D1_POINT_2F point{(b.left + b.right) / 2, b.top};
    sink->BeginFigure(point, D2D1_FIGURE_BEGIN_HOLLOW);
    const auto straight = [&](float x, float y) {
        if (left <= 0.f) return;
        const float distance = std::hypot(x - point.x, y - point.y);
        if (distance <= 0.f) return;
        const float fraction = std::min(left / distance, 1.f);
        point = {point.x + (x - point.x) * fraction, point.y + (y - point.y) * fraction};
        sink->AddLine(point);
        left -= distance;
    };
    const auto turn = [&](float cx, float cy, float angle) {
        if (left <= 0.f || corner <= 0.f) return;
        angle += quarter * std::min(left / corner, 1.f);
        point = {cx + std::cos(angle) * radius, cy + std::sin(angle) * radius};
        sink->AddArc(D2D1::ArcSegment(point, D2D1::SizeF(radius, radius), 0.f,
            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        left -= corner;
    };
    straight(b.right - radius, b.top);
    turn(b.right - radius, b.top + radius, -quarter);
    straight(b.right, b.bottom - radius);
    turn(b.right - radius, b.bottom - radius, 0.f);
    straight(b.left + radius, b.bottom);
    turn(b.left + radius, b.bottom - radius, quarter);
    straight(b.left, b.top + radius);
    turn(b.left + radius, b.top + radius, 2 * quarter);
    straight((b.left + b.right) / 2, b.top);
    sink->EndFigure(portion >= 1.f ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close())) {
        brush->SetColor(d2dColor(color));
        target->DrawGeometry(geometry.Get(), brush, stroke, roundStroke);
    }
}

void Draw::arc(Box b, float start, float sweep, Color color, float stroke) {
    if (sweep <= 0 || width(b) <= 0 || height(b) <= 0) return;
    brush->SetColor(d2dColor(color));
    const float rx = width(b) / 2.f, ry = height(b) / 2.f;
    const float cx = b.left + rx, cy = b.top + ry;
    if (sweep >= 359.5f) {
        target->DrawEllipse({{cx, cy}, rx, ry}, brush, stroke);
        return;
    }
    if (auto* geo = cachedArcGeo(factory, rx, ry, start, sweep)) {
        D2D1::Matrix3x2F prev{};
        target->GetTransform(&prev);
        target->SetTransform(D2D1::Matrix3x2F::Translation(cx, cy) * prev);
        target->DrawGeometry(geo, brush, stroke, roundStroke);
        target->SetTransform(prev);
    }
}
void Draw::path(std::initializer_list<Vertex> points, Color c, float thickness) {
    if (points.size() < 2) return;
    auto at = points.begin();
    for (auto next = at + 1; next != points.end(); ++at, ++next)
        line(at->x, at->y, next->x, next->y, c, thickness);
}
void Draw::image(const std::shared_ptr<Gdiplus::Bitmap>& image, Box bounds, bool lightSurface, float opacity) {
    if (!image || opacity <= 0.001f) return;
    const Gdiplus::Bitmap* raw = image.get();
    const auto cached = std::find_if(images.begin(), images.end(), [&](const auto& entry) {
        return entry.raw == raw && entry.light == lightSurface && !entry.source.expired();
    });
    if (cached != images.end()) {
        target->DrawBitmap(cached->bitmap.Get(), d2dRect(bounds), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, image->GetWidth(), image->GetHeight());
    if (image->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) return;
    const auto row = [&](UINT y) {
        return reinterpret_cast<const UINT32*>(static_cast<const BYTE*>(data.Scan0) + static_cast<INT_PTR>(y) * data.Stride);
    };
    const void* pixels = data.Scan0;
    UINT stride = data.Stride;
    std::vector<UINT32> monochrome;
    if (lightSurface) {
        size_t visible = 0, nearWhite = 0;
        for (UINT y = 0; y < data.Height; ++y) for (UINT x = 0; x < data.Width; ++x) {
            const UINT32 pixel = row(y)[x], alpha = pixel >> 24;
            if (alpha < 128) continue;
            ++visible;
            if (std::min({pixel & 255, (pixel >> 8) & 255, (pixel >> 16) & 255}) >= alpha * 7 / 8) ++nearWhite;
        }
        if (visible > 0 && nearWhite * 100 >= visible * 94) {
            monochrome.resize(static_cast<size_t>(data.Width) * data.Height);
            for (UINT y = 0; y < data.Height; ++y) for (UINT x = 0; x < data.Width; ++x) {
                const UINT32 alpha = row(y)[x] >> 24, gray = alpha * 34 / 255;
                monochrome[static_cast<size_t>(y) * data.Width + x] = (alpha << 24) | (gray * 0x010101);
            }
            pixels = monochrome.data();
            stride = data.Width * sizeof(UINT32);
        }
    }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bmp;
    const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (SUCCEEDED(target->CreateBitmap(D2D1::SizeU(data.Width, data.Height), pixels, stride, props, &bmp))) {
        target->DrawBitmap(bmp.Get(), d2dRect(bounds), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    image->UnlockBits(&data);
    if (bmp) {
        std::erase_if(images, [](const auto& entry) { return entry.source.expired(); });
        if (images.size() >= 96) images.erase(images.begin());
        images.push_back({raw, image, std::move(bmp), lightSurface});
    }
}

void Draw::icon(Glyph glyph, Box b, Color c) {
    D2D1::Matrix3x2F previous{};
    target->GetTransform(&previous);
    target->SetTransform(D2D1::Matrix3x2F::Scale(width(b) / 24, height(b) / 24) *
        D2D1::Matrix3x2F::Translation(b.left, b.top) * previous);
    auto l = [&](float x, float y, float u, float v) { line(x, y, u, v, c, 1.65f); };
    auto p = [&](std::initializer_list<Vertex> points) { path(points, c); };
    auto o = [&](float x, float y, float r) { circle(x, y, r, c, false, 1.6f); };
    auto rect = [&](float x, float y, float w, float h, float r) { outline(box(x, y, w, h), r, c, 1.6f); };
    switch (glyph) {
    case Logo: {
        fill(box(1.f, 1.f, 22.f, 22.f), 5.6f, c);
        if (gLogoVGeo) {
            brush->SetColor(d2dColor(tint(0x141820)));
            target->FillGeometry(gLogoVGeo, brush);
        }
        fill(box(7.2f, 17.3f, 9.6f, 1.55f), .77f, tint(0x141820));
        break;
    }
    case FolderIcon:
        fill(box(3, 7, 18, 13), 2, c); fill(box(3, 4.5f, 8, 5), 1.5f, c);
        line(5, 10, 19, 10, tint(0xFFFFFF, .55f)); break;
    case Globe:
        o(12, 12, 8.5f); l(4, 12, 20, 12);
        p({{12, 3.5f}, {8.6f, 8}, {8.6f, 16}, {12, 20.5f}, {15.4f, 16}, {15.4f, 8}, {12, 3.5f}}); break;
    case Terminal: rect(3, 5, 18, 14, 3); p({{7, 9}, {10, 12}, {7, 15}}); l(12, 15, 17, 15); break;
    case Document: rect(5, 3, 14, 18, 2); l(8, 8, 16, 8); l(8, 12, 16, 12); l(8, 16, 13, 16); break;
    case DateIcon: rect(3.5f, 5.5f, 17, 15, 3); l(8, 3, 8, 8); l(16, 3, 16, 8); l(4, 10, 20, 10); break;
    case Timer: o(12, 13, 7.5f); l(9, 2, 15, 2); l(12, 13, 15, 9); break;
    case Check: p({{5, 12}, {10, 17}, {19, 7}}); break;
    case Chip: rect(5, 5, 14, 14, 3); rect(9, 9, 6, 6, 1); l(9, 2, 9, 5); l(15, 2, 15, 5); l(9, 19, 9, 22); l(15, 19, 15, 22); l(2, 9, 5, 9); l(19, 9, 22, 9); l(2, 15, 5, 15); l(19, 15, 22, 15); break;
    case Branch: o(7, 5, 2.5f); o(7, 19, 2.5f); o(17, 6, 2.5f); l(7, 7.5f, 7, 16.5f); p({{17, 9}, {16, 12}, {13, 14}, {7, 14}}); break;
    case Bag: rect(4, 7, 16, 13, 3); p({{8, 8}, {8, 5}, {10, 3}, {14, 3}, {16, 5}, {16, 8}}); break;
    case Trend: p({{3, 18}, {8, 12}, {12, 15}, {20, 6}, {15, 6}}); l(20, 6, 20, 11); break;
    case Music: p({{8, 17}, {8, 6}, {19, 3}, {19, 15}}); circle(5.5f, 17.5f, 2.8f, c); circle(16.5f, 15.5f, 2.8f, c); break;
    case Gear: o(12, 12, 7); o(12, 12, 2.5f); l(12, 1, 12, 5); l(12, 19, 12, 23); l(1, 12, 5, 12); l(19, 12, 23, 12); l(4, 4, 7, 7); l(17, 17, 20, 20); l(4, 20, 7, 17); l(17, 7, 20, 4); break;
    case ChevronLeft: p({{14, 6}, {8, 12}, {14, 18}}); break;
    case ChevronRight: p({{10, 6}, {16, 12}, {10, 18}}); break;
    case ChevronUp: p({{6, 15}, {12, 9}, {18, 15}}); break;
    case ChevronDown: p({{6, 9}, {12, 15}, {18, 9}}); break;
    case Close: l(6, 6, 18, 18); l(18, 6, 6, 18); break;
    case Plus: l(12, 5, 12, 19); l(5, 12, 19, 12); break;
    case Minus: l(6, 12, 18, 12); break;
    case Play: p({{8, 5}, {19, 12}, {8, 19}, {8, 5}}); break;
    case PlaySolid: case SkipSolid: {
        const bool skip = glyph == SkipSolid;
        if (auto* tri = skip ? gSkipSolidGeo : gPlaySolidGeo) {
            brush->SetColor(d2dColor(c));
            target->FillGeometry(tri, brush);
        }
        if (skip) fill(box(17.5f, 4.5f, 2.5f, 15.f), 1.f, c);
        break;
    }
    case Pause: l(9, 5, 9, 19); l(15, 5, 15, 19); break;
    case Back: p({{17, 5}, {7, 12}, {17, 19}, {17, 5}}); l(5, 6, 5, 18); break;
    case Next: p({{7, 5}, {17, 12}, {7, 19}, {7, 5}}); l(19, 6, 19, 18); break;
    case Link: p({{11, 7}, {14, 4}, {18, 4}, {21, 7}, {21, 11}, {17, 15}}); p({{13, 17}, {10, 20}, {6, 20}, {3, 17}, {3, 13}, {7, 9}}); l(9, 15, 15, 9); break;
    case Battery: rect(3, 6, 16, 12, 2); l(21, 10, 21, 14); fill(box(6, 9, 10, 6), 1, c); break;
    case Trash: p({{7, 8}, {8, 20}, {16, 20}, {17, 8}}); l(4, 7, 20, 7); l(9, 4, 15, 4); break;
    case DiskIcon: rect(2.5f, 6, 19, 13, 3); l(4, 13, 20, 13); circle(17.5f, 16.2f, 1.25f, c); break;
    case Wifi:
        arc(box(-1.5f, 6.f, 27.f, 27.f), 228.f, 84.f, c, 1.65f);
        arc(box(3.f, 10.5f, 18.f, 18.f), 226.f, 88.f, c, 1.65f);
        arc(box(7.2f, 14.7f, 9.6f, 9.6f), 222.f, 96.f, c, 1.65f);
        circle(12.f, 19.2f, 1.45f, c);
        break;
    case Speaker: p({{3,9},{7,9},{12,5},{12,19},{7,15},{3,15},{3,9}}); p({{16,8},{18,12},{16,16}}); p({{19,5},{22,12},{19,19}}); break;
    case MicIcon:
        rect(9, 2.5f, 6, 12, 3); p({{5.5f, 11}, {5.5f, 13.5f}, {7.5f, 17}, {12, 18.5f},
            {16.5f, 17}, {18.5f, 13.5f}, {18.5f, 11}});
        l(12, 18.5f, 12, 22); break;
    case HeadphonesIcon:
        arc(box(4, 4, 16, 17), 180, 180, c, 1.8f);
        rect(3.5f, 12, 5, 8, 2); rect(15.5f, 12, 5, 8, 2); break;
    case Layers: rect(3, 3, 13, 13, 2); p({{18,8},{21,8},{21,21},{8,21},{8,18}}); break;
    case Sliders:
        l(3,6,21,6); l(3,12,21,12); l(3,18,21,18);
        l(9,3,9,9); l(15,9,15,15); l(11,15,11,21); break;
    case Sun:
        o(12,12,4.5f); l(12,1,12,4); l(12,20,12,23); l(1,12,4,12); l(20,12,23,12);
        l(4,4,6,6); l(18,18,20,20); l(4,20,6,18); l(18,6,20,4); break;
    case Cloud:
    case Rain: {
        if (auto* cloud = glyph == Cloud ? gCloudGeo : gRainCloudGeo) {
            brush->SetColor(d2dColor(c));
            target->DrawGeometry(cloud, brush, 1.55f, roundStroke);
        }
        if (glyph == Rain) { l(7,18,5,21); l(12,18,10,21); l(17,18,15,21); }
        break;
    }
    case Moon: p({{14,3},{9,5},{7,10},{9,15},{14,17},{20,15},{17,20},{12,22},{6,20},{3,15},{3,9},{7,4},{14,3}}); break;
    case Activity: p({{2,13},{6,13},{9,5},{13,20},{16,12},{22,12}}); break;
    case BluetoothIcon:
        p({{7.f, 6.5f}, {17.f, 16.5f}, {12.f, 21.5f}, {12.f, 2.5f}, {17.f, 7.5f}, {7.f, 17.5f}});
        break;
    case Plane:
        p({{12.f, 2.5f}, {13.4f, 4.2f}, {13.4f, 8.8f}, {21.2f, 13.2f}, {21.2f, 15.2f}, {13.4f, 13.4f},
           {13.4f, 17.8f}, {16.4f, 19.6f}, {16.4f, 21.2f}, {12.f, 20.1f}, {7.6f, 21.2f}, {7.6f, 19.6f},
           {10.6f, 17.8f}, {10.6f, 13.4f}, {2.8f, 15.2f}, {2.8f, 13.2f}, {10.6f, 8.8f}, {10.6f, 4.2f}, {12.f, 2.5f}});
        break;
    case Accessibility:
        circle(12, 4, 2, c);
        p({{4, 8}, {12, 11}, {20, 8}});
        p({{12, 9}, {12, 15}, {7, 21}});
        p({{12, 15}, {17, 21}});
        break;
    case Grip:
        for (int y = 7; y <= 17; y += 5) { circle(9,y,1.25f,c); circle(15,y,1.25f,c); }
        break;
    case Refresh:
        arc(box(4.5f, 4.5f, 15, 15), -40, 285, c, 1.6f);
        p({{15.5f, 3.5f}, {19.5f, 6.5f}, {15.5f, 9.5f}});
        break;
    case SearchIcon: o(10.5f, 10.5f, 5.5f); l(14.5f, 14.5f, 20, 20); break;
    case HistoryIcon:
        arc(box(4, 4, 16, 16), -157, 315, c, 1.7f);
        p({{3.5f, 6}, {3.5f, 10.5f}, {8, 10.5f}}); p({{12, 8}, {12, 12}, {15, 14}}); break;
    case CropIcon:
        p({{7, 2}, {7, 17}, {22, 17}}); p({{2, 7}, {17, 7}, {17, 22}}); break;
    case ScreenIcon:
        rect(2.5f, 4, 19, 13, 2.5f); l(9, 21, 15, 21); l(12, 17, 12, 21); break;
    case WindIcon:
        p({{2, 8}, {16, 8}, {19, 6.5f}, {19, 4}, {17, 3}}); l(2, 12, 22, 12);
        p({{2, 16}, {14, 16}, {17, 17.5f}, {17, 20}, {15, 21}}); break;
    case DropIcon: p({{12, 2}, {6, 11}, {5, 15}, {7, 20}, {12, 22}, {17, 20}, {19, 15}, {18, 11}, {12, 2}}); break;
    case PinIcon:
        p({{12, 3}, {20, 11}, {16, 12}, {13, 16}, {13, 19}, {5, 11}, {8, 11}, {12, 7}, {12, 3}});
        l(9, 15, 3, 21); break;
    }
    target->SetTransform(previous);
}

namespace {
struct GdiPlusLifetime {
    ~GdiPlusLifetime() {
        if (Draw::token) GdiplusShutdown(Draw::token);
    }
};

// The executable-wide bitmap caches must be destroyed before GDI+ shuts down.
#pragma warning(push)
#pragma warning(disable: 4073)
#pragma init_seg(lib)
GdiPlusLifetime gGdiPlusLifetime;
#pragma warning(pop)
}
