set(sources
    intel_counters.cpp
    intel_counters.h
    intel_gl_counters.cpp
    intel_gl_counters.h)

set(include_dirs ${RDOC_INCLUDES})

if(WIN32)
    list(APPEND sources
        official/DriverStorePath.h
        official/metrics_discovery_api.h)
endif()

add_library(rdoc_intel OBJECT ${sources})
target_compile_definitions(rdoc_intel ${RDOC_DEFINITIONS})
target_include_directories(rdoc_intel ${include_dirs})
