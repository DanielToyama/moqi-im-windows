add_library(MoqiSogouSkin STATIC
    SogouSkin.cpp
    SogouSkinStatusBar.cpp
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_zip.c
)

target_include_directories(MoqiSogouSkin PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
)

target_compile_definitions(MoqiSogouSkin PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(MoqiSogouSkin PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:char8_t->
    )
endif()

target_link_libraries(MoqiSogouSkin PUBLIC
    gdiplus
    shlwapi
)
