if(NOT _QTAV_WINDOWS_CLANG_CL_TOOLCHAIN)
    set(_QTAV_WINDOWS_CLANG_CL_TOOLCHAIN ON)

    set(_QTAV_CLANG_CL_HINTS
        "${VCPKG_VISUAL_STUDIO_PATH}/VC/Tools/Llvm/x64/bin"
        "${VCPKG_VISUAL_STUDIO_PATH}/VC/Tools/Llvm/bin"
        "$ENV{VSINSTALLDIR}/VC/Tools/Llvm/x64/bin"
        "$ENV{VSINSTALLDIR}/VC/Tools/Llvm/bin"
        "$ENV{VCToolsInstallDir}/../../Llvm/x64/bin"
        "$ENV{VCToolsInstallDir}/../../Llvm/bin"
        "$ENV{ProgramFiles}/LLVM/bin"
    )

    # vcpkg loads a chainloaded toolchain before all of its internal Visual
    # Studio variables are guaranteed to be available. Enumerate installed VS
    # editions as a fallback so service accounts do not need clang-cl on PATH.
    if(DEFINED ENV{ProgramFiles} AND NOT "$ENV{ProgramFiles}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{ProgramFiles}" _QTAV_PROGRAM_FILES)
        file(GLOB _QTAV_VISUAL_STUDIO_INSTALLATIONS LIST_DIRECTORIES true
            "${_QTAV_PROGRAM_FILES}/Microsoft Visual Studio/*/*"
        )
        foreach(_QTAV_VISUAL_STUDIO_PATH IN LISTS _QTAV_VISUAL_STUDIO_INSTALLATIONS)
            list(APPEND _QTAV_CLANG_CL_HINTS
                "${_QTAV_VISUAL_STUDIO_PATH}/VC/Tools/Llvm/x64/bin"
                "${_QTAV_VISUAL_STUDIO_PATH}/VC/Tools/Llvm/bin"
            )
        endforeach()
    endif()

    find_program(QTAV_CLANG_CL
        NAMES clang-cl.exe
        HINTS ${_QTAV_CLANG_CL_HINTS}
    )

    if(NOT QTAV_CLANG_CL)
        message(FATAL_ERROR
            "clang-cl.exe was not found. Searched PATH, Visual Studio installs "
            "under '$ENV{ProgramFiles}', VCPKG_VISUAL_STUDIO_PATH "
            "'${VCPKG_VISUAL_STUDIO_PATH}', and VSINSTALLDIR "
            "'$ENV{VSINSTALLDIR}'.")
    endif()

    set(CMAKE_C_COMPILER "${QTAV_CLANG_CL}" CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER "${QTAV_CLANG_CL}" CACHE FILEPATH "" FORCE)

    include("${CMAKE_CURRENT_LIST_DIR}/../../vcpkg/scripts/toolchains/windows.cmake")

    # CMake's cmcldeps passes resource-compiler flags through clang-cl while
    # scanning .rc dependencies. clang-cl treats rc.exe's /c65001 option as a
    # file name, so retain the Windows define without that RC-only codepage
    # switch. C and C++ sources still receive vcpkg's /utf-8 option.
    set(CMAKE_RC_FLAGS "/DWIN32" CACHE STRING "" FORCE)
endif()
