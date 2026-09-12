#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "application/ApplicationManager.h"
#include "application/BaseGame.h"
#include "engine/EngineEvents.h"
#include "platform/BasePlatform.h"
#include "platform/interfaces/modules/ISystemWindow.h"
#include "platform/interfaces/modules/ISystemWindowManager.h"
#include "platform/linux/modules/CanvasRenderingContext2DDelegate.h"

#include <SDL2/SDL_events.h>
#include <SDL2/SDL_video.h>
#include <X11/X.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>

namespace {
int graphicsContexts = 0;
int pixmaps = 0;
int fonts = 0;
int fontDraws = 0;

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
    require(fontDraws == 0, "font draw was leaked or freed without acquisition");
}

void verifyWindowClose(cc::ISystemWindowManager &windows, cc::ISystemWindow &window) {
    // Flush initial window notifications before observing the close request.
    windows.processEvent();
    std::vector<cc::WindowEvent> closeEvents;
    cc::events::WindowEvent::Listener listener;
    listener.bind([&closeEvents](const cc::WindowEvent &event) {
        if (event.type == cc::WindowEvent::Type::CLOSE || event.type == cc::WindowEvent::Type::QUIT) {
            closeEvents.push_back(event);
        }
    });
    window.closeWindow();
    windows.processEvent();
    require(closeEvents.size() == 1, "one production close request must deliver exactly one close event");
    require(closeEvents.front().type == cc::WindowEvent::Type::CLOSE, "programmatic close emitted QUIT instead of the engine close lifecycle event");
    require(closeEvents.front().windowId == window.getWindowId(), "close event identified the wrong engine window");

    SDL_SetEventFilter([](void * /*userData*/, SDL_Event *event) -> int {
        return event->type != SDL_WINDOWEVENT || event->window.event != SDL_WINDOWEVENT_CLOSE;
    },
                       nullptr);
    window.closeWindow();
    windows.processEvent();
    SDL_SetEventFilter(nullptr, nullptr);
    require(closeEvents.size() == 1, "rejected close request must not deliver an engine close event");
}
} // namespace

extern "C" {
decltype(XCreateGC) realCreateGraphicsContext asm("__real_XCreateGC");
decltype(XFreeGC) realFreeGraphicsContext asm("__real_XFreeGC");
decltype(XCreatePixmap) realCreatePixmap asm("__real_XCreatePixmap");
decltype(XFreePixmap) realFreePixmap asm("__real_XFreePixmap");
decltype(XftFontOpenPattern) realOpenFont asm("__real_XftFontOpenPattern");
decltype(XftFontClose) realCloseFont asm("__real_XftFontClose");
decltype(XftDrawCreateAlpha) realCreateFontDraw asm("__real_XftDrawCreateAlpha");
decltype(XftDrawDestroy) realDestroyFontDraw asm("__real_XftDrawDestroy");
decltype(XCreateGC) createGraphicsContext asm("__wrap_XCreateGC");
decltype(XFreeGC) freeGraphicsContext asm("__wrap_XFreeGC");
decltype(XCreatePixmap) createPixmap asm("__wrap_XCreatePixmap");
decltype(XFreePixmap) freePixmap asm("__wrap_XFreePixmap");
decltype(XftFontOpenPattern) openFont asm("__wrap_XftFontOpenPattern");
decltype(XftFontClose) closeFont asm("__wrap_XftFontClose");
decltype(XftDrawCreateAlpha) createFontDraw asm("__wrap_XftDrawCreateAlpha");
decltype(XftDrawDestroy) destroyFontDraw asm("__wrap_XftDrawDestroy");

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

XftFont *openFont(Display *display, FcPattern *pattern) {
    auto *font = realOpenFont(display, pattern);
    fonts += font != nullptr;
    return font;
}

void closeFont(Display *display, XftFont *font) {
    require(font != nullptr && fonts > 0, "freeing an unallocated font");
    --fonts;
    realCloseFont(display, font);
}

XftDraw *createFontDraw(Display *display, Pixmap pixmap, int depth) {
    auto *draw = realCreateFontDraw(display, pixmap, depth);
    fontDraws += draw != nullptr;
    return draw;
}

void destroyFontDraw(XftDraw *draw) {
    require(draw != nullptr && fontDraws > 0, "freeing an unallocated font draw");
    --fontDraws;
    realDestroyFontDraw(draw);
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
    // SDL and engine window IDs belong to separate namespaces. Consume an SDL
    // ID first so this regression cannot pass by assuming the IDs are equal.
    auto *temporaryWindow = SDL_CreateWindow("SDL ID allocation", 0, 0, 8, 8, SDL_WINDOW_HIDDEN);
    require(temporaryWindow != nullptr, "temporary SDL window creation failed");
    SDL_DestroyWindow(temporaryWindow);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    cc::ISystemWindowInfo info;
    info.title = "Canvas lifecycle regression";
    info.width = 64;
    info.height = 64;
    info.flags = cc::ISystemWindow::CC_WINDOW_HIDDEN;
    auto *mainWindow = windows->createWindow(info);
    require(mainWindow != nullptr, "hidden main window creation failed");
    require(SDL_GetWindowFromID(mainWindow->getWindowId()) == nullptr, "test did not separate SDL and engine window IDs");

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
        require(graphicsContexts == 1 && pixmaps == 1 && fontDraws == 1, "initial buffer resources were not allocated");
        canvas.recreateBuffer(32, 32);
        require(graphicsContexts == 1 && pixmaps == 1 && fontDraws == 1, "buffer resize did not replace its resources");
        canvas.updateFont("sans-serif", 12, false, false, false, false);
        require(fonts == 1, "font was not loaded");
        canvas.updateFont("sans-serif", 18, false, false, false, false);
        require(fonts == 1, "font update did not replace its resource");
        canvas.updateFont("sans-serif", 60, false, false, false, false);
        require(canvas.measureText("Play")[1] >= 50, "requested 60px font was replaced by a small fallback");
        canvas.recreateBuffer(0, 0);
        require(graphicsContexts == 0 && pixmaps == 0 && fontDraws == 0, "zero-sized buffer retained resources");
        canvas.recreateBuffer(128, 128);
        canvas.setTextAlign(Canvas::TextAlign::LEFT);
        canvas.setTextBaseline(Canvas::TextBaseline::TOP);
        canvas.setFillStyle(255, 255, 255, 255);
        canvas.fillText("Play", 1, 1, 0);
        const auto *pixels = canvas.getDataRef().getBytes();
        int firstRow = 128;
        int lastRow = -1;
        for (int row = 0; row < 128; ++row) {
            for (int column = 0; column < 128; ++column) {
                if (pixels[(row * 128 + column) * 4 + 3] != 0) {
                    firstRow = std::min(firstRow, row);
                    lastRow = row;
                }
            }
        }
        require(lastRow - firstRow >= 30, "60px text rendered at a small fallback size");
        XSync(canvas._dis, False);
    }
    requireReleased();
    verifyWindowClose(*windows, *mainWindow);
    std::puts("canvas lifecycle: scalable 60px text and font-owning contexts released all X11 resources");
    std::puts("window lifecycle: programmatic close delivered one CLOSE event with the engine window ID");
}
