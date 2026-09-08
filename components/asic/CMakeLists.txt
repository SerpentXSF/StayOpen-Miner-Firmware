# lt0051.c is the only thing in this project that calls into components/a,
# a prebuilt liba.a we have no source for. It is reached solely from the
# LOTTO and DC dispatch arms in asic.c, which no BC board takes -- but the
# linker does not care whether a call is reachable, only whether it is
# referenced, so compiling the file put 82 sections of that archive at live
# flash addresses in every published BC01 and BC04 image. A GPL release that
# conveys object code with no corresponding source is exactly the defect this
# project raises against the vendor.
#
# Leave STAYOPEN_ASIC_LT0051 off for a blob-free build. Turn it on only for
# an LT0051 board, and know that the resulting binary is not distributable
# under the GPL.
set(asic_srcs
    "bm1370.c"
    "serial.c"
    "crc.c"
    "common.c"
    "asic.c"
    "frequency_transition_bmXX.c"
    "pll.c")

# Component "a" stays required either way: common.c includes
# asic_abstration.h from it, which is a header of #defines and no code. An
# archive contributes nothing to an image until something references a symbol
# in it, so with lt0051.c out of the build no member is extracted -- verified
# against the link map, not assumed.
set(asic_reqs
    "freertos"
    "driver"
    "stratum"
    "a")

if(CONFIG_STAYOPEN_ASIC_LT0051)
    list(APPEND asic_srcs "lt0051.c")
endif()

idf_component_register(
SRCS ${asic_srcs}

INCLUDE_DIRS 
    "include"

REQUIRES ${asic_reqs}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -fvisibility=hidden
)

# Include the header files from "main" directory
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../main")

# Include the header files from "main/tasks" directory
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../main/tasks")