# Shared operating-system helpers live below platform-specific subdirectories.
# The core never links a platform helper implicitly.
if(ANDROID)
    add_subdirectory(android)
elseif(WIN32)
    add_subdirectory(windows)
elseif(APPLE)
    add_subdirectory(apple)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(linux)
endif()
