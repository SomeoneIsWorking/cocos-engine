# Linux canvas resource lifecycle regression

Configure a Linux native Cocos consumer with
`-DCC_BUILD_LINUX_CANVAS_TESTS=ON`, then build the
`cocos-linux-canvas-lifecycle` target. Run that executable with a working X11
`DISPLAY` (a dedicated Xvfb display is sufficient). It creates a hidden window
and exercises the production canvas delegate without gameplay or audio.

The test poisons placement storage before constructing an unused canvas, then
checks zero-sized buffers, repeated buffer allocation and font replacement.
It also measures and rasterizes 60 px text through the production delegate so
the old X core-font fallback cannot pass. Linker wrappers count actual Xlib
and Xft resource acquisitions and releases; they do not replace allocation or
rendering. Each phase requires exact balanced ownership, and missing display
initialization is a failure.

The same test calls the production window's `closeWindow()` and drains events
through its window manager. It requires exactly one engine `CLOSE` event for
that window, so a generic SDL `QUIT` notification cannot masquerade as the
engine's close lifecycle. It consumes an SDL window ID before creating the
engine window to prove the translation between their independent ID namespaces.
An SDL event filter then rejects another close request: the engine must report
the refusal through its logger and must not deliver a spurious close event.
