add_executable(container
    ${SF}/usermode/container/ldr_walk.cpp
    ${SF}/usermode/container/main.cpp
    ${SF}/usermode/container/mirror_view.cpp
)
target_include_directories(container PRIVATE ${SF})

add_executable(target_app ${SF}/usermode/target/main.c)
nice_target_sources(target_app ${SF} PRIVATE
    usermode/target/main.c
)
