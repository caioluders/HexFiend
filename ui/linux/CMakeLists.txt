include(FetchContent)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(TCL REQUIRED tcl)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)
find_path(HEXFIEND_SDL2_INCLUDE_DIR SDL.h
    PATHS /usr/include/SDL2 /usr/local/include/SDL2
    REQUIRED
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_sdl_opengl STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui_sdl_opengl PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${HEXFIEND_SDL2_INCLUDE_DIR}
)
target_compile_features(imgui_sdl_opengl PUBLIC cxx_std_17)
target_compile_options(imgui_sdl_opengl PRIVATE ${SDL2_CFLAGS_OTHER})
target_link_libraries(imgui_sdl_opengl PUBLIC ${SDL2_LIBRARIES} OpenGL::GL)
target_link_directories(imgui_sdl_opengl PUBLIC ${SDL2_LIBRARY_DIRS})

add_executable(HexFiendLinux
    src/EngineDocument.mm
    src/main.cpp
)

target_include_directories(HexFiendLinux PRIVATE
    src
    ${HEXFIEND_SDL2_INCLUDE_DIR}
    ${TCL_INCLUDE_DIRS}
)
target_compile_features(HexFiendLinux PRIVATE cxx_std_17)
target_compile_options(HexFiendLinux PRIVATE ${SDL2_CFLAGS_OTHER})
target_compile_options(HexFiendLinux PRIVATE ${TCL_CFLAGS_OTHER})
target_compile_options(HexFiendLinux PRIVATE
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fblocks;-fconstant-string-class=NSConstantString>"
)
target_link_libraries(HexFiendLinux PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,HexFiendCore>"
    imgui_sdl_opengl
    ${TCL_LIBRARIES}
    ZLIB::ZLIB
)
target_link_directories(HexFiendLinux PRIVATE ${SDL2_LIBRARY_DIRS} ${TCL_LIBRARY_DIRS})

if(HEXFIEND_GNUSTEP_ROOT)
    set_target_properties(HexFiendLinux PROPERTIES
        BUILD_RPATH "${HEXFIEND_GNUSTEP_ROOT}/usr/lib"
        INSTALL_RPATH "${HEXFIEND_GNUSTEP_ROOT}/usr/lib"
    )
endif()

configure_file(
    packaging/org.hexfiend.HexFiend.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/org.hexfiend.HexFiend.desktop
    COPYONLY
)

install(TARGETS HexFiendLinux
    RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.hexfiend.HexFiend.desktop
    DESTINATION share/applications
)
install(FILES ${CMAKE_SOURCE_DIR}/docs/images/hex_icon.png
    DESTINATION share/pixmaps
    RENAME hexfiend.png
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/templates
    DESTINATION share/hexfiend
)

if(BUILD_TESTING)
    add_executable(HexFiendLinuxEngineSmokeTests
        src/EngineDocument.mm
        tests/EngineDocumentSmokeTests.cpp
    )
    target_include_directories(HexFiendLinuxEngineSmokeTests PRIVATE src)
    target_compile_features(HexFiendLinuxEngineSmokeTests PRIVATE cxx_std_17)
    target_compile_options(HexFiendLinuxEngineSmokeTests PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fblocks;-fconstant-string-class=NSConstantString>"
    )
    target_link_libraries(HexFiendLinuxEngineSmokeTests PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,HexFiendCore>"
    )
    if(HEXFIEND_GNUSTEP_ROOT)
        set_target_properties(HexFiendLinuxEngineSmokeTests PROPERTIES
            BUILD_RPATH "${HEXFIEND_GNUSTEP_ROOT}/usr/lib"
        )
    endif()
    add_test(NAME HexFiendLinuxEngineSmokeTests COMMAND HexFiendLinuxEngineSmokeTests)
    add_test(NAME HexFiendLinuxSelfTests COMMAND HexFiendLinux --self-test)
endif()
