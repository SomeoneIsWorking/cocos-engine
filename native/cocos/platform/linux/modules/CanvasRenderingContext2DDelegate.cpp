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
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
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
    const auto pixels = static_cast<std::size_t>(_bufferWidth) * static_cast<std::size_t>(_bufferHeight);
    const auto textureSize = static_cast<int>(pixels * 4);
    auto *data = static_cast<int8_t *>(malloc(sizeof(int8_t) * textureSize));
    memset(data, 0x00, textureSize);
    _imageData.fastSet((uint8_t *)data, textureSize);

    // The canvas is this buffer, and it is a new canvas: all zero, which in straight RGBA8 is
    // a fully transparent pixel.
    _coverageBytes = pixels;
    _coverage = static_cast<unsigned char *>(calloc(pixels, 1));
    CC_ASSERT_NOT_NULL(_coverage);
    // And that is the whole of a buffer's X resources: none. This delegate used to ask for a
    // depth-32 pixmap and draw straight into it, which is right on a server that has a
    // 32-bit visual and silently wrong on one that does not. An ordinary X server offers
    // depth 24 with masks 0xff0000, 0xff00, 0xff and *no alpha at all*, and refuses depth 32
    // outright, so the drawable that came back had red_mask, green_mask and blue_mask all
    // zero. There was then nowhere to put an alpha: a "cleared" canvas could not be cleared,
    // clearRect's zero foreground painted opaque black, and every canvas this delegate
    // produced on Linux had a non-transparent background -- which for a label meant its
    // texture tinted everything behind it and showed a second, dimmer copy of its own text.
    //
    // The pixels live in this process, glyph coverage comes from FreeType, and the colour is
    // composited here where the arithmetic is exact. A canvas that owns no server resource
    // also cannot leak one.
}

void CanvasRenderingContext2DDelegate::clearCoverage() {
    if (_coverage != nullptr) {
        memset(_coverage, 0, _coverageBytes);
    }
}

// The mask Xft just drew, read back as coverage. Runs are laid out at their own advances and
// do not overlap within one string, so a straight copy is the whole of it.
// The coverage byte out of a mask image. An A8 drawable carries its value in the low
// byte, not in the alpha bits where a 32-bit drawable would put it, and getting this
// wrong reads every glyph as fully covered -- a slab rather than a letter.
static unsigned char coverageByte(const XImage *image, uint32_t pixel) {
    return image->depth <= 8 ? static_cast<unsigned char>(pixel & 0xffU)
                             : static_cast<unsigned char>((pixel >> 24U) & 0xffU);
}

void CanvasRenderingContext2DDelegate::sourceOver(unsigned char *pixel, unsigned char red, unsigned char green,
                                                  unsigned char blue, unsigned char alpha) const {
    if (alpha == 0) {
        return;
    }
    const unsigned int destinationAlpha = pixel[3];
    if (destinationAlpha == 0) {
        // Exact, and the case this whole change exists for: onto a cleared canvas the result
        // is the source, not an approximation of it.
        pixel[0] = red;
        pixel[1] = green;
        pixel[2] = blue;
        pixel[3] = alpha;
        return;
    }
    const unsigned int inverse = 255U - alpha;
    const unsigned int outAlpha = alpha + (destinationAlpha * inverse + 127U) / 255U;
    const unsigned int source[3] = {red, green, blue};
    for (unsigned int channel = 0; channel < 3; ++channel) {
        const unsigned int numerator = source[channel] * alpha + (pixel[channel] * destinationAlpha * inverse + 127U) / 255U;
        pixel[channel] = static_cast<unsigned char>(std::min(255U, (numerator + outAlpha / 2U) / outAlpha));
    }
    pixel[3] = static_cast<unsigned char>(outAlpha);
}

void CanvasRenderingContext2DDelegate::compositeCoverage(unsigned long style) {
    if (_coverage == nullptr || _imageData.isNull()) {
        return;
    }
    const auto red = static_cast<unsigned char>((style >> PIXEL_RED_SHIFT) & 0xffU);
    const auto green = static_cast<unsigned char>((style >> PIXEL_GREEN_SHIFT) & 0xffU);
    const auto blue = static_cast<unsigned char>((style >> PIXEL_BLUE_SHIFT) & 0xffU);
    const auto styleAlpha = static_cast<unsigned int>((style >> PIXEL_ALPHA_SHIFT) & 0xffU);
    unsigned char *pixels = _imageData.getBytes();
    for (int y = 0; y < static_cast<int>(_bufferHeight); ++y) {
        for (int x = 0; x < static_cast<int>(_bufferWidth); ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(_bufferWidth) +
                                   static_cast<std::size_t>(x);
            const unsigned int coverage = _coverage[at];
            if (coverage == 0 || styleAlpha == 0) {
                continue;
            }
            const auto alpha = static_cast<unsigned char>((coverage * styleAlpha + 127U) / 255U);
            sourceOver(pixels + at * 4U, red, green, blue, alpha);
        }
    }
}

bool CanvasRenderingContext2DDelegate::clipRect(float x, float y, float w, float h, int &left, int &top,
                                                int &right, int &bottom) const {
    if (_bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return false;
    }
    // A rect may leave the canvas entirely, and a negative origin is not an error.
    left = std::max(0, static_cast<int>(std::lround(x)));
    top = std::max(0, static_cast<int>(std::lround(y)));
    right = std::min(static_cast<int>(_bufferWidth), static_cast<int>(std::lround(x + w)));
    bottom = std::min(static_cast<int>(_bufferHeight), static_cast<int>(std::lround(y + h)));
    return right > left && bottom > top;
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
    if (_coverage == nullptr || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }
    const auto width = static_cast<unsigned int>(_bufferWidth);
    const auto height = static_cast<unsigned int>(_bufferHeight);
    XRenderPictFormat *maskFormat = XRenderFindStandardFormat(_dis, PictStandardA8);
    CC_ASSERT_NOT_NULL(maskFormat);
    const Pixmap maskPixmap = XCreatePixmap(_dis, _win, width, height, 8);
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
    // The mask becomes the coverage and the colour is composited through it here, rather
    // than handed to XRender as a target picture: the target would have to be an ARGB32
    // drawable, and this server has no alpha to put in one.
    XImage *image = XGetImage(_dis, maskPixmap, 0, 0, _bufferWidth, _bufferHeight, AllPlanes, ZPixmap);
    if (image != nullptr) {
        for (int y = 0; y < image->height; ++y) {
            for (int x = 0; x < image->width; ++x) {
                const auto pixel = static_cast<uint32_t>(XGetPixel(image, x, y));
                const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(image->width) +
                                       static_cast<std::size_t>(x);
                _coverage[at] = static_cast<unsigned char>(
                    std::min(255U, static_cast<unsigned int>(_coverage[at]) + coverageByte(image, pixel)));
            }
        }
        XDestroyImage(image);
    }
    XRenderFreePicture(_dis, white);
    XRenderFreePicture(_dis, mask);
    XFreePixmap(_dis, maskPixmap);
    compositeCoverage(style);
}

// The distance from a point to a segment, so a stroke can be covered analytically instead
// of by stamping a disc along it. That keeps the stroke antialiased and alpha-correct, which
// the core X line it replaces was not: it wrote pixels rather than blending them.
static float distanceToSegment(double px, double py, const XPointDouble &from, const XPointDouble &to) {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double lengthSquared = dx * dx + dy * dy;
    double t = 0.0;
    if (lengthSquared > 0.0) {
        t = ((px - from.x) * dx + (py - from.y) * dy) / lengthSquared;
        t = std::clamp(t, 0.0, 1.0);
    }
    const double nearestX = from.x + t * dx;
    const double nearestY = from.y + t * dy;
    return static_cast<float>(std::hypot(px - nearestX, py - nearestY));
}

void CanvasRenderingContext2DDelegate::strokeSegmentCoverage(const XPointDouble &from, const XPointDouble &to,
                                                            float radius) {
    if (_coverage == nullptr) {
        return;
    }
    const int left = std::max(0, static_cast<int>(std::floor(std::min(from.x, to.x) - radius - 1.0)));
    const int top = std::max(0, static_cast<int>(std::floor(std::min(from.y, to.y) - radius - 1.0)));
    const int right = std::min(static_cast<int>(_bufferWidth), static_cast<int>(std::ceil(std::max(from.x, to.x) + radius + 1.0)));
    const int bottom = std::min(static_cast<int>(_bufferHeight), static_cast<int>(std::ceil(std::max(from.y, to.y) + radius + 1.0)));
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const float distance = distanceToSegment(x + 0.5, y + 0.5, from, to);
            // One pixel of ramp across the edge, which is what antialiasing means.
            const float edge = radius + 0.5F - distance;
            if (edge <= 0.0F) {
                continue;
            }
            const auto coverage = static_cast<unsigned char>(
                std::min(255.0F, std::max(0.0F, edge) * 255.0F));
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(_bufferWidth) +
                                   static_cast<std::size_t>(x);
            _coverage[at] = static_cast<unsigned char>(std::min(255U, static_cast<unsigned int>(_coverage[at]) + coverage));
        }
    }
}

// Strokes the current path's subpaths at the line width, into the same coverage the fill
// uses, so both composite through one arithmetic.
void CanvasRenderingContext2DDelegate::strokePath(unsigned long style) {
    if (_coverage == nullptr || _lineWidth <= 0.0F) {
        return;
    }
    clearCoverage();
    const float radius = _lineWidth / 2.0F;
    for (const auto &subpath : _path) {
        for (std::size_t index = 1; index < subpath.size(); ++index) {
            strokeSegmentCoverage(subpath[index - 1], subpath[index], radius);
        }
        // A subpath of a single point is a dot in the web canvas, and a stroke of one.
        if (subpath.size() == 1) {
            strokeSegmentCoverage(subpath[0], subpath[0], radius);
        }
    }
    compositeCoverage(style);
}

void CanvasRenderingContext2DDelegate::saveContext() {
}

void CanvasRenderingContext2DDelegate::restoreContext() {
}

void CanvasRenderingContext2DDelegate::clearRect(float x, float y, float w, float h) {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    if (_imageData.isNull() || !clipRect(x, y, w, h, left, top, right, bottom)) {
        return;
    }
    // Zero is a fully transparent pixel in straight RGBA8, so this is what "cleared" means.
    // The web's clearRect ignores the fill style and any clipping region; the clip region is
    // not tracked at all by this delegate, so nothing else has to be honoured here.
    unsigned char *pixels = _imageData.getBytes();
    for (int row = top; row < bottom; ++row) {
        memset(pixels + (static_cast<std::size_t>(row) * static_cast<std::size_t>(_bufferWidth) +
                         static_cast<std::size_t>(left)) * 4U,
               0, static_cast<std::size_t>(right - left) * 4U);
    }
    // The coverage mask is scratch for the shape being drawn, and a shape that starts after
    // a partial clear must not inherit coverage from before it.
    for (int row = top; row < bottom; ++row) {
        memset(_coverage + static_cast<std::size_t>(row) * static_cast<std::size_t>(_bufferWidth) +
                   static_cast<std::size_t>(left),
               0, static_cast<std::size_t>(right - left));
    }
}

void CanvasRenderingContext2DDelegate::fillRect(float x, float y, float w, float h) {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    if (_imageData.isNull() || !clipRect(x, y, w, h, left, top, right, bottom)) {
        return;
    }
    // Source-over with the fill style's own alpha, which is what the web does. The core X
    // fill this replaces could not: it wrote a premultiplied pixel, which only blends if
    // the target carries an alpha, and this server's target cannot.
    const auto red = static_cast<unsigned char>((_fillStyle >> PIXEL_RED_SHIFT) & 0xffU);
    const auto green = static_cast<unsigned char>((_fillStyle >> PIXEL_GREEN_SHIFT) & 0xffU);
    const auto blue = static_cast<unsigned char>((_fillStyle >> PIXEL_BLUE_SHIFT) & 0xffU);
    const auto alpha = static_cast<unsigned char>((_fillStyle >> PIXEL_ALPHA_SHIFT) & 0xffU);
    unsigned char *pixels = _imageData.getBytes();
    for (int row = top; row < bottom; ++row) {
        for (int column = left; column < right; ++column) {
            const std::size_t at = (static_cast<std::size_t>(row) * static_cast<std::size_t>(_bufferWidth) +
                                    static_cast<std::size_t>(column)) * 4U;
            sourceOver(pixels + at, red, green, blue, alpha);
        }
    }
}

void CanvasRenderingContext2DDelegate::fillText(const ccstd::string &text, float x, float y, float /*maxWidth*/) {
    if (text.empty() || !_font || _coverage == nullptr || _bufferWidth < 1.0F || _bufferHeight < 1.0F) {
        return;
    }

    Point offsetPoint = convertDrawPoint(Point{x, y}, text);
    drawTextToPixmap(text, static_cast<int>(offsetPoint[0]), static_cast<int>(offsetPoint[1]), _fillStyle);
}

// Composites one rasterised glyph into the coverage buffer. The glyph bitmap is FreeType's
// own coverage, which is the one thing this delegate must not lose: it is what makes a
// letter a letter rather than a slab.
void CanvasRenderingContext2DDelegate::blitGlyphCoverage(int penX, int baselineY, const FT_GlyphSlot &slot) {
    if (_coverage == nullptr || slot == nullptr || slot->bitmap.buffer == nullptr) {
        return;
    }
    const FT_Bitmap &bitmap = slot->bitmap;
    const int left = penX + slot->bitmap_left;
    const int top = baselineY - slot->bitmap_top;
    const int width = static_cast<int>(_bufferWidth);
    const int height = static_cast<int>(_bufferHeight);
    for (unsigned int row = 0; row < bitmap.rows; ++row) {
        const int y = top + static_cast<int>(row);
        if (y < 0 || y >= height) {
            continue;
        }
        for (unsigned int column = 0; column < bitmap.width; ++column) {
            const int x = left + static_cast<int>(column);
            if (x < 0 || x >= width) {
                continue;
            }
            const auto *source = bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch + column;
            unsigned int coverage = 0;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                coverage = *source;
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                coverage = ((*source >> (7U - (column % 8U))) & 1U) != 0U ? 255U : 0U;
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                // A colour glyph carries its own colour; the canvas draws with one fill
                // style, so its alpha is the coverage and its colour is not used.
                coverage = source[3];
            }
            if (coverage == 0) {
                continue;
            }
            auto &target = _coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x)];
            target = static_cast<unsigned char>(std::max(static_cast<unsigned int>(target), coverage));
        }
    }
}

void CanvasRenderingContext2DDelegate::drawTextToPixmap(const ccstd::string &text, int x, int y, unsigned long style) {
    if (_coverage == nullptr || _font == nullptr) {
        return;
    }
    // Glyphs are rasterised through FreeType, from the face the Xft font already wraps, and
    // the coverage is composited here. Xft cannot do this job on this platform: it produces
    // antialiased output by compositing into a 32-bit drawable, and a server with no
    // 32-bit visual has nowhere to put the alpha, so `XftDrawStringUtf8` into a mask draws
    // nothing at all. Measured: a 128x128 canvas with "A A" drawn into it came back with
    // 16384 of 16384 pixels fully transparent.
    clearCoverage();
    int penX = x;
    for (const auto &run : resolveTextRuns(text)) {
        FT_Face face = XftLockFace(run.font);
        if (face == nullptr) {
            continue;
        }
        FT_UInt previous = 0;
        for (std::size_t offset = 0; offset < run.length;) {
            FcChar32 codepoint = 0;
            const auto *bytes = reinterpret_cast<const FcChar8 *>(text.data() + run.begin + offset);
            int length = FcUtf8ToUcs4(bytes, &codepoint, static_cast<int>(run.length - offset));
            if (length <= 0) {
                codepoint = static_cast<unsigned char>(text[run.begin + offset]);
                length = 1;
            }
            offset += static_cast<std::size_t>(length);
            const FT_UInt index = FT_Get_Char_Index(face, codepoint);
            if (index == 0) {
                // A missing glyph still advances, and it has to advance by the same amount
                // measureText reports or the text will not sit where it was asked to sit --
                // and convertDrawPoint centres by that width. Xft's own extents are the one
                // place both agree, so they are what this asks for.
                XGlyphInfo extents{};
                XftTextExtentsUtf8(_dis, run.font, bytes, length, &extents);
                penX += extents.xOff;
                previous = 0;
                continue;
            }
            if (previous != 0 && !FT_HAS_COLOR(face)) {
                FT_Vector kerning{};
                FT_Get_Kerning(face, previous, index, FT_KERNING_DEFAULT, &kerning);
                penX += kerning.x >> 6;
            }
            if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) == 0 &&
                FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == 0) {
                blitGlyphCoverage(penX, y, face->glyph);
                penX += face->glyph->advance.x >> 6;
            }
            previous = FT_HAS_COLOR(face) ? 0 : index;
        }
        XftUnlockFace(run.font);
    }
    compositeCoverage(style);
}

// The canvas's pixels are this delegate's own buffer now, so there is nothing to read back:
// every draw above has already composited into it. The interface still asks, because the
// other platforms' delegates draw into a drawable and do need to copy out of one.
void CanvasRenderingContext2DDelegate::updateData() {
}

void CanvasRenderingContext2DDelegate::releaseBuffer() {
    free(_coverage);
    _coverage = nullptr;
    _coverageBytes = 0;
    _imageData.clear();
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
    if (text.empty() || !_font || _coverage == nullptr || _bufferWidth < 1.0F || _bufferHeight < 1.0F || _lineWidth <= 0.0F) {
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
