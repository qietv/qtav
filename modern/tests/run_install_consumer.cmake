foreach(required_variable IN ITEMS
        QTAV_CORE_CONSUMER_SOURCE_DIR
        QTAV_CORE_CONSUMER_BINARY_DIR
        QTAV_CORE_INSTALL_PREFIX
        QTAV_CORE_INSTALL_LIBDIR
        QTAV_CORE_CONSUMER_CONFIG
        QTAV_CORE_CONSUMER_GENERATOR)
    if(NOT DEFINED "${required_variable}"
       OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${QTAV_CORE_CONSUMER_BINARY_DIR}")

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${QTAV_CORE_CONSUMER_SOURCE_DIR}"
    -B "${QTAV_CORE_CONSUMER_BINARY_DIR}"
    -G "${QTAV_CORE_CONSUMER_GENERATOR}"
    "-DQtAVCore_DIR=${QTAV_CORE_INSTALL_PREFIX}/${QTAV_CORE_INSTALL_LIBDIR}/cmake/QtAVCore"
    "-DCMAKE_BUILD_TYPE=${QTAV_CORE_CONSUMER_CONFIG}"
)
if(DEFINED QTAV_CORE_CONSUMER_PLATFORM
   AND NOT QTAV_CORE_CONSUMER_PLATFORM STREQUAL "")
    list(APPEND configure_command
        -A "${QTAV_CORE_CONSUMER_PLATFORM}")
endif()
if(DEFINED QTAV_CORE_CONSUMER_TOOLSET
   AND NOT QTAV_CORE_CONSUMER_TOOLSET STREQUAL "")
    list(APPEND configure_command
        -T "${QTAV_CORE_CONSUMER_TOOLSET}")
endif()
if(DEFINED QTAV_CORE_CONSUMER_TOOLCHAIN
   AND NOT QTAV_CORE_CONSUMER_TOOLCHAIN STREQUAL "")
    list(APPEND configure_command
        "-DCMAKE_TOOLCHAIN_FILE=${QTAV_CORE_CONSUMER_TOOLCHAIN}")
endif()
if(DEFINED QTAV_CORE_CONSUMER_VCPKG_TRIPLET
   AND NOT QTAV_CORE_CONSUMER_VCPKG_TRIPLET STREQUAL "")
    list(APPEND configure_command
        "-DVCPKG_TARGET_TRIPLET=${QTAV_CORE_CONSUMER_VCPKG_TRIPLET}")
endif()
if(DEFINED QTAV_CORE_CONSUMER_VCPKG_INSTALLED_DIR
   AND NOT QTAV_CORE_CONSUMER_VCPKG_INSTALLED_DIR STREQUAL "")
    list(APPEND configure_command
        "-DVCPKG_INSTALLED_DIR=${QTAV_CORE_CONSUMER_VCPKG_INSTALLED_DIR}"
        -DVCPKG_MANIFEST_MODE=OFF
    )
endif()
if(DEFINED QTAV_CORE_CONSUMER_PKG_CONFIG
   AND NOT QTAV_CORE_CONSUMER_PKG_CONFIG STREQUAL "")
    list(APPEND configure_command
        "-DPKG_CONFIG_EXECUTABLE=${QTAV_CORE_CONSUMER_PKG_CONFIG}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed-package consumer configuration failed:\n"
        "${configure_output}\n${configure_error}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        --build "${QTAV_CORE_CONSUMER_BINARY_DIR}"
        --config "${QTAV_CORE_CONSUMER_CONFIG}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed-package consumer build failed:\n"
        "${build_output}\n${build_error}")
endif()
