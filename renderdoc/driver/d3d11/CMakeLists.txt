set(sources
    d3d11_common.cpp
    d3d11_common.h
    d3d11_counters.cpp
    d3d11_debug.cpp
    d3d11_debug.h
    d3d11_context_wrap.cpp
    d3d11_context.cpp
    d3d11_context.h
    d3d11_context1_wrap.cpp
    d3d11_context2_wrap.cpp
    d3d11_context3_wrap.cpp
    d3d11_device_wrap.cpp
    d3d11_device.cpp
    d3d11_device.h
    d3d11_device1_wrap.cpp
    d3d11_device2_wrap.cpp
    d3d11_device3_wrap.cpp
    d3d11_hooks.cpp
    d3d11_hooks.h
    d3d11_initstate.cpp
    d3d11_manager.cpp
    d3d11_manager.h
    d3d11_msaa_array_conv.cpp
    d3d11_outputwindow.cpp
    d3d11_overlay.cpp
    d3d11_pixelhistory.cpp
    d3d11_postvs.cpp
    d3d11_rendermesh.cpp
    d3d11_renderstate.cpp
    d3d11_renderstate.h
    d3d11_rendertext.cpp
    d3d11_rendertext.h
    d3d11_rendertexture.cpp
    d3d11_replay.cpp
    d3d11_replay.h
    d3d11_resources.cpp
    d3d11_resources.h
    d3d11_serialise.cpp
    d3d11_shader_cache.cpp
    d3d11_shader_cache.h
    d3d11_shaderdebug.cpp
    d3d11_stringise.cpp
    d3d11_video.cpp
    d3d11_video.h
    d3d9_hooks.cpp
    precompiled.cpp
    precompiled.h)

add_library(rdoc_d3d11 OBJECT ${sources})
target_compile_definitions(rdoc_d3d11 ${RDOC_DEFINITIONS})
target_include_directories(rdoc_d3d11 ${RDOC_INCLUDES})

if(WIN32)
    target_compile_options(rdoc_d3d11 PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/precompiled.h")
endif()
