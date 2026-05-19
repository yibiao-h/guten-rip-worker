find_package(Protobuf CONFIG QUIET)
find_package(gRPC CONFIG QUIET)
find_package(Qt6 6.11 QUIET COMPONENTS Core Gui)

get_filename_component(GUTENRIP_WORKER_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(GUTENRIP_WORKER_HOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

if (NOT TARGET guten_rip_core)
    add_library(guten_rip_worker_support STATIC
        "${GUTENRIP_WORKER_ROOT}/src/rip/escp_parser.cpp"
        "${GUTENRIP_WORKER_ROOT}/src/rip/escp_parser.h"
        "${GUTENRIP_WORKER_ROOT}/src/rip/packbits.cpp"
        "${GUTENRIP_WORKER_ROOT}/src/rip/packbits.h"
        "${GUTENRIP_WORKER_ROOT}/src/rip/prn_compositor.h"
        "${GUTENRIP_WORKER_ROOT}/src/rip/prn_stream_rewriter.cpp"
        "${GUTENRIP_WORKER_ROOT}/src/rip/prn_stream_rewriter.h"
        "${GUTENRIP_WORKER_ROOT}/src/rip/raw_interop_image.cpp"
        "${GUTENRIP_WORKER_ROOT}/src/rip/raw_interop_image.h"
        "${GUTENRIP_WORKER_ROOT}/src/rip/white_plate_generator.cpp"
        "${GUTENRIP_WORKER_ROOT}/src/rip/white_plate_generator.h"
    )
    target_include_directories(guten_rip_worker_support PUBLIC
        "${GUTENRIP_WORKER_ROOT}/src"
    )
    set(GUTENRIP_WORKER_SUPPORT_TARGET guten_rip_worker_support)
else()
    set(GUTENRIP_WORKER_SUPPORT_TARGET guten_rip_core)
endif()

if (Protobuf_FOUND AND gRPC_FOUND AND
    TARGET protobuf::protoc AND TARGET protobuf::libprotobuf AND
    TARGET gRPC::grpc++ AND TARGET gRPC::grpc_cpp_plugin AND
    TARGET Qt6::Core AND TARGET Qt6::Gui)
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
        target_compile_definitions(YinkStudio PRIVATE GUTENRIP_HAS_WORKER_CLIENT=1)
        target_link_libraries(YinkStudio PRIVATE guten_rip_worker_proto)
    endif()

    set(GUTENPRINT_LIB_DIR "${CMAKE_SOURCE_DIR}/build-gutenprint/_out" CACHE PATH "Directory containing the built Gutenprint library")
    set(GUTENRIP_DEFAULT_GUTENPRINT_INCLUDE_DIR "${GUTENRIP_WORKER_ROOT}/gutenprint/include")
    if (NOT DEFINED GUTENPRINT_INCLUDE_DIR OR
        GUTENPRINT_INCLUDE_DIR STREQUAL "${CMAKE_SOURCE_DIR}/gutenprint/include" OR
        NOT EXISTS "${GUTENPRINT_INCLUDE_DIR}")
        set(GUTENPRINT_INCLUDE_DIR "${GUTENRIP_DEFAULT_GUTENPRINT_INCLUDE_DIR}" CACHE PATH "Directory containing Gutenprint headers" FORCE)
    endif()
    find_library(GUTENPRINT_LIBRARY
        NAMES gutenprint libgutenprint
        PATHS "${GUTENPRINT_LIB_DIR}"
        NO_DEFAULT_PATH
    )

    if (WIN32)
        file(GLOB GUTENPRINT_RUNTIME_FILES CONFIGURE_DEPENDS "${GUTENPRINT_LIB_DIR}/libgutenprint*.dll")
    elseif(APPLE)
        file(GLOB GUTENPRINT_RUNTIME_FILES CONFIGURE_DEPENDS "${GUTENPRINT_LIB_DIR}/libgutenprint*.dylib")
    else()
        file(GLOB GUTENPRINT_RUNTIME_FILES CONFIGURE_DEPENDS "${GUTENPRINT_LIB_DIR}/libgutenprint.so*")
    endif()

    if (BUILD_RIP_WORKER AND GUTENPRINT_LIBRARY)
        add_executable(guten-rip-worker
            "${GUTENRIP_WORKER_ROOT}/src/worker/main.cpp"
            "${GUTENRIP_WORKER_ROOT}/src/worker/rip_worker_service.cpp"
            "${GUTENRIP_WORKER_ROOT}/src/worker/rip_worker_service.h"
        )

        target_include_directories(guten-rip-worker PRIVATE
            "${GUTENRIP_WORKER_HOST_ROOT}/src"
            "${GUTENRIP_WORKER_ROOT}/src"
            "${GUTENPRINT_INCLUDE_DIR}"
            "${GUTENPRINT_LIB_DIR}"
        )

        target_link_libraries(guten-rip-worker PRIVATE
            ${GUTENRIP_WORKER_SUPPORT_TARGET}
            guten_rip_worker_proto
            Qt6::Core
            Qt6::Gui
            "${GUTENPRINT_LIBRARY}"
        )
    elseif(BUILD_RIP_WORKER)
        message(STATUS "Skipping guten-rip-worker: Gutenprint library was not found in ${GUTENPRINT_LIB_DIR}.")
    endif()

    if (BUILD_CORE_TESTS AND TARGET Qt6::Core AND GUTENPRINT_LIBRARY AND
        EXISTS "${GUTENRIP_WORKER_HOST_ROOT}/tests/worker/test_rip_worker_client.cpp")
        add_executable(test_rip_worker_client
            "${GUTENRIP_WORKER_HOST_ROOT}/tests/worker/test_rip_worker_client.cpp"
            "${GUTENRIP_WORKER_HOST_ROOT}/src/core/cmyk_plane_preparer.cpp"
            "${GUTENRIP_WORKER_HOST_ROOT}/src/core/print_driver_catalog.cpp"
            "${GUTENRIP_WORKER_HOST_ROOT}/src/core/print_driver_catalog.h"
            "${GUTENRIP_WORKER_HOST_ROOT}/src/core/rip_worker_client.cpp"
            "${GUTENRIP_WORKER_HOST_ROOT}/src/core/rip_worker_client.h"
            "${GUTENRIP_WORKER_ROOT}/src/worker/rip_worker_service.cpp"
            "${GUTENRIP_WORKER_ROOT}/src/worker/rip_worker_service.h"
        )

        target_compile_definitions(test_rip_worker_client PRIVATE GUTENRIP_HAS_WORKER_CLIENT=1)
        target_include_directories(test_rip_worker_client PRIVATE
            "${GUTENRIP_WORKER_HOST_ROOT}/src"
            "${GUTENRIP_WORKER_ROOT}/src"
            "${GUTENPRINT_INCLUDE_DIR}"
            "${GUTENPRINT_LIB_DIR}"
        )

        target_link_libraries(test_rip_worker_client PRIVATE
            ${GUTENRIP_WORKER_SUPPORT_TARGET}
            guten_rip_worker_proto
            Qt6::Core
            Qt6::Gui
            lcms2::lcms2
            "${GUTENPRINT_LIBRARY}"
        )

        if (WIN32)
            target_link_libraries(test_rip_worker_client PRIVATE gdi32)
        endif()

        add_test(NAME rip_worker_client COMMAND test_rip_worker_client)
    elseif(BUILD_CORE_TESTS AND TARGET Qt6::Core)
        message(STATUS "Skipping test_rip_worker_client: Gutenprint library was not found in ${GUTENPRINT_LIB_DIR}.")
    endif()
else()
    message(STATUS "Skipping guten-rip-worker and worker client: Protobuf/gRPC C++ packages or Qt6 Core/Gui were not found.")
endif()
