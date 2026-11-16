cmake_minimum_required(VERSION 3.21)
project(SogouSkinTools LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOGOU_SKIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../MoqiTextService/SogouSkin")
set(MINIZ_DIR "${SOGOU_SKIN_DIR}/third_party/miniz")

add_library(SogouSkin STATIC
    "${SOGOU_SKIN_DIR}/SogouSkin.cpp"
    "${SOGOU_SKIN_DIR}/SogouSkinStatusBar.cpp"
    "${MINIZ_DIR}/miniz.c"
    "${MINIZ_DIR}/miniz_tdef.c"
    "${MINIZ_DIR}/miniz_tinfl.c"
    "${MINIZ_DIR}/miniz_zip.c"
)
target_include_directories(SogouSkin PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../MoqiTextService"
    "${MINIZ_DIR}"
)
target_compile_definitions(SogouSkin PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(SogouSkin PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:char8_t->
    )
endif()
target_link_libraries(SogouSkin PUBLIC gdiplus shlwapi user32 gdi32)

add_executable(SogouSkinRenderTest
    SogouSkinRenderTest/main.cpp
)
target_link_libraries(SogouSkinRenderTest PRIVATE SogouSkin)
target_compile_definitions(SogouSkinRenderTest PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(SogouSkinRenderTest PRIVATE /utf-8 /Zc:char8_t-)
endif()

# Headless logic tests: key-repeat state machine, symbol menu/id mapping and
# search-host exclusion.  These need no input method/TSF context.
add_executable(SogouLogicTest
    SogouLogicTest/main.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../MoqiTextService/KeyRepeatAccelerator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../MoqiTextService/SogouSymbols.cpp"
)
target_include_directories(SogouLogicTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../MoqiTextService"
)
target_link_libraries(SogouLogicTest PRIVATE SogouSkin user32)
target_compile_definitions(SogouLogicTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(SogouLogicTest PRIVATE /utf-8 /Zc:char8_t-)
endif()

# Native settings UI.  In the main build the renderer target is called
# MoqiSogouSkin; the standalone SogouSkinTools build defines SogouSkin.
if(TARGET MoqiSogouSkin)
    set(SOGOU_SETTINGS_RENDERER MoqiSogouSkin)
else()
    set(SOGOU_SETTINGS_RENDERER SogouSkin)
endif()

add_executable(SogouSkinSettings WIN32
    SogouSkinSettings/main.cpp
)
target_link_libraries(SogouSkinSettings PRIVATE
    ${SOGOU_SETTINGS_RENDERER}
    comctl32
    comdlg32
    shell32
    shlwapi
    gdiplus
)
target_compile_definitions(SogouSkinSettings PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(SogouSkinSettings PRIVATE /utf-8 /Zc:char8_t-)
endif()
