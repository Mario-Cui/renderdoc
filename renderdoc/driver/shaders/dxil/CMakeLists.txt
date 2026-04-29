set(sources
    dxil_bytecode.cpp
    dxil_bytecode.h
    dxil_bytecode_editor.cpp
    dxil_bytecode_editor.h
    dxil_common.cpp
    dxil_common.h
    dxil_controlflow.cpp
    dxil_controlflow.h
    dxil_debug.cpp
    dxil_debug.h
    dxil_debuginfo.cpp
    dxil_debuginfo.h
    dxil_disassemble.cpp
    dxil_metadata.cpp
    dxil_metadata.h
    dxil_reflect.cpp
    dxil_stringise.cpp
    llvm_decoder.cpp
    llvm_decoder.h
    llvm_encoder.cpp
    llvm_encoder.h
    llvm_bitreader.h
    llvm_bitwriter.h
    llvm_common.h
    precompiled.cpp
    precompiled.h)

add_library(rdoc_dxil OBJECT ${sources})
target_compile_definitions(rdoc_dxil ${RDOC_DEFINITIONS})
target_include_directories(rdoc_dxil ${RDOC_INCLUDES})

if(WIN32)
    target_compile_options(rdoc_dxil PRIVATE /FIprecompiled.h)
endif()
