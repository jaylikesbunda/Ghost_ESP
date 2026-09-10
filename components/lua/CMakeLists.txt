file(GLOB LUA_SOURCES "src/*.c")
list(REMOVE_ITEM LUA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ldblib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/liolib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/linit.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/loadlib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/loslib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lutf8lib.c"
)

idf_component_register(SRCS ${LUA_SOURCES}
                       INCLUDE_DIRS "src")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LUA_COMPAT_5_3
    LUA_NO_RESERVED_WORDS
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-maybe-uninitialized
)
