# Shared operating-system helpers live below platform-specific subdirectories.
# The core never links a platform helper implicitly.
if(ANDROID)
    add_subdirectory(android)
elseif(WIN32)
    add_subdirectory(windows)
elseif(CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$")
    add_subdirectory(ohos)
endif()
