# M5.3b lobby client (server package; docs/online/M5_3B_NETCODE.md). port/net/lobby_client.cpp is picked up by
# the port/ glob of the mppm target; it talks to mppm-server (server/) over borealis::ws.
target_link_libraries(mppm PRIVATE borealis::ws)

# Lobby client test against a running mppm-server: `ninja -C build-pc p2lobby_test`, then
# `./build-pc/p2lobby_test http://127.0.0.1:<port>` (tests/net/lobby/lobby_test.sh starts the server). Not part
# of the default build.
add_executable(p2lobby_test EXCLUDE_FROM_ALL
        ${CMAKE_SOURCE_DIR}/tests/net/lobby/lobby_client_test.cpp
        ${CMAKE_SOURCE_DIR}/port/net/lobby_client.cpp)
target_compile_features(p2lobby_test PRIVATE cxx_std_20)
target_include_directories(p2lobby_test PRIVATE ${CMAKE_SOURCE_DIR}/port/net ${CMAKE_SOURCE_DIR}/port/include)
target_link_libraries(p2lobby_test PRIVATE borealis::ws nlohmann_json::nlohmann_json Threads::Threads)
