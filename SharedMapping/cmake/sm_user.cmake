add_executable(container ${SF}/usermode/container/main.cpp)
nice_target_sources(container ${SF} PRIVATE
    usermode/container/main.cpp
)
target_include_directories(container PRIVATE ${SF})

add_executable(target_app ${SF}/usermode/target/main.c)
nice_target_sources(target_app ${SF} PRIVATE
    usermode/target/main.c
)
