#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "application/ApplicationManager.h"
#include "application/BaseGame.h"
#include "platform/BasePlatform.h"
#include "platform/interfaces/modules/ISystemWindow.h"
#include "platform/interfaces/modules/ISystemWindowManager.h"
#include "platform/linux/modules/CanvasRenderingContext2DDelegate.h"

#include <X11/X.h>
#include <X11/Xlib.h>

namespace {
int graphicsContexts = 0;
int pixmaps = 0;
int fonts = 0;

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "canvas lifecycle failure: %s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

void requireReleased() {
    require(graphicsContexts == 0, "graphics context was leaked or freed without acquisition");
    require(pixmaps == 0, "pixmap was leaked or freed without acquisition");
    require(fonts == 0, "font was leaked or freed without acquisition");
}
} // namespace

extern "C" {
decltype(XCreateGC) realCreateGraphicsContext asm("__real_XCreateGC");
decltype(XFreeGC) realFreeGraphicsContext asm("__real_XFreeGC");
decltype(XCreatePixmap) realCreatePixmap asm("__real_XCreatePixmap");
decltype(XFreePixmap) realFreePixmap asm("__real_XFreePixmap");
decltype(XLoadQueryFont) realLoadFont asm("__real_XLoadQueryFont");
decltype(XFreeFont) realFreeFont asm("__real_XFreeFont");
decltype(XCreateGC) createGraphicsContext asm("__wrap_XCreateGC");
decltype(XFreeGC) freeGraphicsContext asm("__wrap_XFreeGC");
decltype(XCreatePixmap) createPixmap asm("__wrap_XCreatePixmap");
decltype(XFreePixmap) freePixmap asm("__wrap_XFreePixmap");
decltype(XLoadQueryFont) loadFont asm("__wrap_XLoadQueryFont");
decltype(XFreeFont) freeFont asm("__wrap_XFreeFont");

GC createGraphicsContext(Display *display, Drawable drawable, std::uintptr_t mask, XGCValues *values) {
    GC context = realCreateGraphicsContext(display, drawable, mask, values);
    graphicsContexts += context != nullptr;
    return context;
}

int freeGraphicsContext(Display *display, GC context) {
    require(context != nullptr && graphicsContexts > 0, "freeing an unallocated graphics context");
    --graphicsContexts;
    return realFreeGraphicsContext(display, context);
}

Pixmap createPixmap(Display *display, Drawable drawable, unsigned int width, unsigned int height, unsigned int depth) {
    const Pixmap pixmap = realCreatePixmap(display, drawable, width, height, depth);
    pixmaps += pixmap != None;
    return pixmap;
}

int freePixmap(Display *display, Pixmap pixmap) {
    require(pixmap != None && pixmaps > 0, "freeing an unallocated pixmap");
    --pixmaps;
    return realFreePixmap(display, pixmap);
}

XFontStruct *loadFont(Display *display, const char *name) {
    auto *font = realLoadFont(display, name);
    fonts += font != nullptr;
    return font;
}

int freeFont(Display *display, XFontStruct *font) {
    require(font != nullptr && fonts > 0, "freeing an unallocated font");
    --fonts;
    return realFreeFont(display, font);
}
}

// Satisfy the platform's entry contract through its normal registration API;
// the regression initializes the platform without entering the application loop.
CC_REGISTER_APPLICATION(cc::BaseGame);

int main() {
    auto *platform = cc::BasePlatform::getPlatform();
    require(platform->init() == 0, "platform initialization failed; run with a working X11 DISPLAY");
    auto *windows = platform->getInterface<cc::ISystemWindowManager>();
    require(windows != nullptr, "missing window manager");
    cc::ISystemWindowInfo info;
    info.title = "Canvas lifecycle regression";
    info.width = 64;
    info.height = 64;
    info.flags = cc::ISystemWindow::CC_WINDOW_HIDDEN;
    require(windows->createWindow(info) != nullptr, "hidden main window creation failed");

    using Canvas = cc::CanvasRenderingContext2DDelegate;
    alignas(Canvas) std::array<unsigned char, sizeof(Canvas)> storage;
    storage.fill(0xA5);
    auto *unallocated = new (storage.data()) Canvas;
    require(unallocated->_gc == nullptr, "new canvas contains an indeterminate graphics context");
    unallocated->~Canvas();
    requireReleased();

    {
        Canvas canvas;
        canvas.recreateBuffer(0, 0);
    }
    requireReleased();

    {
        Canvas canvas;
        canvas.recreateBuffer(16, 16);
        require(graphicsContexts == 1 && pixmaps == 1, "initial buffer resources were not allocated");
        canvas.recreateBuffer(32, 32);
        require(graphicsContexts == 1 && pixmaps == 1, "buffer resize did not replace its resources");
        canvas.updateFont("sans-serif", 12, false, false, false, false);
        require(fonts == 1, "font was not loaded");
        canvas.updateFont("sans-serif", 18, false, false, false, false);
        require(fonts == 1, "font update did not replace its resource");
        canvas.recreateBuffer(0, 0);
        require(graphicsContexts == 0 && pixmaps == 0, "zero-sized buffer retained resources");
        canvas.recreateBuffer(8, 8);
        XSync(canvas._dis, False);
    }
    requireReleased();
    std::puts("canvas lifecycle: construct-only, zero-sized, resized and font-owning contexts released all X11 resources");
}
