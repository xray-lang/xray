# Embed one text file as a NUL-terminated C byte array.
#
# Generated C carries the shared text kernel verbatim so every executor
# compiles the same source. Invoked in script mode with INPUT, OUTPUT and
# SYMBOL defined.
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "EmbedTextFile.cmake needs INPUT, OUTPUT and SYMBOL")
endif()
file(READ "${INPUT}" _embed_hex HEX)
string(LENGTH "${_embed_hex}" _embed_hex_length)
math(EXPR _embed_size "${_embed_hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _embed_bytes "${_embed_hex}")
get_filename_component(_embed_name "${INPUT}" NAME)
file(WRITE "${OUTPUT}"
    "/* Generated from ${_embed_name}; do not edit. */\n"
    "static const unsigned char ${SYMBOL}[] = {${_embed_bytes}0x00};\n"
    "static const size_t ${SYMBOL}_size = ${_embed_size};\n")
