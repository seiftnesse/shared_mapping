# Generates the per-build kernel offset table header from kernel_offsets.csv.

function(generate_kernel_offsets target_name)
    set(gen_dst ${CMAKE_BINARY_DIR}/gen/include)
    set(_out ${gen_dst}/kernel_offsets_table.gen.h)

    if (NOT TARGET generate_kernel_offsets)
        file(MAKE_DIRECTORY ${gen_dst})
        set(_csv ${CMAKE_CURRENT_SOURCE_DIR}/SourceFiles/common/kernel_offsets.csv)
        set(_script ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/kernel_offsets_generate.cmake)
        set(_template ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/templates/kernel_offsets_table.gen.h.in)

        add_custom_command(
                OUTPUT ${_out}
                COMMAND ${CMAKE_COMMAND}
                -DSM_OFFSETS_CSV=${_csv}
                -DSM_TEMPLATE=${_template}
                -DSM_OUTPUT=${_out}
                -P ${_script}
                DEPENDS ${_csv} ${_script} ${_template}
                COMMENT "Generating kernel offsets table (${_out})"
                VERBATIM
        )
        add_custom_target(generate_kernel_offsets DEPENDS ${_out})
    endif ()

    target_sources(${target_name} PRIVATE ${_out})
    target_include_directories(${target_name} PUBLIC ${gen_dst})
endfunction()
