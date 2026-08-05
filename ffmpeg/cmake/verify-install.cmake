if(NOT DEFINED INSTALL_ROOT OR NOT DEFINED TRIPLET)
    message(FATAL_ERROR "INSTALL_ROOT and TRIPLET are required")
endif()

set(PREFIX "${INSTALL_ROOT}/${TRIPLET}")
set(REQUIRED_FILES
    include/libavcodec/avcodec.h
    include/libavformat/avformat.h
    include/libavutil/avutil.h
    include/libavfilter/avfilter.h
    include/libswresample/swresample.h
    include/libswscale/swscale.h
    include/smb2/smb2.h
    include/openssl/ssl.h
    include/ass/ass.h
    include/libplacebo/config.h
    include/dav1d/dav1d.h
    include/vulkan/vulkan.h
    lib/pkgconfig/libavcodec.pc
    lib/pkgconfig/libavformat.pc
    lib/pkgconfig/libplacebo.pc
    lib/pkgconfig/libsmb2.pc
)

foreach(REQUIRED_FILE IN LISTS REQUIRED_FILES)
    if(NOT EXISTS "${PREFIX}/${REQUIRED_FILE}")
        message(FATAL_ERROR "Required dependency output is missing: ${PREFIX}/${REQUIRED_FILE}")
    endif()
endforeach()

file(READ "${PREFIX}/include/libavcodec/version_major.h" AVCODEC_VERSION_HEADER)
if(NOT AVCODEC_VERSION_HEADER MATCHES "#define LIBAVCODEC_VERSION_MAJOR[ \t]+62")
    message(FATAL_ERROR "QtAVCore requires FFmpeg 8 / libavcodec major 62")
endif()

if(TRIPLET MATCHES "ohos")
    if(NOT EXISTS "${PREFIX}/include/libavutil/hwcontext_oh.h")
        message(FATAL_ERROR "OHOS FFmpeg is missing its OHCodec hardware-context header")
    endif()
    if(NOT EXISTS "${PREFIX}/include/libavcodec/ohcodec_surface.h")
        message(FATAL_ERROR "OHOS FFmpeg is missing its explicit OHCodec surface-output API")
    endif()

    find_program(QTAV_OHOS_LLVM_NM
        NAMES llvm-nm llvm-nm.exe
        HINTS "$ENV{OHOS_SDK_ROOT}/native/llvm/bin"
        NO_DEFAULT_PATH
    )
    if(NOT QTAV_OHOS_LLVM_NM)
        find_program(QTAV_OHOS_LLVM_NM NAMES llvm-nm llvm-nm.exe)
    endif()
    if(NOT QTAV_OHOS_LLVM_NM)
        message(FATAL_ERROR "llvm-nm is required to verify the OHCodec archive")
    endif()

    execute_process(
        COMMAND "${QTAV_OHOS_LLVM_NM}" "${PREFIX}/lib/libavcodec.a"
        RESULT_VARIABLE QTAV_OHOS_NM_RESULT
        OUTPUT_VARIABLE QTAV_OHOS_AVCODEC_SYMBOLS
        ERROR_VARIABLE QTAV_OHOS_NM_ERROR
    )
    if(NOT QTAV_OHOS_NM_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Could not inspect the OHOS libavcodec archive: ${QTAV_OHOS_NM_ERROR}"
        )
    endif()
    foreach(QTAV_OHOS_CODEC_SYMBOL IN ITEMS
        av_ohcodec_release_buffer
        av_ohcodec_render_buffer_at_time
        ff_h264_oh_decoder
        ff_hevc_oh_decoder
    )
        if(NOT QTAV_OHOS_AVCODEC_SYMBOLS MATCHES
           "(^|[\r\n])[^\r\n]*${QTAV_OHOS_CODEC_SYMBOL}([\r\n]|$)")
            message(FATAL_ERROR
                "OHOS FFmpeg was built without ${QTAV_OHOS_CODEC_SYMBOL}"
            )
        endif()
    endforeach()
endif()

file(READ "${PREFIX}/include/libplacebo/config.h" LIBPLACEBO_CONFIG)
if(NOT LIBPLACEBO_CONFIG MATCHES "#define PL_HAVE_VULKAN 1")
    message(FATAL_ERROR "libplacebo was built without Vulkan support")
endif()
if(NOT LIBPLACEBO_CONFIG MATCHES "#define PL_HAVE_GLSLANG 1")
    message(FATAL_ERROR "libplacebo was built without glslang support")
endif()
if(NOT LIBPLACEBO_CONFIG MATCHES "#define PL_HAVE_OPENGL 1")
    message(FATAL_ERROR "libplacebo was built without OpenGL/OpenGL ES support")
endif()
if(NOT LIBPLACEBO_CONFIG MATCHES "#define PL_HAVE_DOVI 1")
    message(FATAL_ERROR "libplacebo was built without Dolby Vision reshaping support")
endif()
if(TRIPLET MATCHES "windows"
   AND NOT LIBPLACEBO_CONFIG MATCHES "#define PL_HAVE_D3D11 1")
    message(FATAL_ERROR "Windows libplacebo was built without D3D11 support")
endif()
if(TRIPLET MATCHES "windows"
   AND NOT EXISTS "${PREFIX}/include/spirv_cross/spirv_cross_c.h")
    message(FATAL_ERROR "Windows libplacebo is missing its SPIRV-Cross dependency")
endif()
if(LIBPLACEBO_CONFIG MATCHES "#define PL_HAVE_LIBDOVI 1")
    message(FATAL_ERROR "libdovi must remain disabled; FFmpeg supplies parsed RPU metadata")
endif()

set(VCPKG_STATUS "${INSTALL_ROOT}/vcpkg/status")
if(NOT EXISTS "${VCPKG_STATUS}")
    message(FATAL_ERROR "vcpkg status database is missing: ${VCPKG_STATUS}")
endif()
file(READ "${VCPKG_STATUS}" VCPKG_STATUS_CONTENT)
if(EXISTS "${PREFIX}/include/wolfssl")
    message(FATAL_ERROR "wolfSSL must not be present; QtAVCore uses OpenSSL")
endif()
if(EXISTS "${PREFIX}/include/vvenc/vvenc.h")
    message(FATAL_ERROR "VVenC must not be present; only native VVC decoding is required")
endif()
foreach(FFMPEG_FEATURE IN ITEMS
    ass
    dav1d
    gpl
    libplacebo
    libsmb2
    openssl
    qtav-player
    version3
    vulkan
)
    if(NOT VCPKG_STATUS_CONTENT MATCHES
       "Package: ffmpeg\nFeature: ${FFMPEG_FEATURE}\n")
        message(FATAL_ERROR "Required FFmpeg feature is missing: ${FFMPEG_FEATURE}")
    endif()
endforeach()

file(READ "${PREFIX}/share/ffmpeg/FindFFMPEG.cmake" FFMPEG_CMAKE_MODULE)
if(FFMPEG_CMAKE_MODULE MATCHES "vcpkg/(packages|buildtrees|downloads)")
    message(FATAL_ERROR "FFmpeg CMake metadata contains a non-relocatable build path")
endif()

message(STATUS "Verified QtAVCore FFmpeg dependency package: ${PREFIX}")
