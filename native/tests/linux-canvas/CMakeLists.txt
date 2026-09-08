add_executable(cocos-linux-canvas-lifecycle canvas_lifecycle.cpp)
target_link_libraries(cocos-linux-canvas-lifecycle PRIVATE ${ENGINE_NAME} dl)
target_link_options(cocos-linux-canvas-lifecycle PRIVATE
    -Wl,--wrap=XCreateGC -Wl,--wrap=XFreeGC
    -Wl,--wrap=XCreatePixmap -Wl,--wrap=XFreePixmap
    -Wl,--wrap=XLoadQueryFont -Wl,--wrap=XFreeFont)
