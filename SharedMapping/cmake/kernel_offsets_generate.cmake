# Script-mode generator: kernel_offsets.csv -> kernel_offsets_table.gen.h.

if (NOT DEFINED SM_OFFSETS_CSV OR NOT DEFINED SM_TEMPLATE OR NOT DEFINED SM_OUTPUT)
    message(FATAL_ERROR "SM_OFFSETS_CSV, SM_TEMPLATE and SM_OUTPUT must be defined")
endif ()

file(READ ${SM_OFFSETS_CSV} _raw)
# Escape embedded semicolons first so only newlines split the line list.
string(REPLACE ";" "\;" _raw "${_raw}")
string(REPLACE "\r" "" _raw "${_raw}")
string(REPLACE "\n" ";" _lines "${_raw}")

set(SM_OFFSET_ROWS "")
foreach (_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    if (_line STREQUAL "" OR _line MATCHES "^#")
        continue()
    endif ()
    string(REPLACE "," ";" _f "${_line}")
    list(LENGTH _f _n)
    if (NOT _n EQUAL 10)
        message(FATAL_ERROR "kernel_offsets.csv: expected 10 fields, got ${_n}: '${_line}'")
    endif ()
    list(GET _f 0 _os)
    list(GET _f 1 _ubr)
    list(GET _f 2 _ntver)
    list(GET _f 3 _dtb)
    list(GET _f 4 _udtb)
    list(GET _f 5 _ksz)
    list(GET _f 6 _pfn)
    list(GET _f 7 _pfnoff)
    list(GET _f 8 _shift)
    list(GET _f 9 _pfndb)
    string(APPEND SM_OFFSET_ROWS
            "    { ${_os}u, ${_ubr}u, ${_dtb}u, ${_udtb}u, ${_ksz}u, ${_pfn}u, ${_pfnoff}u, ${_shift}u, ${_pfndb}u },  // nt ${_ntver}\n")
endforeach ()

if (SM_OFFSET_ROWS STREQUAL "")
    message(FATAL_ERROR "kernel_offsets.csv contains no data rows")
endif ()

configure_file(${SM_TEMPLATE} ${SM_OUTPUT} @ONLY)
