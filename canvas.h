#pragma once
#include "model.h"
#include <objidl.h>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <initializer_list>
#include <memory>
#include <span>

struct Box { float left, top, right, bottom; };
struct Vertex { float x, y; };
using Color = Gdiplus::Color;
inline Box box(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
inline float width(Box b) { return b.right - b.left; }
inline float height(Box b) { return b.bottom - b.top; }
inline Color tint(unsigned rgb, float alpha = 1) { return {static_cast<unsigned>(alpha * 255 + .5f) << 24 | rgb}; }
inline unsigned mixRgb(unsigned first, unsigned second, float amount) {
    unsigned result = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int a = (first >> shift) & 0xFF, b = (second >> shift) & 0xFF;
        result |= static_cast<unsigned>(a + (b - a) * amount + .5f) << shift;
    }
    return result;
}

enum Font { Small, Caption, Body, Strong, Value, Title, Display, Number, Micro, DateBig, ClockBig, TileLabel, FontCount };
enum Alignment { Left, Center, Right };
enum Glyph { Logo, FolderIcon, Globe, Terminal, Document, DateIcon, Timer, Check, Chip, Branch, Bag, Trend, Music, Gear, ChevronLeft, ChevronRight, ChevronUp, ChevronDown, Close, Plus, Minus, Play, Pause, Back, Next, Link, Battery, Trash, DiskIcon, Wifi, Speaker, Layers, Sliders, Sun, Cloud, Moon, Rain, Activity, Grip, BluetoothIcon, Plane, Accessibility, Refresh, SearchIcon, HistoryIcon, CropIcon, ScreenIcon, WindIcon, DropIcon, PinIcon, PlaySolid, SkipSolid, MicIcon, HeadphonesIcon };

struct Draw {
    static ULONG_PTR token;
    static ID2D1Factory* factory;
    static IDWriteFactory* writing;
    static IDWriteRenderingParams* textParams;
    static ID2D1StrokeStyle* roundStroke;
    static IDWriteTextFormat* textFormats[FontCount];
    HWND window{};
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ original{};
    void* pixels{};
    ID2D1DCRenderTarget* target{};
    ID2D1SolidColorBrush* brush{};
    ID2D1Layer* layer{};
    int w = 0, h = 0;
    float scale = 1;
    RECT localClip{}, appliedClip{};
    BYTE layerOpacity = 255;
    BYTE presentedOpacity = 255;
    bool clipped = false, regionSet = false;
    bool pixelsNeedUpload = true;
    struct CachedImage {
        const Gdiplus::Bitmap* raw = nullptr;
        std::weak_ptr<Gdiplus::Bitmap> source;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        bool light = false;
    };
    struct GradientEntry {
        uint32_t first = 0, last = 0;
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
        uint64_t stamp = 0;
    };
    struct RimEntry {
        int key = -1;
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
        uint64_t stamp = 0;
    };
    struct MotionScope {
        bool pushedLayer = false;
        bool pushedClip = false;
        bool pushedTransform = false;
        D2D1::Matrix3x2F prevTransform{};
    };
    std::vector<CachedImage> images;
    std::vector<GradientEntry> gradientBrushes;
    std::vector<RimEntry> rimBrushes;
    uint64_t brushClock = 0;
    Draw() = default;
    Draw(const Draw&) = delete;
    Draw& operator=(const Draw&) = delete;
    ~Draw() { release(); }
    void swap(Draw& other) noexcept;
    static bool initialize();
    static void shutdown();
    void release();
    bool begin(HWND hwnd, int pixelWidth, int pixelHeight, float factor);
    void end(const POINT* origin = nullptr);
    void present(const POINT* origin = nullptr);
    void clip(Box b);
    void unclip();
    MotionScope pushMotion(Box clipBounds, float opacity, float dx = 0.f, float dy = 0.f);
    void popMotion(const MotionScope& scope);
    void fill(Box b, float r, Color c);
    void outline(Box b, float r, Color c, float stroke = 1);
    void gradient(Box b, float r, Color first, Color last, bool horizontal = false);
    void gradientOutline(Box b, float r, Color first, Color last, float stroke);
    void glassRim(Box b, float radius, float strength, bool polished = false);
    void text(const Text& value, Box b, Font font, Color color, Alignment alignment = Left);
    void line(float x, float y, float toX, float toY, Color c, float stroke = 1);
    void circle(float x, float y, float radius, Color c, bool solid = true, float stroke = 1);
    void arc(Box b, float start, float sweep, Color color, float stroke);
    void rimProgress(Box b, float radius, float portion, Color color, float stroke);
    void path(std::initializer_list<Vertex> points, Color c, float thickness = 1.6f);
    void icon(Glyph glyph, Box b, Color c);
    void image(const std::shared_ptr<Gdiplus::Bitmap>& image, Box bounds, bool lightSurface = false, float opacity = 1.f);
    void graph(Box bounds, const std::array<long long, 7>& values, unsigned color, bool area);
    void sparkline(Box bounds, std::span<const double> values, unsigned color, float progress, bool area = true);
    Font fit(const Text& value, float available, Font preferred, Font fallback);
    Box snap(Box b) const;
private:
    ID2D1LinearGradientBrush* getGradientBrush(Color first, Color last);
    ID2D1LinearGradientBrush* getRimBrush(float strength);
};
