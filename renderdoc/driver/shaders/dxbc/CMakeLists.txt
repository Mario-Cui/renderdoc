set(sources
    dx_debug.cpp
    dx_debug.h
    dxbc_bytecode.cpp
    dxbc_bytecode.h
    dxbc_bytecode_editor.cpp
    dxbc_bytecode_editor.h
    dxbc_bytecode_vendorext.cpp
    dxbc_bytecode_ops.cpp
    dxbc_bytecode_ops.h
    dxbc_compile.cpp
    dxbc_compile.h
    dxbc_debug.cpp
    dxbc_debug.h
    dxbc_container.cpp
    dxbc_container.h
    dxbc_reflect.cpp
    dxbc_reflect.h
    dxbc_sdbg.cpp
    dxbc_sdbg.h
    dxbc_spdb.cpp
    dxbc_spdb.h
    dxbc_stringise.cpp
    dxbc_common.h
    precompiled.cpp
    precompiled.h
    official/cvconst.h
    official/cvinfo.h)

add_library(rdoc_dxbc OBJECT ${sources})
target_compile_definitions(rdoc_dxbc ${RDOC_DEFINITIONS})
target_include_directories(rdoc_dxbc ${RDOC_INCLUDES})

if(WIN32)
    target_compile_options(rdoc_dxbc PRIVATE /FIprecompiled.h)
endif()
