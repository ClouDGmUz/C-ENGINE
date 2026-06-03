file(READ "${SPV_FILE}" spv_raw HEX)
string(LENGTH "${spv_raw}" hex_len)
math(EXPR u32_count "${hex_len} / 8")

set(data "")
math(EXPR last "${u32_count} - 1")
foreach(i RANGE 0 ${last})
    math(EXPR pos "${i} * 8")
    string(SUBSTRING "${spv_raw}" ${pos} 8 word)
    string(APPEND data "    0x")
    # Reverse bytes (SPV is little-endian, file READ HEX is big-endian pairs)
    string(SUBSTRING "${word}" 6 2 b0)
    string(SUBSTRING "${word}" 4 2 b1)
    string(SUBSTRING "${word}" 2 2 b2)
    string(SUBSTRING "${word}" 0 2 b3)
    string(APPEND data "${b0}${b1}${b2}${b3}")
    if(i LESS ${last})
        string(APPEND data ",\n")
    endif()
endforeach()

file(WRITE "${OUT}"
    "// Auto-generated from ${SPV_FILE}\n"
    "#include <cstdint>\n"
    "const uint32_t ${VAR_NAME}[] = {\n${data}\n};\n"
    "const size_t ${VAR_NAME}_size = ${u32_count} * sizeof(uint32_t);\n"
)
