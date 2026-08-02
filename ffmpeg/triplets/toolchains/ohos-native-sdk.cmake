set(OHOS_ARCH arm64-v8a CACHE STRING "" FORCE)
set(OHOS_SDK_NATIVE_PLATFORM ohos-23 CACHE STRING "" FORCE)
set(OHOS_STL c++_static CACHE STRING "" FORCE)

if(DEFINED ENV{OHOS_SDK_ROOT} AND NOT "$ENV{OHOS_SDK_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{OHOS_SDK_ROOT}" QTAV_OHOS_SDK_ROOT)
elseif(DEFINED ENV{OHOS_NDK} AND NOT "$ENV{OHOS_NDK}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{OHOS_NDK}" QTAV_OHOS_NATIVE_ROOT)
    get_filename_component(QTAV_OHOS_SDK_ROOT "${QTAV_OHOS_NATIVE_ROOT}" DIRECTORY)
elseif(DEFINED ENV{DEVECO_SDK_HOME} AND NOT "$ENV{DEVECO_SDK_HOME}" STREQUAL "")
    file(TO_CMAKE_PATH
        "$ENV{DEVECO_SDK_HOME}/default/openharmony"
        QTAV_OHOS_SDK_ROOT
    )
else()
    message(FATAL_ERROR
        "Set OHOS_SDK_ROOT to the OpenHarmony SDK root (the directory that contains native/)"
    )
endif()

set(QTAV_OHOS_TOOLCHAIN
    "${QTAV_OHOS_SDK_ROOT}/native/build/cmake/ohos.toolchain.cmake"
)
if(NOT EXISTS "${QTAV_OHOS_TOOLCHAIN}")
    message(FATAL_ERROR
        "OpenHarmony native toolchain was not found: ${QTAV_OHOS_TOOLCHAIN}"
    )
endif()

set(OHOS_SDK_ROOT "${QTAV_OHOS_SDK_ROOT}" CACHE PATH "" FORCE)
include("${QTAV_OHOS_TOOLCHAIN}")

# The SDK toolchain initializes CMAKE_*_FLAGS itself and therefore bypasses
# the flag injection performed by vcpkg's outer Linux toolchain. Re-apply the
# triplet flags after loading it. -Qunused-arguments is needed because the SDK
# always supplies --gcc-toolchain, including for compiler-only Meson probes.
# The explicit target also covers upstream code generators (notably libpng)
# which invoke CMAKE_C_COMPILER directly and omit CMAKE_C_COMPILER_TARGET.
function(qtav_ohos_append_flags_once VARIABLE FLAGS)
    if(NOT "${FLAGS}" STREQUAL "")
        string(FIND "${${VARIABLE}}" "${FLAGS}" QTAV_OHOS_FLAG_INDEX)
        if(QTAV_OHOS_FLAG_INDEX EQUAL -1)
            string(APPEND ${VARIABLE} " ${FLAGS}")
        endif()
    endif()
    set(${VARIABLE} "${${VARIABLE}}" CACHE STRING "" FORCE)
endfunction()

qtav_ohos_append_flags_once(CMAKE_C_FLAGS "${VCPKG_C_FLAGS}")
qtav_ohos_append_flags_once(CMAKE_CXX_FLAGS "${VCPKG_CXX_FLAGS}")
qtav_ohos_append_flags_once(CMAKE_C_FLAGS_DEBUG "${VCPKG_C_FLAGS_DEBUG}")
qtav_ohos_append_flags_once(CMAKE_CXX_FLAGS_DEBUG "${VCPKG_CXX_FLAGS_DEBUG}")
qtav_ohos_append_flags_once(CMAKE_C_FLAGS_RELEASE "${VCPKG_C_FLAGS_RELEASE}")
qtav_ohos_append_flags_once(CMAKE_CXX_FLAGS_RELEASE "${VCPKG_CXX_FLAGS_RELEASE}")
qtav_ohos_append_flags_once(
    CMAKE_C_FLAGS
    "--target=${CMAKE_C_COMPILER_TARGET}"
)
qtav_ohos_append_flags_once(
    CMAKE_CXX_FLAGS
    "--target=${CMAKE_CXX_COMPILER_TARGET}"
)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" CACHE STRING "" FORCE)
