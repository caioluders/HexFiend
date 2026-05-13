include(${CMAKE_CURRENT_SOURCE_DIR}/HexFiendCoreSources.cmake)
include(CTest)

set(HEXFIEND_GNUSTEP_ROOT "$ENV{HEXFIEND_GNUSTEP_ROOT}" CACHE PATH "Optional staged GNUstep root for Linux builds")
if(HEXFIEND_GNUSTEP_ROOT)
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${HEXFIEND_GNUSTEP_ROOT}")
    set(ENV{PKG_CONFIG_PATH} "${HEXFIEND_GNUSTEP_ROOT}/usr/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    file(GLOB HEXFIEND_STAGED_OBJC_INCLUDE_DIRS
        "${HEXFIEND_GNUSTEP_ROOT}/usr/lib/gcc/*/*/include"
    )
endif()

set(HEXFIEND_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")
set(HEXFIEND_INCLUDE_ROOT "${HEXFIEND_GENERATED_INCLUDE_DIR}/HexFiend")

file(MAKE_DIRECTORY "${HEXFIEND_GENERATED_INCLUDE_DIR}")
file(CREATE_LINK
    "${CMAKE_SOURCE_DIR}/framework/sources"
    "${HEXFIEND_INCLUDE_ROOT}"
    SYMBOLIC
)

set(_hexfiend_core_deps_found FALSE)

if(CMAKE_OBJC_COMPILER)
    if(APPLE)
        set(_hexfiend_core_deps_found TRUE)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(GNUSTEP_BASE QUIET gnustep-base)
        endif()
        if(GNUSTEP_BASE_FOUND)
            set(HEXFIEND_GNUSTEP_CFLAGS_OTHER ${GNUSTEP_BASE_CFLAGS_OTHER})
            list(REMOVE_ITEM HEXFIEND_GNUSTEP_CFLAGS_OTHER -flto=auto -MMD -MP)
            list(APPEND HEXFIEND_GNUSTEP_CFLAGS_OTHER -fconstant-string-class=NSConstantString)
            set(_hexfiend_core_deps_found TRUE)
        endif()
    endif()
endif()

if(_hexfiend_core_deps_found)
    add_library(HexFiendCore STATIC ${HEXFIEND_CORE_SOURCES} ${HEXFIEND_CORE_HEADERS})
    add_library(HexFiend::Core ALIAS HexFiendCore)

    target_include_directories(HexFiendCore
        PUBLIC
            "${HEXFIEND_GENERATED_INCLUDE_DIR}"
        PRIVATE
            "${CMAKE_SOURCE_DIR}/framework/sources"
            "${CMAKE_SOURCE_DIR}/framework/sources/BTree"
            "${CMAKE_SOURCE_DIR}/platform/linux/include"
    )
    if(HEXFIEND_STAGED_OBJC_INCLUDE_DIRS)
        target_include_directories(HexFiendCore PUBLIC ${HEXFIEND_STAGED_OBJC_INCLUDE_DIRS})
    else()
        # Clang does not search GCC's internal include paths, so locate the
        # GCC-shipped objc/ headers when building with clang on Linux.
        # We symlink only the objc/ subdirectory into a staging area to avoid
        # pulling in GCC intrinsic headers that are incompatible with clang.
        file(GLOB _gcc_objc_include_dirs "/usr/lib/gcc/*/*/include")
        foreach(_dir ${_gcc_objc_include_dirs})
            if(EXISTS "${_dir}/objc/objc.h")
                set(_objc_staging "${CMAKE_BINARY_DIR}/_objc_include")
                file(MAKE_DIRECTORY "${_objc_staging}")
                file(CREATE_LINK "${_dir}/objc" "${_objc_staging}/objc" SYMBOLIC)
                target_include_directories(HexFiendCore PUBLIC "${_objc_staging}")
                break()
            endif()
        endforeach()
    endif()

    set_source_files_properties(${HEXFIEND_CORE_SOURCES} PROPERTIES COMPILE_OPTIONS "-fblocks")
    target_compile_features(HexFiendCore PUBLIC c_std_11)

    if(APPLE)
        find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
        target_link_libraries(HexFiendCore PUBLIC "${FOUNDATION_FRAMEWORK}")
    else()
        target_include_directories(HexFiendCore PUBLIC ${GNUSTEP_BASE_INCLUDE_DIRS})
        target_compile_options(HexFiendCore PRIVATE ${HEXFIEND_GNUSTEP_CFLAGS_OTHER})
        target_link_libraries(HexFiendCore PUBLIC ${GNUSTEP_BASE_LIBRARIES} gnustep-base BlocksRuntime)
        target_link_directories(HexFiendCore PUBLIC ${GNUSTEP_BASE_LIBRARY_DIRS})
        if(HEXFIEND_GNUSTEP_ROOT)
            target_link_directories(HexFiendCore PUBLIC "${HEXFIEND_GNUSTEP_ROOT}/usr/lib")
        endif()
    endif()

    if(BUILD_TESTING)
        add_executable(HexFiendCoreSmokeTests tests/HexFiendCoreSmokeTests.m)
        target_link_libraries(HexFiendCoreSmokeTests PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,HexFiendCore>")
        target_compile_options(HexFiendCoreSmokeTests PRIVATE -fblocks)
        if(NOT APPLE)
            target_compile_options(HexFiendCoreSmokeTests PRIVATE ${HEXFIEND_GNUSTEP_CFLAGS_OTHER})
            target_link_libraries(HexFiendCoreSmokeTests PRIVATE gnustep-base BlocksRuntime)
            target_link_directories(HexFiendCoreSmokeTests PRIVATE ${GNUSTEP_BASE_LIBRARY_DIRS})
            if(HEXFIEND_STAGED_OBJC_INCLUDE_DIRS)
                target_include_directories(HexFiendCoreSmokeTests PRIVATE ${HEXFIEND_STAGED_OBJC_INCLUDE_DIRS})
            endif()
            if(HEXFIEND_GNUSTEP_ROOT)
                target_link_directories(HexFiendCoreSmokeTests PRIVATE "${HEXFIEND_GNUSTEP_ROOT}/usr/lib")
                list(APPEND GNUSTEP_BASE_LIBRARY_DIRS "${HEXFIEND_GNUSTEP_ROOT}/usr/lib")
            endif()
            set_target_properties(HexFiendCoreSmokeTests PROPERTIES
                BUILD_RPATH "${GNUSTEP_BASE_LIBRARY_DIRS}"
            )
        endif()
        add_test(NAME HexFiendCoreSmokeTests COMMAND HexFiendCoreSmokeTests)
    endif()
elseif(NOT CMAKE_OBJC_COMPILER)
    message(STATUS "HexFiendCore target disabled: no Objective-C compiler is available in this environment.")
else()
    message(STATUS "HexFiendCore target disabled: install Foundation support (for Linux, gnustep-base via pkg-config) to build the portable core.")
endif()
