# Linux canvas resource lifecycle regression

Configure a Linux native Cocos consumer with
`-DCC_BUILD_LINUX_CANVAS_TESTS=ON`, then build the
`cocos-linux-canvas-lifecycle` target. Run that executable with a working X11
`DISPLAY` (a dedicated Xvfb display is sufficient). It creates a hidden window
and exercises the production canvas delegate without gameplay or audio.

The test poisons placement storage before constructing an unused canvas, then
checks zero-sized buffers, repeated buffer allocation and font replacement.
Linker wrappers count actual Xlib resource acquisitions and releases; they do
not replace allocation or rendering. Each phase requires exact balanced
ownership, and missing display initialization is a failure.
