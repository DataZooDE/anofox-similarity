cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME anofox_similarity)

# Extension version
set(EXT_VERSION_ANOFOX_SIMILARITY "0.1.0")
add_compile_definitions(EXT_VERSION_ANOFOX_SIMILARITY="${EXT_VERSION_ANOFOX_SIMILARITY}")

project(${TARGET_NAME})

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/anofox_similarity_extension.cpp
    src/core/constants.cpp
    src/core/error_handling.cpp
    src/modules/jaccard_similarity.cpp
    src/modules/similarity_search.cpp
    src/modules/wl_kernel.cpp
    src/modules/predecessor_inference.cpp
    src/modules/sap_transformations.cpp
    src/modules/vss_integration.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
