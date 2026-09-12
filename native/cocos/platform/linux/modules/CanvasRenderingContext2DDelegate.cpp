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
#include <X11/Xlib.h>
#include <fontconfig/fcfreetype.h>
#include <algorithm>
#include <cmath>

namespace {
#define RGB(r, g, b)     (int)((int)r | (((int)g) << 8) | (((int)b) << 16))
#define RGBA(r, g, b, a) (int)((int)r | (((int)g) << 8) | (((int)b) << 16) | (((int)a) << 24))
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
    // called: set_lineWidth() -> beginPath() -> moveTo() -> lineTo() -> stroke(), when draw line
    XSetLineAttributes(_dis, _gc, static_cast<int>(_lineWidth), LineSolid, _lineCap, _lineJoin);
    XSetForeground(_dis, _gc, RGB(255, 255, 255));
}

void CanvasRenderingContext2DDelegate::closePath() {
}

void CanvasRenderingContext2DDelegate::moveTo(float x, float y) {
    // MoveToEx(_DC, static_cast<int>(x), static_cast<int>(-(y - _bufferHeight - _fontSize)), nullptr);
    _x = x;
    _y = y;
}

void CanvasRenderingContext2DDelegate::lineTo(float x, float y) {
    // LineTo(_DC,  static_cast<int>(x),  static_cast<int>(-(y - _bufferHeight - _fontSize)));
    XDrawLine(_dis, _pixmap, _gc, _x, _y, x, y);
}

void CanvasRenderingContext2DDelegate::stroke() {
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
    readPixmapPixels();
}

void CanvasRenderingContext2DDelegate::drawTextToPixmap(const ccstd::string &text, int x, int y, unsigned long style) {
    const auto channel = [](unsigned long color, unsigned int shift) {
        return static_cast<unsigned short>(((color >> shift) & 0xffU) * 257U);
    };
    const XftColor color{0, {channel(style, 0), channel(style, 8), channel(style, 16), channel(style, 24)}};
    XftDrawStringUtf8(_fontDraw, &color, _font, x, y,
                      reinterpret_cast<const FcChar8 *>(text.c_str()), static_cast<int>(text.length()));
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
    XGlyphInfo extents{};
    XftTextExtentsUtf8(_dis, _font, reinterpret_cast<const FcChar8 *>(text.c_str()),
                       static_cast<int>(text.length()), &extents);
    return ccstd::array<float, 2>{static_cast<float>(extents.xOff), static_cast<float>(_font->height)};
}

void CanvasRenderingContext2DDelegate::updateFont(const ccstd::string &fontName,
                                                  float fontSize,
                                                  bool bold,
                                                  bool italic,
                                                  bool oblique,
                                                  bool /* smallCaps */) {
    _fontName = fontName;
    _fontSize = static_cast<int>(fontSize);
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
    FcPatternAddInteger(requested, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(requested, FC_SLANT, italic ? FC_SLANT_ITALIC : (oblique ? FC_SLANT_OBLIQUE : FC_SLANT_ROMAN));
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
    if (_font) {
        XftFontClose(_dis, _font);
        _font = nullptr;
    }
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
    XGlyphInfo extents{};
    XftTextExtentsUtf8(_dis, _font, reinterpret_cast<const FcChar8 *>(text.c_str()),
                       static_cast<int>(text.length()), &extents);
    const int width = extents.xOff;
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

void CanvasRenderingContext2DDelegate::fill() {
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
    readPixmapPixels();
}

void CanvasRenderingContext2DDelegate::rect(float /* x */,
                                            float /* y */,
                                            float /* w */,
                                            float /* h */) {
}

} // namespace cc
