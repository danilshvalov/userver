cmake_minimum_required(VERSION 3.14)
cmake_policy(SET CMP0025 NEW)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.15")
  cmake_policy(SET CMP0093 NEW)
endif()
set(CMAKE_POLICY_DEFAULT_CMP0028 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0048 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

project(userver)

set(USERVER_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}")

option(
    USERVER_INSTALL
    "Prepare build of userver to install in system"
    OFF
)

set(USERVER_AVAILABLE_COMPONENTS universal)
set(USERVER_NOT_INCLUDED_AS_SUBDIR OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR AND NOT USERVER_INSTALL)
  set(USERVER_NOT_INCLUDED_AS_SUBDIR ON)
endif()

option(USERVER_FEATURE_CORE "Provide a core library with coroutines, otherwise build only userver-universal" ON)
option(USERVER_FEATURE_CHAOTIC "Provide chaotic-codegen for jsonschema" ON)

set(USERVER_BUILD_PLATFORM_X86 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^x86")
  set(USERVER_BUILD_PLATFORM_X86 ${USERVER_FEATURE_CORE})
endif()

function(_require_userver_core FEATURE)
  if (NOT USERVER_FEATURE_CORE)
    message(FATAL_ERROR "'${FEATURE}' requires 'USERVER_FEATURE_CORE=ON'")
  endif()
endfunction()

option(USERVER_FEATURE_UTEST "Provide 'utest' and 'ubench' for gtest and gbenchmark integration" ON)
if (USERVER_FEATURE_UTEST)
  message(STATUS "Building utest with gtest and ubench with gbench")
endif()

option(
    USERVER_IS_THE_ROOT_PROJECT
    "Contributor mode: build userver tests, samples and helper tools"
    "${USERVER_NOT_INCLUDED_AS_SUBDIR}"
)
if (USERVER_IS_THE_ROOT_PROJECT)
  message(STATUS "Building userver as a primary project")
  if (NOT USERVER_FEATURE_UTEST)
    message(FATAL_ERROR "Cannot build tests without utest")
  endif()
else()
  message(STATUS "Building userver as a subproject")
endif()

set(USERVER_LIB_ENABLED_DEFAULT OFF)
if(USERVER_FEATURE_CORE AND USERVER_IS_THE_ROOT_PROJECT)
  set(USERVER_LIB_ENABLED_DEFAULT ON)
endif()

set(USERVER_MONGODB_DEFAULT OFF)
set(USERVER_CLICKHOUSE_DEFAULT OFF)
if(USERVER_FEATURE_CORE AND USERVER_IS_THE_ROOT_PROJECT AND USERVER_BUILD_PLATFORM_X86)
  if(NOT CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(USERVER_MONGODB_DEFAULT ON)
  endif()
  set(USERVER_CLICKHOUSE_DEFAULT ON)
endif()

set(USERVER_YDB_DEFAULT OFF)
if(USERVER_FEATURE_CORE AND USERVER_IS_THE_ROOT_PROJECT AND
    DEFINED CMAKE_CXX_STANDARD AND CMAKE_CXX_STANDARD GREATER_EQUAL 20)
  set(USERVER_YDB_DEFAULT ON)
endif()

option(USERVER_CONAN "Build with Conan packages" ${CONAN_EXPORTED})

option(
    USERVER_DOWNLOAD_PACKAGES
    "Download missing third party packages and use the downloaded versions"
    ON
)
option(
    USERVER_FORCE_DOWNLOAD_PACKAGES
    "Download all possible third party packages even if a system package is available"
    OFF
)

option(USERVER_FEATURE_CRYPTOPP_BLAKE2 "Provide wrappers for blake2 algorithms of crypto++" ON)
if (NOT USERVER_FEATURE_CRYPTOPP_BLAKE2)
  add_compile_definitions("USERVER_NO_CRYPTOPP_BLAKE2=1")
endif()

option(USERVER_FEATURE_CRYPTOPP_BASE64_URL "Provide wrappers for Base64 URL decoding and encoding algorithms of crypto++" ON)
if (NOT USERVER_FEATURE_CRYPTOPP_BASE64_URL)
  add_compile_definitions("USERVER_NO_CRYPTOPP_BASE64_URL=1")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "BSD")
  set(JEMALLOC_DEFAULT OFF)
else()
  set(JEMALLOC_DEFAULT ON)
endif()
option(USERVER_FEATURE_JEMALLOC "Enable linkage with jemalloc memory allocator" ${JEMALLOC_DEFAULT})

option(USERVER_DISABLE_PHDR_CACHE "Disable caching of dl_phdr_info items, which interferes with dlopen" OFF)

set(USERVER_DISABLE_RSEQ_DEFAULT ON)
if (USERVER_BUILD_PLATFORM_X86 AND CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(USERVER_DISABLE_RSEQ_DEFAULT OFF)
  message(STATUS "rseq-based acceleration is enabled by default")
endif()
option(USERVER_DISABLE_RSEQ_ACCELERATION "Disable rseq-based optimizations" ${USERVER_DISABLE_RSEQ_DEFAULT})

option(USERVER_CHECK_PACKAGE_VERSIONS "Check package versions" ON)

option(USERVER_FEATURE_MONGODB "Provide asynchronous driver for MongoDB" "${USERVER_MONGODB_DEFAULT}")
option(USERVER_FEATURE_POSTGRESQL "Provide asynchronous driver for PostgreSQL" "${USERVER_LIB_ENABLED_DEFAULT}")
option(USERVER_FEATURE_REDIS "Provide asynchronous driver for Redis" "${USERVER_LIB_ENABLED_DEFAULT}")
option(USERVER_FEATURE_GRPC "Provide asynchronous driver for gRPC" "${USERVER_LIB_ENABLED_DEFAULT}")
option(USERVER_FEATURE_CLICKHOUSE "Provide asynchronous driver for ClickHouse" "${USERVER_CLICKHOUSE_DEFAULT}")
option(USERVER_FEATURE_KAFKA "Provide asynchronous driver for Apache Kafka" "${USERVER_LIB_ENABLED_DEFAULT}")
option(USERVER_FEATURE_RABBITMQ "Provide asynchronous driver for RabbitMQ" "${USERVER_LIB_ENABLED_DEFAULT}")
option(USERVER_FEATURE_MYSQL "Provide asynchronous driver for MariaDB/MySQL" "${USERVER_LIB_ENABLED_DEFAULT}")
option(USERVER_FEATURE_ROCKS "Provide asynchronous driver for Rocks" "${USERVER_LIB_ENABLED_DEFAULT}")
option(USERVER_FEATURE_YDB "Provide asynchronous driver for YDB" "${USERVER_YDB_DEFAULT}")

set(CMAKE_DEBUG_POSTFIX d)

include(cmake/UserverSetupEnvironment.cmake)
userver_setup_environment()

include(PrepareInstall)

if(USERVER_INSTALL)
  include(GNUInstallDirs)
endif()

include(ModuleHelpers)
include(GetUserverVersion)
include(AddGoogleTests)
include(FindPackageRequired)
include(IncludeWhatYouUse)
include(UserverTestsuite)
include(CheckCompileFlags)
include(CMakePackageConfigHelpers)

set(USERVER_THIRD_PARTY_DIRS ${USERVER_ROOT_DIR}/third_party CACHE INTERNAL "")

init_debian_depends()

include(SetupGTest)

option(USERVER_FEATURE_OTLP "Provide asynchronous OTLP exporters" "${USERVER_LIB_ENABLED_DEFAULT}")

if (USERVER_FEATURE_GRPC)
  include(SetupProtobuf)
endif()

if (USERVER_IS_THE_ROOT_PROJECT)
  include(testsuite/SetupUserverTestsuiteEnv.cmake)
  add_subdirectory(testsuite)
endif()

add_subdirectory(universal)

if (USERVER_FEATURE_CORE)
  add_subdirectory(core)
  list(APPEND USERVER_AVAILABLE_COMPONENTS core)
endif()

if (USERVER_FEATURE_CHAOTIC)
  add_subdirectory(chaotic)
  list(APPEND USERVER_AVAILABLE_COMPONENTS chaotic)
endif()

if (USERVER_IS_THE_ROOT_PROJECT AND USERVER_FEATURE_CORE)
  add_subdirectory(tools/engine)
  add_subdirectory(tools/httpclient)
  add_subdirectory(tools/netcat)
  add_subdirectory(tools/dns_resolver)
  add_subdirectory(tools/congestion_control_emulator)
endif()

if (USERVER_FEATURE_MONGODB)
  _require_userver_core("USERVER_FEATURE_MONGODB")
  add_subdirectory(mongo)
  list(APPEND USERVER_AVAILABLE_COMPONENTS mongo)
endif()

if (USERVER_FEATURE_POSTGRESQL)
  _require_userver_core("USERVER_FEATURE_POSTGRESQL")
  add_subdirectory(postgresql)
  list(APPEND USERVER_AVAILABLE_COMPONENTS postgres)
endif()

if (USERVER_FEATURE_REDIS)
  _require_userver_core("USERVER_FEATURE_REDIS")
  add_subdirectory(redis)
  list(APPEND USERVER_AVAILABLE_COMPONENTS redis)
endif()

if (USERVER_FEATURE_GRPC)
  _require_userver_core("USERVER_FEATURE_GRPC")
  add_subdirectory(grpc)
  list(APPEND USERVER_AVAILABLE_COMPONENTS grpc)
endif()

if (USERVER_FEATURE_OTLP)
  if (NOT USERVER_FEATURE_GRPC)
    message(FATAL_ERROR "'USERVER_FEATURE_OTLP' requires 'USERVER_FEATURE_GRPC=ON'")
  endif()
  _require_userver_core("USERVER_FEATURE_OTLP")
  add_subdirectory(otlp)
  list(APPEND USERVER_AVAILABLE_COMPONENTS otlp)
endif()

if (USERVER_FEATURE_CLICKHOUSE)
  _require_userver_core("USERVER_FEATURE_CLICKHOUSE")
  add_subdirectory(clickhouse)
  list(APPEND USERVER_AVAILABLE_COMPONENTS clickhouse)
endif()

if (USERVER_FEATURE_KAFKA)
  _require_userver_core("USERVER_FEATURE_KAFKA")
  add_subdirectory(kafka)
  list(APPEND USERVER_AVAILABLE_COMPONENTS kafka)
endif()

if (USERVER_FEATURE_RABBITMQ)
  _require_userver_core("USERVER_FEATURE_RABBITMQ")
  add_subdirectory(rabbitmq)
  list(APPEND USERVER_AVAILABLE_COMPONENTS rabbitmq)
endif()

if (USERVER_FEATURE_MYSQL)
  _require_userver_core("USERVER_FEATURE_MYSQL")
  add_subdirectory(mysql)
  list(APPEND USERVER_AVAILABLE_COMPONENTS mysql)
endif()

if (USERVER_FEATURE_ROCKS)
  _require_userver_core("USERVER_FEATURE_ROCKS")
  add_subdirectory(rocks)
endif()

if (USERVER_FEATURE_YDB)
  _require_userver_core("USERVER_FEATURE_YDB")
  add_subdirectory(ydb)
  list(APPEND USERVER_AVAILABLE_COMPONENTS ydb)
endif()

_userver_export_targets()
_userver_make_install_config()

if (USERVER_IS_THE_ROOT_PROJECT AND USERVER_FEATURE_CORE)
  add_subdirectory(samples)
endif()

if(USERVER_INSTALL)
  include(cmake/UserverPack.cmake)
endif()
