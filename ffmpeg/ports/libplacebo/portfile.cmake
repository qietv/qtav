vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://code.videolan.org/videolan/libplacebo.git
    REF 3188549fba13bbdf3a5a98de2a38c2e71f04e21e
    FETCH_REF v7.351.0
    HEAD_REF master
    PATCHES
        0001-add-glslang-libdir-option.patch
        0002-fix-windows-shlwapi-linkage.patch
        0003-use-static-spirv-cross.patch
        0004-align-windows-clang-allocations.patch
        0005-add-d3d11-pass-diagnostics.patch
)

x_vcpkg_get_python_packages(
    PYTHON_VERSION 3
    OUT_PYTHON_VAR PYTHON3
    PACKAGES glad2==2.0.8 jinja2 markupsafe
)
get_filename_component(PYTHON3_DIR "${PYTHON3}" DIRECTORY)
vcpkg_add_to_path(PREPEND "${PYTHON3_DIR}")

set(VULKAN_REGISTRY "${CURRENT_INSTALLED_DIR}/share/vulkan/registry/vk.xml")
if(NOT EXISTS "${VULKAN_REGISTRY}")
    message(FATAL_ERROR "Vulkan registry not found: ${VULKAN_REGISTRY}")
endif()

set(LIBPLACEBO_D3D11 disabled)
if(VCPKG_TARGET_IS_WINDOWS)
    set(LIBPLACEBO_D3D11 enabled)
endif()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Ddefault_library=static
        -Dprefer_static=true
        -Dglslang-libdir=${CURRENT_INSTALLED_DIR}/lib
        -Dvulkan=enabled
        -Dvk-proc-addr=disabled
        -Dvulkan-registry=${VULKAN_REGISTRY}
        -Dopengl=enabled
        -Dd3d11=${LIBPLACEBO_D3D11}
        -Dshaderc=disabled
        -Dglslang=enabled
        -Dlcms=disabled
        -Ddovi=enabled
        -Dlibdovi=disabled
        -Dxxhash=disabled
        -Dunwind=disabled
        -Ddemos=false
        -Dtests=false
        -Dbench=false
        -Dfuzz=false
)
vcpkg_install_meson()
vcpkg_fixup_pkgconfig()

# Meson serializes find_library() results as absolute archive paths. Express
# the glslang closure as normal linker names so FFmpeg's generated CMake module
# remains relocatable after CI artifacts are downloaded elsewhere.
set(LIBPLACEBO_PC "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libplacebo.pc")
file(READ "${LIBPLACEBO_PC}" LIBPLACEBO_PC_CONTENT)
foreach(GLSLANG_LIBRARY IN ITEMS
    glslang-default-resource-limits
    SPIRV
    glslang
    MachineIndependent
    OSDependent
    OGLCompiler
    GenericCodeGen
    SPIRV-Tools
    SPIRV-Tools-opt
)
    string(REGEX REPLACE
        "\\$\\{prefix\\}/lib/(lib)?${GLSLANG_LIBRARY}\\.(a|lib)"
        "-l${GLSLANG_LIBRARY}"
        LIBPLACEBO_PC_CONTENT
        "${LIBPLACEBO_PC_CONTENT}"
    )
endforeach()
if(VCPKG_TARGET_IS_WINDOWS)
    foreach(WINDOWS_LIBRARY IN ITEMS shlwapi version)
        string(REGEX REPLACE
            "(^|[ \t])${WINDOWS_LIBRARY}\\.lib"
            "\\1-l${WINDOWS_LIBRARY}"
            LIBPLACEBO_PC_CONTENT
            "${LIBPLACEBO_PC_CONTENT}"
        )
    endforeach()
    # The upstream SPIRV-Cross C pkg-config module exposes only its C wrapper
    # archive even though the static wrapper calls the backend and core C++
    # archives. Publish the complete ordered closure required by libplacebo's
    # D3D11 SPIR-V-to-HLSL compiler.
    string(REPLACE
        "Requires: spirv-cross-c >= 0.29.0"
        "Requires:"
        LIBPLACEBO_PC_CONTENT
        "${LIBPLACEBO_PC_CONTENT}"
    )
    string(REPLACE
        " -lversion\n"
        " -lversion -lspirv-cross-c -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-msl -lspirv-cross-cpp -lspirv-cross-reflect -lspirv-cross-util -lspirv-cross-core\n"
        LIBPLACEBO_PC_CONTENT
        "${LIBPLACEBO_PC_CONTENT}"
    )
endif()
file(WRITE "${LIBPLACEBO_PC}" "${LIBPLACEBO_PC_CONTENT}")

# FFmpeg's configure probes libplacebo with the C compiler. Static libplacebo
# contains C++ and glslang objects, so mobile pkg-config consumers must receive
# the complete static libc++ closure explicitly.
if(VCPKG_CMAKE_SYSTEM_NAME STREQUAL "Android" OR TARGET_TRIPLET MATCHES "ohos")
    file(APPEND
        "${LIBPLACEBO_PC}"
        "\nLibs.private: -lc++_static -lc++abi -lunwind\n"
    )
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
