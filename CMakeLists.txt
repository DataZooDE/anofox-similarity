cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME anofox_similarity)

project(${TARGET_NAME})

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

include_directories(src/include)

# Optional: Find OpenVINO for textual embeddings support
find_package(OpenVINO QUIET)
if(OpenVINO_FOUND)
    add_compile_definitions(DUCKDB_OPENVINO_AVAILABLE=1)
endif()

set(EXTENSION_SOURCES
    src/anofox_similarity_extension.cpp
    src/core/constants.cpp
    src/core/error_handling.cpp
    src/modules/jaccard_similarity.cpp
    src/modules/similarity_search.cpp
    src/modules/wl_kernel.cpp
    src/modules/predecessor_inference.cpp
    src/modules/sap_transformations.cpp
    src/modules/dynamics365_transformations.cpp
    src/modules/vss_integration.cpp
    src/modules/textual_embeddings.cpp
    src/modules/multimodal_fusion.cpp
    src/modules/transactional_embeddings.cpp
    src/modules/universal_schema.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenVINO if available
if(OpenVINO_FOUND)
    target_link_libraries(${EXTENSION_NAME} PRIVATE openvino::runtime)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} PRIVATE openvino::runtime)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
