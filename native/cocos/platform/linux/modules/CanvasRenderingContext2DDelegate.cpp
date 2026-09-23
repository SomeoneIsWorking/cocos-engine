/****************************************************************************
 Copyright (c) 2021-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#include "platform/linux/modules/CanvasRenderingContext2DDelegate.h"
#include "bindings/manual/jsb_platform.h"
#include "platform/interfaces/modules/ISystemWindowManager.h"
#include "platform/linux/LinuxPlatform.h"
#include "platform/linux/modules/SystemWindow.h"

#include <X11/X.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <fontconfig/fcfreetype.h>
#include <fontconfig/fontconfig.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {
// The canvas draws into a depth-32 pixmap, whose pixels are ARGB32: alpha in
// bits 24-31, then red, green and blue. CSS hands the components in the
// opposite order, and packing them straight through made every filled or
// stroked shape swap red and blue -- while Xft text, which is given real
// colour components rather than a pixel value, stayed correct. That is the
// asymmetry to preserve: one packing for the X pixel, components for Xft.
#define RGB(r, g, b)     (int)((((int)r) << 16) | (((int)g) << 8) | (int)b)
#define RGBA(r, g, b, a) (int)((((int)a) << 24) | RGB(r, g, b))

constexpr unsigned int PIXEL_BLUE_SHIFT = 0U;
constexpr unsigned int PIXEL_GREEN_SHIFT = 8U;
constexpr unsigned int PIXEL_RED_SHIFT = 16U;
constexpr unsigned int PIXEL_ALPHA_SHIFT = 24U;
} // namespace

namespace cc {
CanvasRenderingContext2DDelegate::CanvasRenderingContext2DDelegate() {
    auto *windowManager = BasePlatform::getPlatform()->getInterface<ISystemWindowManager>();
    CC_ASSERT_NOT_NULL(windowManager);
    auto *window = static_cast<SystemWindow *>(windowManager->getWindow(ISystemWindow::mainWindowId));
    CC_ASSERT_NOT_NULL(window);
    _dis = reinterpret_cast<Display *>(window->getDisplay());
    _win = reinterpret_cast<Drawable>(window->getWindowHandle());
    _screen = DefaultScreen(_dis);
}

CanvasRenderingContext2DDelegate::~CanvasRenderingContext2DDelegate() {
    // The window manager owns the display; only release this canvas's resources.
    releaseBuffer();
    releaseFont();
}

void CanvasRenderingContext2DDelegate::recreateBuffer(float w, float h) {
    releaseBuffer();
    _bufferWidth = w;
    _bufferHeight = h;
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }
    auto textureSize = static_cast<int>(_bufferWidth * _bufferHeight * 4);
    auto *data = static_cast<int8_t *>(malloc(sizeof(int8_t) * textureSize));
    memset(data, 0x00, textureSize);
    _imageData.fastSet((uint8_t *)data, textureSize);

    if (!_win) {
        return;
    }
    // Screen *scr = DefaultScreenOfDisplay(_dis);
    _pixmap = XCreatePixmap(_dis, _win, w, h, 32);
    _gc = XCreateGC(_dis, _pixmap, 0, 0);
    _fontDraw = XftDrawCreateAlpha(_dis, _pixmap, 32);
    CC_ASSERT_NOT_NULL(_fontDraw);
}

void CanvasRenderingContext2DDelegate::beginPath() {
    _path.clear();
}

void CanvasRenderingContext2DDelegate::closePath() {
    if (!_path.empty() && _path.back().size() > 1) {
        const XPointDouble start = _path.back().front();
        _path.back().push_back(start);
        _path.push_back({start});
    }
}

void CanvasRenderingContext2DDelegate::moveTo(float x, float y) {
    _path.push_back({XPointDouble{x, y}});
}

void CanvasRenderingContext2DDelegate::lineTo(float x, float y) {
    if (_path.empty()) {
        _path.push_back({});
    }
    _path.back().push_back(XPointDouble{x, y});
}

void CanvasRenderingContext2DDelegate::rect(float x, float y, float w, float h) {
    _path.push_back({XPointDouble{x, y}, XPointDouble{x + w, y}, XPointDouble{x + w, y + h}, XPointDouble{x, y + h}, XPointDouble{x, y}});
    _path.push_back({XPointDouble{x, y}});
}

void CanvasRenderingContext2DDelegate::fill() {
    compositePath(_fillStyle);
}

void CanvasRenderingContext2DDelegate::stroke() {
    strokePath(_strokeStyle);
}

// Fills the current path source-over, as the web canvas does, antialiased.
// Each subpath's coverage is added into one alpha mask before the colour is
// composited through it, so subpaths that overlap are covered once -- the
// nonzero rule for subpaths wound the same way. A subpath wound against
// another to cut a hole is not distinguished from one that adds to it.
void CanvasRenderingContext2DDelegate::compositePath(unsigned long style) {
    if (_pixmap == None || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }
    const auto width = static_cast<unsigned int>(_bufferWidth);
    const auto height = static_cast<unsigned int>(_bufferHeight);
    XRenderPictFormat *maskFormat = XRenderFindStandardFormat(_dis, PictStandardA8);
    XRenderPictFormat *targetFormat = XRenderFindStandardFormat(_dis, PictStandardARGB32);
    CC_ASSERT_NOT_NULL(maskFormat);
    CC_ASSERT_NOT_NULL(targetFormat);
    const Pixmap maskPixmap = XCreatePixmap(_dis, _pixmap, width, height, 8);
    const Picture mask = XRenderCreatePicture(_dis, maskPixmap, maskFormat, 0, nullptr);
    const XRenderColor clear{0, 0, 0, 0};
    XRenderFillRectangle(_dis, PictOpSrc, mask, &clear, 0, 0, width, height);
    const XRenderColor opaque{0xffff, 0xffff, 0xffff, 0xffff};
    const Picture white = XRenderCreateSolidFill(_dis, &opaque);
    for (auto &subpath : _path) {
        if (subpath.size() > 2) {
            XRenderCompositeDoublePoly(_dis, PictOpAdd, white, mask, maskFormat, 0, 0, 0, 0,
                                       subpath.data(), static_cast<int>(subpath.size()), WindingRule);
        }
    }
    const auto channel = [style](unsigned int shift) {
        const unsigned long alpha = (style >> PIXEL_ALPHA_SHIFT) & 0xffU;
        const unsigned long component = shift == PIXEL_ALPHA_SHIFT ? 0xffU : (style >> shift) & 0xffU;
        return static_cast<unsigned short>((component * alpha + 127U) / 255U * 257U);
    };
    const XRenderColor colour{channel(PIXEL_RED_SHIFT), channel(PIXEL_GREEN_SHIFT), channel(PIXEL_BLUE_SHIFT), channel(PIXEL_ALPHA_SHIFT)};
    const Picture source = XRenderCreateSolidFill(_dis, &colour);
    const Picture target = XRenderCreatePicture(_dis, _pixmap, targetFormat, 0, nullptr);
    XRenderComposite(_dis, PictOpOver, source, mask, target, 0, 0, 0, 0, 0, 0, width, height);
    XRenderFreePicture(_dis, target);
    XRenderFreePicture(_dis, source);
    XRenderFreePicture(_dis, white);
    XRenderFreePicture(_dis, mask);
    XFreePixmap(_dis, maskPixmap);
}

// Strokes the current path's subpaths at the line width. Core X lines write
// the pixel rather than blend it, so the colour is premultiplied to be the
// ARGB32 pixel the pixmap holds.
void CanvasRenderingContext2DDelegate::strokePath(unsigned long style) {
    if (_pixmap == None || _gc == nullptr) {
        return;
    }
    const unsigned long alpha = (style >> PIXEL_ALPHA_SHIFT) & 0xffU;
    const auto premultiplied = [style, alpha](unsigned int shift) {
        return (((style >> shift) & 0xffU) * alpha + 127U) / 255U << shift;
    };
    XSetForeground(_dis, _gc, (alpha << PIXEL_ALPHA_SHIFT) | premultiplied(PIXEL_RED_SHIFT) | premultiplied(PIXEL_GREEN_SHIFT) | premultiplied(PIXEL_BLUE_SHIFT));
    XSetLineAttributes(_dis, _gc, static_cast<unsigned int>(std::lround(_lineWidth)), LineSolid, CapButt, JoinRound);
    for (const auto &subpath : _path) {
        std::vector<XPoint> points;
        points.reserve(subpath.size());
        for (const auto &point : subpath) {
            points.push_back(XPoint{static_cast<short>(std::lround(point.x)), static_cast<short>(std::lround(point.y))});
        }
        if (points.size() > 1) {
            XDrawLines(_dis, _pixmap, _gc, points.data(), static_cast<int>(points.size()), CoordModeOrigin);
        }
    }
}

void CanvasRenderingContext2DDelegate::saveContext() {
}

void CanvasRenderingContext2DDelegate::restoreContext() {
}

void CanvasRenderingContext2DDelegate::clearRect(float x, float y, float w, float h) {
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    if (_imageData.isNull()) {
        return;
    }

    recreateBuffer(w, h);
}

void CanvasRenderingContext2DDelegate::fillRect(float x, float y, float w, float h) {
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    XSetForeground(_dis, _gc, _fillStyle);
    XFillRectangle(_dis, _pixmap, _gc, x, y, w, h);
}

void CanvasRenderingContext2DDelegate::fillText(const ccstd::string &text, float x, float y, float /*maxWidth*/) {
    if (text.empty() || !_font || !_fontDraw || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    Point offsetPoint = convertDrawPoint(Point{x, y}, text);
    drawTextToPixmap(text, static_cast<int>(offsetPoint[0]), static_cast<int>(offsetPoint[1]), _fillStyle);
}

void CanvasRenderingContext2DDelegate::drawTextToPixmap(const ccstd::string &text, int x, int y, unsigned long style) {
    const auto channel = [](unsigned long color, unsigned int shift) {
        return static_cast<unsigned short>(((color >> shift) & 0xffU) * 257U);
    };
    const XftColor color{0, {channel(style, PIXEL_RED_SHIFT), channel(style, PIXEL_GREEN_SHIFT), channel(style, PIXEL_BLUE_SHIFT), channel(style, PIXEL_ALPHA_SHIFT)}};
    for (const auto &run : resolveTextRuns(text)) {
        const auto *bytes = reinterpret_cast<const FcChar8 *>(text.data() + run.begin);
        const int length = static_cast<int>(run.length);
        XftDrawStringUtf8(_fontDraw, &color, run.font, x, y, bytes, length);
        XGlyphInfo extents{};
        XftTextExtentsUtf8(_dis, run.font, bytes, length, &extents);
        x += extents.xOff;
    }
}

// The JSB canvas asks for its pixels (fetchData) only when script reads them,
// and every draw call invalidates the copy it read, so the pixmap is read back
// once per read rather than after each draw -- and fillRect and fill, which
// never read back themselves, reach script like text does.
void CanvasRenderingContext2DDelegate::updateData() {
    if (_pixmap != None && !_imageData.isNull()) {
        readPixmapPixels();
    }
}

void CanvasRenderingContext2DDelegate::readPixmapPixels() {
    XImage *image = XGetImage(_dis, _pixmap, 0, 0, _bufferWidth, _bufferHeight, AllPlanes, ZPixmap);
    CC_ASSERT_NOT_NULL(image);
    int width = image->width;
    int height = image->height;
    unsigned char *data = _imageData.getBytes();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; x++) {
            const auto pixel = static_cast<uint32_t>(XGetPixel(image, x, y));
            const uint32_t alpha = pixel >> 24U;
            const auto straight = [alpha](uint32_t premultiplied) {
                return alpha == 0U ? 0U : std::min(255U, (premultiplied * 255U + alpha / 2U) / alpha);
            };
            const uint32_t red = straight((pixel >> 16U) & 0xffU);
            const uint32_t green = straight((pixel >> 8U) & 0xffU);
            const uint32_t blue = straight(pixel & 0xffU);
            reinterpret_cast<uint32_t *>(data)[y * width + x] = (alpha << 24U) | (blue << 16U) | (green << 8U) | red;
        }
    }
    XDestroyImage(image);
}

CanvasRenderingContext2DDelegate::Size CanvasRenderingContext2DDelegate::measureText(const ccstd::string &text) {
    if (text.empty() || !_font)
        return ccstd::array<float, 2>{0.0f, 0.0f};
    return ccstd::array<float, 2>{static_cast<float>(textAdvance(text)), static_cast<float>(_font->height)};
}

void CanvasRenderingContext2DDelegate::updateFont(const ccstd::string &fontName,
                                                  float fontSize,
                                                  bool bold,
                                                  bool italic,
                                                  bool oblique,
                                                  bool /* smallCaps */) {
    _fontName = fontName;
    _fontSize = static_cast<int>(fontSize);
    _fontWeight = bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR;
    _fontSlant = italic ? FC_SLANT_ITALIC : (oblique ? FC_SLANT_OBLIQUE : FC_SLANT_ROMAN);
    releaseFont();
    const auto &registeredFonts = getFontFamilyNameMap();
    const auto registered = registeredFonts.find(fontName);
    const bool customFont = registered != registeredFonts.end();
    FcPattern *requested = customFont
                               ? FcFreeTypeQuery(reinterpret_cast<const FcChar8 *>(registered->second.c_str()), 0, nullptr, nullptr)
                               : FcPatternCreate();
    CC_ASSERT_NOT_NULL(requested);
    if (customFont) {
        FcPatternDel(requested, FC_PIXEL_SIZE);
    } else {
        const auto &family = fontName.empty() ? ccstd::string("sans-serif") : fontName;
        FcPatternAddString(requested, FC_FAMILY, reinterpret_cast<const FcChar8 *>(family.c_str()));
    }
    FcPatternAddDouble(requested, FC_PIXEL_SIZE, static_cast<double>(fontSize));
    FcPatternAddInteger(requested, FC_WEIGHT, _fontWeight);
    FcPatternAddInteger(requested, FC_SLANT, _fontSlant);
    FcConfigSubstitute(nullptr, requested, FcMatchPattern);
    XftDefaultSubstitute(_dis, _screen, requested);
    FcPattern *resolved = requested;
    if (!customFont) {
        FcResult result = FcResultNoMatch;
        resolved = XftFontMatch(_dis, _screen, requested, &result);
        FcPatternDestroy(requested);
    }
    CC_ASSERT_NOT_NULL(resolved);
    _font = XftFontOpenPattern(_dis, resolved);
    CC_ASSERT_NOT_NULL(_font);
}

void CanvasRenderingContext2DDelegate::setTextAlign(TextAlign align) {
    _textAlign = align;
}

void CanvasRenderingContext2DDelegate::setTextBaseline(TextBaseline baseline) {
    _textBaseLine = baseline;
}

void CanvasRenderingContext2DDelegate::setFillStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    _fillStyle = RGBA(r, g, b, a);
}

void CanvasRenderingContext2DDelegate::setStrokeStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    _strokeStyle = RGBA(r, g, b, a);
}

void CanvasRenderingContext2DDelegate::setLineWidth(float lineWidth) {
    _lineWidth = lineWidth;
}

const cc::Data &CanvasRenderingContext2DDelegate::getDataRef() const {
    return _imageData;
}

void CanvasRenderingContext2DDelegate::releaseFont() {
    for (auto *font : _fallbackFonts) {
        XftFontClose(_dis, font);
    }
    _fallbackFonts.clear();
    if (_font) {
        XftFontClose(_dis, _font);
        _font = nullptr;
    }
}

XftFont *CanvasRenderingContext2DDelegate::resolveFont(FcChar32 codepoint) {
    if (XftCharIndex(_dis, _font, codepoint) != 0) {
        return _font;
    }
    for (auto *font : _fallbackFonts) {
        if (XftCharIndex(_dis, font, codepoint) != 0) {
            return font;
        }
    }

    FcCharSet *characters = FcCharSetCreate();
    FcPattern *requested = FcPatternCreate();
    CC_ASSERT_NOT_NULL(characters);
    CC_ASSERT_NOT_NULL(requested);
    FcCharSetAddChar(characters, codepoint);
    FcPatternAddString(requested, FC_FAMILY, reinterpret_cast<const FcChar8 *>("sans-serif"));
    FcPatternAddCharSet(requested, FC_CHARSET, characters);
    FcPatternAddDouble(requested, FC_PIXEL_SIZE, static_cast<double>(_fontSize));
    FcPatternAddInteger(requested, FC_WEIGHT, _fontWeight);
    FcPatternAddInteger(requested, FC_SLANT, _fontSlant);
    FcCharSetDestroy(characters);
    FcConfigSubstitute(nullptr, requested, FcMatchPattern);
    XftDefaultSubstitute(_dis, _screen, requested);
    FcResult result = FcResultNoMatch;
    FcPattern *matched = XftFontMatch(_dis, _screen, requested, &result);
    FcPatternDestroy(requested);
    if (matched == nullptr) {
        return _font;
    }
    XftFont *fallback = XftFontOpenPattern(_dis, matched);
    if (fallback == nullptr) {
        return _font;
    }
    if (XftCharIndex(_dis, fallback, codepoint) == 0) {
        XftFontClose(_dis, fallback);
        return _font;
    }
    _fallbackFonts.push_back(fallback);
    return fallback;
}

std::vector<CanvasRenderingContext2DDelegate::TextRun>
CanvasRenderingContext2DDelegate::resolveTextRuns(const ccstd::string &text) {
    std::vector<TextRun> runs;
    for (std::size_t position = 0; position < text.length();) {
        FcChar32 codepoint = 0;
        const auto *bytes = reinterpret_cast<const FcChar8 *>(text.data() + position);
        int length = FcUtf8ToUcs4(bytes, &codepoint, static_cast<int>(text.length() - position));
        if (length <= 0) {
            codepoint = static_cast<unsigned char>(text[position]);
            length = 1;
        }
        XftFont *font = resolveFont(codepoint);
        if (!runs.empty() && runs.back().font == font) {
            runs.back().length += static_cast<std::size_t>(length);
        } else {
            runs.push_back(TextRun{font, position, static_cast<std::size_t>(length)});
        }
        position += static_cast<std::size_t>(length);
    }
    return runs;
}

int CanvasRenderingContext2DDelegate::textAdvance(const ccstd::string &text) {
    int advance = 0;
    for (const auto &run : resolveTextRuns(text)) {
        XGlyphInfo extents{};
        XftTextExtentsUtf8(_dis, run.font,
                           reinterpret_cast<const FcChar8 *>(text.data() + run.begin),
                           static_cast<int>(run.length), &extents);
        advance += extents.xOff;
    }
    return advance;
}

// x, y offset value
int CanvasRenderingContext2DDelegate::drawText(const ccstd::string &text, int x, int y) {
    XTextItem item{const_cast<char *>(text.c_str()), static_cast<int>(text.length()), 0, None};
    return XDrawText(_dis, _pixmap, _gc, x, y, &item, 1);
}

CanvasRenderingContext2DDelegate::Size CanvasRenderingContext2DDelegate::sizeWithText(const wchar_t *pszText, int nLen) {
    // if (text.empty())
    //     return ccstd::array<float, 2>{0.0f, 0.0f};
    // XFontStruct *fs = XLoadQueryFont(dpy, "cursor");
    // CC_ASSERT(fs);
    // int font_ascent = 0;
    // int font_descent = 0;
    // XCharStruct overall;
    // XQueryTextExtents(_dis, fs -> fid, text.c_str(), text.length(), nullptr, &font_ascent, &font_descent, &overall);
    // return ccstd::array<float, 2>{static_cast<float>(overall.lbearing),
    //                             static_cast<float>(overall.rbearing)};
    return ccstd::array<float, 2>{0.0F, 0.0F};
}

void CanvasRenderingContext2DDelegate::prepareBitmap(int nWidth, int nHeight) {
}

void CanvasRenderingContext2DDelegate::releaseBuffer() {
    if (_fontDraw) {
        XftDrawDestroy(_fontDraw);
        _fontDraw = nullptr;
    }
    if (_gc) {
        XFreeGC(_dis, _gc);
        _gc = nullptr;
    }
    if (_pixmap) {
        XFreePixmap(_dis, _pixmap);
        _pixmap = None;
    }
    _imageData.clear();
}

void CanvasRenderingContext2DDelegate::fillTextureData() {
}

ccstd::array<float, 2> CanvasRenderingContext2DDelegate::convertDrawPoint(Point point, const ccstd::string &text) {
    const int width = textAdvance(text);
    if (_textAlign == TextAlign::CENTER) {
        point[0] -= width / 2.0f;
    } else if (_textAlign == TextAlign::RIGHT) {
        point[0] -= width;
    }

    if (_textBaseLine == TextBaseline::TOP) {
        point[1] += _font->ascent;
    } else if (_textBaseLine == TextBaseline::MIDDLE) {
        point[1] += (_font->ascent - _font->descent) / 2;
    } else if (_textBaseLine == TextBaseline::BOTTOM) {
        point[1] -= _font->descent;
    } else if (_textBaseLine == TextBaseline::ALPHABETIC) {
        // point[1] -= overall.ascent;
        //  X11 The default way of drawing text
    }

    return point;
}

void CanvasRenderingContext2DDelegate::setLineCap(const ccstd::string &lineCap) {
    _lineCap = LineSolid;
}

void CanvasRenderingContext2DDelegate::setLineJoin(const ccstd::string &lineJoin) {
    _lineJoin = JoinRound;
}

void CanvasRenderingContext2DDelegate::fillImageData(const Data & /* imageData */,
                                                     float /* imageWidth */,
                                                     float /* imageHeight */,
                                                     float /* offsetX */,
                                                     float /* offsetY */) {
    // XCreateImage(display, visual, DefaultDepth(display,DefaultScreen(display)), ZPixmap, 0, image32, width, height, 32, 0);
    // XPutImage(dpy, w, gc, image, 0, 0, 50, 60, 40, 30);
}

void CanvasRenderingContext2DDelegate::strokeText(const ccstd::string &text,
                                                  float x,
                                                  float y,
                                                  float /* maxWidth */) {
    if (text.empty() || !_font || !_fontDraw || _bufferWidth < 1.0F || _bufferHeight < 1.0F || _lineWidth <= 0.0F) {
        return;
    }
    const Point origin = convertDrawPoint(Point{x, y}, text);
    const int radius = static_cast<int>(std::ceil(_lineWidth / 2.0F));
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= radius * radius) {
                drawTextToPixmap(text, static_cast<int>(origin[0]) + dx, static_cast<int>(origin[1]) + dy, _strokeStyle);
            }
        }
    }
}

} // namespace cc
