set(sources
    dxgi_common.cpp
    dxgi_common.h
    dxgi_hooks.cpp
    dxgi_stringise.cpp
    dxgi_wrapped.cpp
    dxgi_wrapped.h
    precompiled.cpp
    precompiled.h)

add_library(rdoc_dxgi OBJECT ${sources})
target_compile_definitions(rdoc_dxgi ${RDOC_DEFINITIONS})
target_include_directories(rdoc_dxgi ${RDOC_INCLUDES})

if(WIN32)
    target_compile_options(rdoc_dxgi PRIVATE /FIprecompiled.h)
endif()
