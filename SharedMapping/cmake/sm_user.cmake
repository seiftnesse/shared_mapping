add_executable(shared_mapping_client ${SF}/usermode/main.c)
nice_target_sources(shared_mapping_client ${SF} PRIVATE
    usermode/main.c
)
