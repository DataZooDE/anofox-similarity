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

find_package(nlohmann_json CONFIG REQUIRED)

set(EXTENSION_SOURCES
    src/anofox_similarity_extension.cpp
    src/core/constants.cpp
    src/core/error_handling.cpp
    src/modules/jaccard_similarity.cpp
    src/modules/similarity_search.cpp
    src/modules/wl_kernel.cpp
    src/modules/wl_kernel_cpp.cpp
    src/modules/jaccard_cpp.cpp
    src/modules/predecessor_inference.cpp
    src/modules/sap_transformations.cpp
    src/modules/dynamics365_transformations.cpp
    src/modules/vss_integration.cpp
    src/modules/bom_utilities.cpp
    src/modules/statistics_functions.cpp
    src/modules/embedding_statistics.cpp
    src/modules/incremental_updates.cpp
    src/modules/textual_embeddings.cpp
    src/modules/multimodal_fusion.cpp
    src/modules/transactional_embeddings.cpp
    src/modules/universal_schema.cpp
    src/modules/duckpgq_integration.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# --- PostHog Telemetry ---
find_package(OpenSSL REQUIRED)

add_library(posthog_telemetry_obj OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/posthog-telemetry/src/telemetry.cpp
)

target_include_directories(posthog_telemetry_obj PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/posthog-telemetry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib
    ${OPENSSL_INCLUDE_DIR}
)

target_compile_features(posthog_telemetry_obj PUBLIC cxx_std_17)

# Add telemetry objects to extension targets
target_sources(${EXTENSION_NAME} PRIVATE $<TARGET_OBJECTS:posthog_telemetry_obj>)
target_sources(${LOADABLE_EXTENSION_NAME} PRIVATE $<TARGET_OBJECTS:posthog_telemetry_obj>)

target_include_directories(${EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/posthog-telemetry/include)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/posthog-telemetry/include)

# Link OpenSSL for telemetry
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

target_link_libraries(${EXTENSION_NAME} nlohmann_json::nlohmann_json)
target_link_libraries(${LOADABLE_EXTENSION_NAME} nlohmann_json::nlohmann_json)

# Link OpenVINO if available
if(OpenVINO_FOUND)
    target_link_libraries(${EXTENSION_NAME} openvino::runtime)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} openvino::runtime)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
