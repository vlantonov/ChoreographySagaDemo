cmake_minimum_required(VERSION 3.25)
project(inventory_service LANGUAGES CXX)

# --- toolchain ---------------------------------------------------------------
# Dependency manager: vcpkg manifest mode (tech-stack §13). Configure with
#   -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
# to resolve gRPC/protobuf, librdkafka, libpqxx, opentelemetry-cpp, nlohmann-json
# and GoogleTest from vcpkg.json.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(INVENTORY_BUILD_SERVER "Build the gRPC/Kafka/Postgres server binary" ON)
option(INVENTORY_BUILD_TESTS "Build the unit-test binary" ON)

set(PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../proto")

include(FetchContent)

# --- nlohmann_json (vcpkg; FetchContent fallback for a toolchain-free build) --
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json not found via find_package; using FetchContent")
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- core library (no gRPC/Kafka/Postgres/OTel): unit-testable --------------
add_library(inventory_core STATIC
  src/envelope.cpp
  src/outbox.cpp
  src/service.cpp
  src/obs.cpp)
target_include_directories(inventory_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(inventory_core PUBLIC nlohmann_json::nlohmann_json)

# --- unit tests --------------------------------------------------------------
if(INVENTORY_BUILD_TESTS)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "GTest not found via find_package; using FetchContent")
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
      URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(inventory_tests
    test/envelope_test.cpp
    test/relay_test.cpp
    test/service_test.cpp)
  target_include_directories(inventory_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(inventory_tests PRIVATE inventory_core GTest::gtest GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(inventory_tests)
endif()

# --- server binary (heavy deps via vcpkg) ------------------------------------
if(INVENTORY_BUILD_SERVER)
  find_package(Protobuf CONFIG REQUIRED)
  find_package(gRPC CONFIG REQUIRED)
  find_package(RdKafka CONFIG QUIET)         # vcpkg exports RdKafka::rdkafka++
  find_package(libpqxx CONFIG REQUIRED)
  find_package(opentelemetry-cpp CONFIG REQUIRED)

  # Generate protobuf + gRPC C++ stubs from the shared proto module.
  set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(MAKE_DIRECTORY ${GEN_DIR})
  add_library(inventory_proto OBJECT ${PROTO_ROOT}/inventory/v1/inventory.proto)
  target_include_directories(inventory_proto PUBLIC ${GEN_DIR})
  target_link_libraries(inventory_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
  protobuf_generate(
    TARGET inventory_proto
    IMPORT_DIRS ${PROTO_ROOT}
    PROTOC_OUT_DIR ${GEN_DIR})
  protobuf_generate(
    TARGET inventory_proto
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    IMPORT_DIRS ${PROTO_ROOT}
    PROTOC_OUT_DIR ${GEN_DIR})

  add_executable(inventory_server
    src/main.cpp
    src/config.cpp
    src/db.cpp
    src/kafka_io.cpp
    src/grpc_server.cpp
    src/obs_otel.cpp
    $<TARGET_OBJECTS:inventory_proto>)
  target_include_directories(inventory_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${GEN_DIR})
  target_link_libraries(inventory_server PRIVATE
    inventory_core
    protobuf::libprotobuf
    gRPC::grpc++
    libpqxx::pqxx
    opentelemetry-cpp::trace
    opentelemetry-cpp::metrics
    opentelemetry-cpp::otlp_grpc_exporter
    opentelemetry-cpp::otlp_grpc_metrics_exporter)
  # librdkafka C++ target name differs across vcpkg/apt; link whichever exists.
  if(TARGET RdKafka::rdkafka++)
    target_link_libraries(inventory_server PRIVATE RdKafka::rdkafka++)
  else()
    target_link_libraries(inventory_server PRIVATE rdkafka++)
  endif()
endif()
