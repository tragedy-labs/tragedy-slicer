# tragedy-slicer-service — the native host of tragedy-slicer-core: HTTP and a
# WebSocket on 127.0.0.1, speaking the generated message types (ADR 0032 in the
# tragedy-labs monorepo). Boost.Beast, which the engine's dependency build already
# carries, is the only server-capable library in that build; it is header-only.

add_executable(tragedy-slicer-service
    main.cpp
    Service.hpp
    Server.hpp
    Server.cpp
)

find_package(Threads REQUIRED)

target_link_libraries(tragedy-slicer-service PRIVATE
    tragedy-slicer-core
    Boost::boost
    Threads::Threads
)

install(TARGETS tragedy-slicer-service RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
