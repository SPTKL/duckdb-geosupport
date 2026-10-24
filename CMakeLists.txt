cmake_minimum_required(VERSION 3.16...4.0)
project(geosupport LANGUAGES C CXX)

if(DEFINED EXTENSION_NAME AND NOT EXTENSION_NAME STREQUAL "geosupport")
    message(FATAL_ERROR "This project builds the geosupport extension")
endif()
foreach(part IN ITEMS MAJOR MINOR PATCH)
    # Validate template arguments so the header API and metadata cannot diverge.
    if(part STREQUAL "MAJOR")
        set(expected 1)
    elseif(part STREQUAL "MINOR")
        set(expected 5)
    else()
        set(expected 6)
    endif()
    if(DEFINED TARGET_DUCKDB_VERSION_${part} AND NOT TARGET_DUCKDB_VERSION_${part} EQUAL expected)
        message(FATAL_ERROR "Geosupport targets stable C API v1.5.6")
    endif()
endforeach()

if(WIN32 OR DUCKDB_WASM_EXTENSION OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Geosupport requires a native POSIX build; community distribution targets Linux x86-64")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "The NYC Geosupport engine requires Linux x86-64")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
if(NOT APPLE)
    find_package(OpenSSL REQUIRED)
endif()

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
add_executable(gs-build-tool scripts/native-build-tool.cpp)
target_include_directories(gs-build-tool PRIVATE vendor)
foreach(resource IN ITEMS resources catalog)
    if(resource STREQUAL "resources")
        set(input resources/layouts.json)
        set(symbol LAYOUTS_JSON)
    else()
        set(input catalog/releases.json)
        set(symbol CATALOG_JSON)
    endif()
    add_custom_command(OUTPUT "${GENERATED_DIR}/${resource}.hpp"
        COMMAND gs-build-tool json "${CMAKE_CURRENT_SOURCE_DIR}/${input}" "${GENERATED_DIR}/${resource}.hpp" ${symbol}
        DEPENDS gs-build-tool "${input}" VERBATIM)
endforeach()

add_executable(geosupport-worker src/common.cpp src/engine.cpp src/worker_main.cpp "${GENERATED_DIR}/resources.hpp")
target_compile_definitions(geosupport-worker PRIVATE GS_WORKER_ONLY)
target_include_directories(geosupport-worker PRIVATE src vendor "${GENERATED_DIR}")
target_link_libraries(geosupport-worker PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
# The embedded worker is a separate executable: it cannot borrow DuckDB's C++ runtime.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(geosupport-worker PRIVATE -static-libstdc++ -static-libgcc)
    # NYC's libgeo.so expects math symbols from its host without declaring a
    # libm dependency. Keep libm even when --as-needed sees no worker references.
    # Pass the soname directly to ld: GCC can move plain -lm into the static
    # libstdc++ link group, which would again drop the shared math symbols.
    target_link_libraries(geosupport-worker PRIVATE "-Wl,--no-as-needed,-l:libm.so.6,--as-needed")
endif()
add_custom_command(OUTPUT "${GENERATED_DIR}/worker.hpp"
    COMMAND gs-build-tool worker "$<TARGET_FILE:geosupport-worker>" "${GENERATED_DIR}/worker.hpp"
    DEPENDS gs-build-tool geosupport-worker VERBATIM)

add_library(gs-miniz STATIC vendor/miniz/miniz.c vendor/miniz/miniz_zip.c vendor/miniz/miniz_tinfl.c)
target_compile_definitions(gs-miniz PRIVATE MINIZ_NO_DEFLATE_APIS MINIZ_NO_ZLIB_APIS)
add_library(geosupport SHARED src/geosupport.cpp src/manager.cpp src/common.cpp src/store.cpp
    "${GENERATED_DIR}/catalog.hpp" "${GENERATED_DIR}/worker.hpp")
target_include_directories(geosupport PRIVATE src vendor vendor/duckdb "${GENERATED_DIR}")
target_compile_definitions(geosupport PRIVATE DUCKDB_EXTENSION_NAME=geosupport
    DUCKDB_EXTENSION_API_VERSION_MAJOR=1 DUCKDB_EXTENSION_API_VERSION_MINOR=5 DUCKDB_EXTENSION_API_VERSION_PATCH=6)
target_link_libraries(geosupport PRIVATE gs-miniz CURL::libcurl ${CMAKE_DL_LIBS} Threads::Threads)
if(NOT APPLE)
    target_link_libraries(geosupport PRIVATE OpenSSL::Crypto)
    target_link_options(geosupport PRIVATE -static-libstdc++ -static-libgcc "-Wl,--exclude-libs,ALL")
endif()
