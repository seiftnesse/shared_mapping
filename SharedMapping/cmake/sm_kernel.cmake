wdk_add_driver(shared_mapping_driver
    ${SF}/kernelmode/driver.c
    WINVER 0x0A00
)
nice_target_sources(shared_mapping_driver ${SF} PRIVATE
    kernelmode/driver.c
)
