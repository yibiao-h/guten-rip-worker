find_package(Protobuf CONFIG QUIET)
find_package(gRPC CONFIG QUIET)
find_package(Threads REQUIRED)

get_filename_component(GUTENRIP_WORKER_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(GUTENRIP_WORKER_HOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

if (Protobuf_FOUND AND gRPC_FOUND AND
    TARGET protobuf::protoc AND TARGET protobuf::libprotobuf AND
    TARGET gRPC::grpc++ AND TARGET gRPC::grpc_cpp_plugin)
    set(GUTENRIP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(GUTENRIP_WORKER_PROTO "${GUTENRIP_WORKER_ROOT}/proto/rip_worker.proto")
    get_filename_component(GUTENRIP_WORKER_PROTO_DIR "${GUTENRIP_WORKER_PROTO}" DIRECTORY)

    set(GUTENRIP_WORKER_PROTO_SRCS
        "${GUTENRIP_GENERATED_DIR}/rip_worker.pb.cc"
        "${GUTENRIP_GENERATED_DIR}/rip_worker.pb.h"
        "${GUTENRIP_GENERATED_DIR}/rip_worker.grpc.pb.cc"
        "${GUTENRIP_GENERATED_DIR}/rip_worker.grpc.pb.h"
    )

    add_custom_command(
        OUTPUT ${GUTENRIP_WORKER_PROTO_SRCS}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GUTENRIP_GENERATED_DIR}"
        COMMAND protobuf::protoc
        ARGS
            --cpp_out "${GUTENRIP_GENERATED_DIR}"
            --grpc_out "${GUTENRIP_GENERATED_DIR}"
            --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
            -I "${GUTENRIP_WORKER_PROTO_DIR}"
            "${GUTENRIP_WORKER_PROTO}"
        DEPENDS "${GUTENRIP_WORKER_PROTO}"
        VERBATIM
    )

    add_library(guten_rip_worker_proto STATIC
        ${GUTENRIP_WORKER_PROTO_SRCS}
    )

    target_include_directories(guten_rip_worker_proto PUBLIC
        "${GUTENRIP_GENERATED_DIR}"
    )

    target_link_libraries(guten_rip_worker_proto PUBLIC
        gRPC::grpc++
        protobuf::libprotobuf
    )

    if (TARGET YinkStudio)
        target_compile_definitions(YinkStudio PRIVATE YINK_HAS_WORKER_CLIENT=1)
        target_link_libraries(YinkStudio PRIVATE guten_rip_worker_proto)
    endif()

    if (TARGET yink_rip_core)
        target_compile_definitions(yink_rip_core PUBLIC YINK_HAS_WORKER_CLIENT=1)
        target_link_libraries(yink_rip_core PUBLIC guten_rip_worker_proto)
    endif()

    set(PRINT_ENGINE_LIB_DIR "${CMAKE_SOURCE_DIR}/build-print-engine" CACHE PATH "Directory containing the built print engine library")
    set(GUTENRIP_DEFAULT_PRINT_ENGINE_INCLUDE_DIR "${GUTENRIP_WORKER_ROOT}/gutenprint/include")
    if (NOT DEFINED PRINT_ENGINE_INCLUDE_DIR OR
        PRINT_ENGINE_INCLUDE_DIR STREQUAL "${CMAKE_SOURCE_DIR}/gutenprint/include" OR
        NOT EXISTS "${PRINT_ENGINE_INCLUDE_DIR}")
        set(PRINT_ENGINE_INCLUDE_DIR "${GUTENRIP_DEFAULT_PRINT_ENGINE_INCLUDE_DIR}" CACHE PATH "Directory containing print engine headers" FORCE)
    endif()
    find_library(PRINT_ENGINE_LIBRARY
        NAMES print_engine libprint_engine print_engine_impl
        PATHS "${PRINT_ENGINE_LIB_DIR}"
        NO_DEFAULT_PATH
    )

    if (BUILD_RIP_WORKER AND PRINT_ENGINE_LIBRARY)
        add_executable(guten-rip-worker
            "${GUTENRIP_WORKER_ROOT}/src/worker/main.cpp"
            "${GUTENRIP_WORKER_ROOT}/src/worker/rip_worker_service.cpp"
            "${GUTENRIP_WORKER_ROOT}/src/worker/rip_worker_service.h"
            "${GUTENRIP_WORKER_ROOT}/src/worker/ed25519_sign.cpp"
            "${GUTENRIP_WORKER_ROOT}/src/worker/ed25519_sign.h"
        )

        target_include_directories(guten-rip-worker PRIVATE
            "${GUTENRIP_WORKER_ROOT}/src"
            "${PRINT_ENGINE_INCLUDE_DIR}"
            "${PRINT_ENGINE_LIB_DIR}"
            "${YINK_WORKER_KEYS_INCLUDE_DIR}"
        )

        target_link_libraries(guten-rip-worker PRIVATE
            guten_rip_worker_proto
            "${PRINT_ENGINE_LIBRARY}"
            OpenSSL::Crypto
            Threads::Threads
        )
    elseif(BUILD_RIP_WORKER)
        message(STATUS "Skipping guten-rip-worker: print engine library was not found in ${PRINT_ENGINE_LIB_DIR}.")
    endif()

    if (BUILD_CORE_TESTS AND PRINT_ENGINE_LIBRARY AND TARGET yink_rip_core AND
        EXISTS "${GUTENRIP_WORKER_HOST_ROOT}/tests/worker/test_rip_worker_client.cpp")
        add_executable(test_rip_worker_client
            "${GUTENRIP_WORKER_HOST_ROOT}/tests/worker/test_rip_worker_client.cpp"
            "${GUTENRIP_WORKER_ROOT}/src/worker/rip_worker_service.cpp"
            "${GUTENRIP_WORKER_ROOT}/src/worker/rip_worker_service.h"
            "${GUTENRIP_WORKER_ROOT}/src/worker/ed25519_sign.cpp"
            "${GUTENRIP_WORKER_ROOT}/src/worker/ed25519_sign.h"
        )

        target_compile_definitions(test_rip_worker_client PRIVATE
            YINK_HAS_WORKER_CLIENT=1
            YINK_TEST_XML_DIR="${GUTENRIP_WORKER_ROOT}/gutenprint/src/xml"
        )
        target_include_directories(test_rip_worker_client PRIVATE
            "${GUTENRIP_WORKER_HOST_ROOT}/src"
            "${GUTENRIP_WORKER_ROOT}/src"
            "${PRINT_ENGINE_INCLUDE_DIR}"
            "${PRINT_ENGINE_LIB_DIR}"
            "${YINK_WORKER_KEYS_INCLUDE_DIR}"
        )

        target_link_libraries(test_rip_worker_client PRIVATE
            yink_rip_core
            guten_rip_worker_proto
            "${PRINT_ENGINE_LIBRARY}"
            Threads::Threads
        )

        if (WIN32)
            target_link_libraries(test_rip_worker_client PRIVATE gdi32)
        endif()

        add_test(NAME rip_worker_client COMMAND test_rip_worker_client)

        if (EXISTS "${GUTENRIP_WORKER_HOST_ROOT}/tests/worker/test_esc_i_feather.cpp")
            add_executable(test_esc_i_feather
                "${GUTENRIP_WORKER_HOST_ROOT}/tests/worker/test_esc_i_feather.cpp"
            )
            target_compile_definitions(test_esc_i_feather PRIVATE
                YINK_TEST_XML_DIR="${GUTENRIP_WORKER_ROOT}/gutenprint/src/xml"
            )
            target_include_directories(test_esc_i_feather PRIVATE
                "${PRINT_ENGINE_INCLUDE_DIR}"
                "${PRINT_ENGINE_LIB_DIR}"
            )
            target_link_libraries(test_esc_i_feather PRIVATE
                "${PRINT_ENGINE_LIBRARY}"
            )
            add_test(NAME esc_i_feather COMMAND test_esc_i_feather)
        endif()

        if (EXISTS "${GUTENRIP_WORKER_HOST_ROOT}/tests/worker/test_xml_raw_escape.cpp")
            add_executable(test_xml_raw_escape
                "${GUTENRIP_WORKER_HOST_ROOT}/tests/worker/test_xml_raw_escape.cpp"
            )
            target_include_directories(test_xml_raw_escape PRIVATE
                "${PRINT_ENGINE_INCLUDE_DIR}"
                "${PRINT_ENGINE_LIB_DIR}"
            )
            target_link_libraries(test_xml_raw_escape PRIVATE
                "${PRINT_ENGINE_LIBRARY}"
            )
            add_test(NAME xml_raw_escape COMMAND test_xml_raw_escape)
        endif()
    elseif(BUILD_CORE_TESTS)
        message(STATUS "Skipping test_rip_worker_client: print engine library was not found in ${PRINT_ENGINE_LIB_DIR}.")
    endif()
else()
    message(STATUS "Skipping guten-rip-worker and worker client: Protobuf/gRPC C++ packages were not found.")
endif()
