add_executable(cocos-linux-canvas-lifecycle canvas_lifecycle.cpp)
target_link_libraries(cocos-linux-canvas-lifecycle PRIVATE ${ENGINE_NAME} dl)
target_compile_definitions(cocos-linux-canvas-lifecycle PRIVATE
    CC_TEST_NO_SPACE_FONT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/OpenSans-NoSpace.ttf")
target_link_options(cocos-linux-canvas-lifecycle PRIVATE
    -Wl,--wrap=XCreateGC -Wl,--wrap=XFreeGC
    -Wl,--wrap=XCreatePixmap -Wl,--wrap=XFreePixmap
    -Wl,--wrap=XftFontOpenPattern -Wl,--wrap=XftFontClose
    -Wl,--wrap=XftDrawCreateAlpha -Wl,--wrap=XftDrawDestroy)
