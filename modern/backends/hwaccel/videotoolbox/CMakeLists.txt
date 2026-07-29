if(NOT APPLE)
    return()
endif()

find_library(QTAV_VIDEOTOOLBOX_FRAMEWORK VideoToolbox)
find_library(QTAV_COREVIDEO_FRAMEWORK CoreVideo)
if(NOT QTAV_VIDEOTOOLBOX_FRAMEWORK OR NOT QTAV_COREVIDEO_FRAMEWORK)
    return()
endif()

add_library(qtav_hw_videotoolbox
    src/videotoolbox_hardware_decoder.cpp
)
add_library(QtAV::HWVideoToolbox ALIAS qtav_hw_videotoolbox)

target_compile_features(qtav_hw_videotoolbox PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_hw_videotoolbox
    PRIVATE QTAV_HW_VIDEOTOOLBOX_BUILDING_LIBRARY
)
target_include_directories(qtav_hw_videotoolbox
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_hw_videotoolbox
    PUBLIC
        QtAV::Core
        "${QTAV_COREVIDEO_FRAMEWORK}"
        "${QTAV_VIDEOTOOLBOX_FRAMEWORK}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_hw_videotoolbox
        PUBLIC QTAV_HW_VIDEOTOOLBOX_STATIC
    )
endif()

set_target_properties(qtav_hw_videotoolbox PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME HWVideoToolbox
)

target_compile_options(
    qtav_hw_videotoolbox
    PRIVATE -Wall -Wextra -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_hw_videotoolbox
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
