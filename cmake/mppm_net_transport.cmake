# M5.3b transport package (docs/online/M5_3B_TRANSPORT.md): ENet 1.3.18 and libjuice 1.7.4 fetched at
# pinned release tarballs, the ENet socket shim (port/net/transport/enet_juice.c) and the transport
# tests. Included from CMakeLists.txt (OPTIONAL); owned by the transport package.
#
# ENet (MIT) is built by our own target, not its CMakeLists: unix.c gets its 8 socket functions
# renamed to enet_sys_socket_* so that port/net/transport/enet_juice.c can define the public names.
# libjuice (MPL-2.0) is built unmodified by its own CMake as the static library juice-static.
include(FetchContent)

FetchContent_Declare(mppm_enet
        URL https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz
        URL_HASH SHA256=28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR mppm-no-cmake # no CMakeLists there: populate only, our own target below
)
FetchContent_MakeAvailable(mppm_enet)
set(MPPM_ENET_DIR ${mppm_enet_SOURCE_DIR})

set(_mppm_enet_platform ${MPPM_ENET_DIR}/unix.c)
if (WIN32)
    set(_mppm_enet_platform ${MPPM_ENET_DIR}/win32.c)
endif ()
add_library(mppm_enet STATIC
        ${MPPM_ENET_DIR}/callbacks.c ${MPPM_ENET_DIR}/compress.c ${MPPM_ENET_DIR}/host.c
        ${MPPM_ENET_DIR}/list.c ${MPPM_ENET_DIR}/packet.c ${MPPM_ENET_DIR}/peer.c
        ${MPPM_ENET_DIR}/protocol.c ${_mppm_enet_platform})
set_target_properties(mppm_enet PROPERTIES LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON)
target_include_directories(mppm_enet PUBLIC ${MPPM_ENET_DIR}/include)
if (NOT WIN32)
    # What ENet's own CMake detects on Linux and macOS.
    target_compile_definitions(mppm_enet PRIVATE HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
            HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
endif ()
target_compile_options(mppm_enet PRIVATE -w)
if (WIN32)
    target_link_libraries(mppm_enet PUBLIC ws2_32 winmm)
endif ()
# The platform file only: its socket functions become enet_sys_socket_*; the shim defines the
# public enet_socket_* names (and delegates to these).
set(_mppm_enet_renames)
foreach (_fn IN ITEMS create bind get_address set_option send receive wait destroy)
    list(APPEND _mppm_enet_renames enet_socket_${_fn}=enet_sys_socket_${_fn})
endforeach ()
set_source_files_properties(${_mppm_enet_platform} PROPERTIES COMPILE_DEFINITIONS "${_mppm_enet_renames}")

set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_SERVER OFF CACHE BOOL "" FORCE) # juice_server: the test TURN server (p2net_turn)
FetchContent_Declare(mppm_juice
        URL https://github.com/paullouisageneau/libjuice/archive/refs/tags/v1.7.4.tar.gz
        URL_HASH SHA256=95c088862aa1b88b73d62aa99577135010eb768fcafb252d13aa85a8515ef798
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        EXCLUDE_FROM_ALL
)
# Do not leave a global BUILD_SHARED_LIBS behind (the borealis xxhash pattern).
set(_mppm_had_bsl_cache OFF)
if (DEFINED CACHE{BUILD_SHARED_LIBS})
    set(_mppm_had_bsl_cache ON)
endif ()
FetchContent_MakeAvailable(mppm_juice)
if (NOT _mppm_had_bsl_cache)
    unset(BUILD_SHARED_LIBS CACHE)
endif ()
if (NOT TARGET LibJuice::LibJuiceStatic)
    message(FATAL_ERROR "libjuice did not define LibJuice::LibJuiceStatic")
endif ()
target_compile_options(juice-static PRIVATE -w)

# The transport sources are globbed into mppm by CMakeLists.txt (port/*.c, port/*.cpp).
target_include_directories(mppm PRIVATE ${MPPM_ENET_DIR}/include ${mppm_juice_SOURCE_DIR}/include)
target_compile_definitions(mppm PRIVATE JUICE_STATIC)
target_link_libraries(mppm PRIVATE mppm_enet LibJuice::LibJuiceStatic)
if (WIN32)
    target_link_libraries(mppm PRIVATE iphlpapi) # GetAdaptersAddresses (transport.cpp)
endif ()

# ---- Tests (not built by default): `ninja -C build-pc p2transport_test p2net_turn`, then
# tests/net/transport/transport_test.sh. ----
file(GLOB MPPM_TRANSPORT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/port/net/transport/*.c
        ${CMAKE_SOURCE_DIR}/port/net/transport/*.cpp)
add_executable(p2transport_test EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tests/net/transport/transport_test.cpp
        ${MPPM_TRANSPORT_SOURCES})
target_compile_features(p2transport_test PRIVATE cxx_std_20)
target_include_directories(p2transport_test PRIVATE ${CMAKE_SOURCE_DIR}/port/net ${CMAKE_SOURCE_DIR}/port/net/transport)
target_compile_definitions(p2transport_test PRIVATE JUICE_STATIC)
target_link_libraries(p2transport_test PRIVATE mppm_enet LibJuice::LibJuiceStatic Threads::Threads)

add_executable(p2net_turn EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tests/net/transport/turn_server.c)
target_compile_definitions(p2net_turn PRIVATE JUICE_STATIC)
target_link_libraries(p2net_turn PRIVATE LibJuice::LibJuiceStatic Threads::Threads)
