# SPDX-License-Identifier: LGPL-2.1-or-later

include_guard(GLOBAL)

# Android's clang++ driver and ANDROID_STL setting own the C++ runtime
# selection. pkg-config dependency closures can nevertheless expose libc++
# runtime entries (for example through libplacebo or harfbuzz). With a
# Windows-hosted NDK, FindPkgConfig may resolve those bare names to host
# archives under prebuilt/windows-x86_64/lib, which cannot be linked into an
# Android target. Remove only C++ runtime entries; clang++ adds the selected
# target runtime at the final link step.
function(qtav_sanitize_android_pkg_config_target target_name)
    if(NOT ANDROID OR NOT TARGET "${target_name}")
        return()
    endif()

    get_target_property(
        _qtav_pc_links
        "${target_name}"
        INTERFACE_LINK_LIBRARIES
    )
    if(NOT _qtav_pc_links)
        return()
    endif()

    set(_qtav_pc_filtered_links "")
    foreach(_qtav_pc_link IN LISTS _qtav_pc_links)
        file(TO_CMAKE_PATH "${_qtav_pc_link}" _qtav_pc_link_normalized)
        get_filename_component(
            _qtav_pc_link_name
            "${_qtav_pc_link_normalized}"
            NAME
        )
        if(_qtav_pc_link STREQUAL "c++"
           OR _qtav_pc_link STREQUAL "c++_static"
           OR _qtav_pc_link STREQUAL "c++abi"
           OR _qtav_pc_link STREQUAL "unwind"
           OR _qtav_pc_link STREQUAL "-lc++"
           OR _qtav_pc_link STREQUAL "-lc++_static"
           OR _qtav_pc_link STREQUAL "-lc++abi"
           OR _qtav_pc_link STREQUAL "-lunwind"
           OR _qtav_pc_link_name STREQUAL "libc++.a"
           OR _qtav_pc_link_name STREQUAL "libc++_static.a"
           OR _qtav_pc_link_name STREQUAL "libc++abi.a"
           OR _qtav_pc_link_name STREQUAL "libunwind.a")
            continue()
        endif()
        list(APPEND _qtav_pc_filtered_links "${_qtav_pc_link}")
    endforeach()

    set_property(
        TARGET "${target_name}"
        PROPERTY INTERFACE_LINK_LIBRARIES "${_qtav_pc_filtered_links}"
    )
endfunction()
