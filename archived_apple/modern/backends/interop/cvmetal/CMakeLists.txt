if(NOT APPLE OR NOT TARGET qtav_render_metal)
    return()
endif()

enable_language(OBJCXX)

find_library(QTAV_CVMETAL_COREVIDEO_FRAMEWORK CoreVideo)
if(NOT QTAV_CVMETAL_COREVIDEO_FRAMEWORK)
    return()
endif()

add_library(qtav_interop_cvmetal
    src/cvmetal_frame_interop.mm
)
add_library(QtAV::InteropCVMetal ALIAS qtav_interop_cvmetal)

target_compile_features(qtav_interop_cvmetal PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_interop_cvmetal
    PRIVATE QTAV_INTEROP_CVMETAL_BUILDING_LIBRARY
)
target_compile_options(qtav_interop_cvmetal PRIVATE -fobjc-arc)
target_include_directories(qtav_interop_cvmetal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_interop_cvmetal
    PUBLIC QtAV::RenderMetal
    PRIVATE "${QTAV_CVMETAL_COREVIDEO_FRAMEWORK}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_interop_cvmetal
        PUBLIC QTAV_INTEROP_CVMETAL_STATIC
    )
endif()

set_target_properties(qtav_interop_cvmetal PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME InteropCVMetal
)

target_compile_options(
    qtav_interop_cvmetal
    PRIVATE -Wall -Wextra -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_interop_cvmetal
        EXPORT QtAVCoreTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        DIRECTORY include/
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
endif()
