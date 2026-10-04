wdk_add_driver(shared_mapping_driver
    ${SF}/kernelmode/driver.c
    ${SF}/kernelmode/mirror.c
    ${SF}/kernelmode/target_process.c
    WINVER 0x0A00
)
target_include_directories(shared_mapping_driver PRIVATE ${SF})

target_compile_definitions(shared_mapping_driver PRIVATE SM_KERNEL)
nice_target_sources(shared_mapping_driver ${SF} PRIVATE
    kernelmode/driver.c
    kernelmode/mirror.c
    kernelmode/target_process.c
)

option(SM_SIGN_DRIVER "Test-sign the driver after each build (tools/sign.bat)" ON)
if(SM_SIGN_DRIVER)
    add_custom_command(TARGET shared_mapping_driver POST_BUILD
        COMMAND cmd /c "${CMAKE_CURRENT_SOURCE_DIR}/../tools/sign.bat" $<TARGET_FILE:shared_mapping_driver>
        COMMENT "Test-signing shared_mapping_driver.sys"
        VERBATIM)
endif()
