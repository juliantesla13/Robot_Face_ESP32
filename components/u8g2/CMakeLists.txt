file(GLOB_RECURSE SOURCES "csrc/*.c")
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "csrc"
)