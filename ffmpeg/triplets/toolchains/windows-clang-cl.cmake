if(NOT _QTAV_WINDOWS_CLANG_CL_TOOLCHAIN)
    set(_QTAV_WINDOWS_CLANG_CL_TOOLCHAIN ON)

    find_program(QTAV_CLANG_CL
        NAMES clang-cl.exe
        HINTS
            "${VCPKG_VISUAL_STUDIO_PATH}/VC/Tools/Llvm/x64/bin"
            "${VCPKG_VISUAL_STUDIO_PATH}/VC/Tools/Llvm/bin"
            "$ENV{VCToolsInstallDir}/../../Llvm/x64/bin"
            "$ENV{VCToolsInstallDir}/../../Llvm/bin"
            "$ENV{ProgramFiles}/LLVM/bin")

    if(NOT QTAV_CLANG_CL)
        message(FATAL_ERROR
            "clang-cl.exe was not found. Install the Visual Studio component "
            "'C++ Clang tools for Windows' and rerun the build.")
    endif()

    set(CMAKE_C_COMPILER "${QTAV_CLANG_CL}" CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER "${QTAV_CLANG_CL}" CACHE FILEPATH "" FORCE)

    include("${CMAKE_CURRENT_LIST_DIR}/../../vcpkg/scripts/toolchains/windows.cmake")
endif()
