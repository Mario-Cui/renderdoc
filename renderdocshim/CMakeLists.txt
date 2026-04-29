set(sources
    renderdocshim.cpp
    renderdocshim.h)

# On Windows, this shim DLL deliberately references only kernel32.dll and user32.dll
# (no CRT) for minimal overhead. This is handled in the source with /MT and
# no-exception settings, but we keep the CMake simple.
add_library(renderdocshim SHARED ${sources})

if(WIN32)
    # The output name is platform-specific in the sln (renderdocshim32/renderdocshim64)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set_target_properties(renderdocshim PROPERTIES OUTPUT_NAME "renderdocshim64")
    else()
        set_target_properties(renderdocshim PROPERTIES OUTPUT_NAME "renderdocshim32")
    endif()

    target_link_libraries(renderdocshim PRIVATE kernel32.lib user32.lib)

    if(MSVC)
        target_compile_options(renderdocshim PRIVATE /wd4100 /wd4189 /wd4505 /wd4702)
    endif()
endif()
