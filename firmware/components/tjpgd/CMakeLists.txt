# TJpgDec R0.03 by ChaN (elm-chan.org/fsw/tjpgd), vendored; only
# JD_FASTDECODE in tjpgdcnf.h changed (0 -> 1, for 32-bit cores).
idf_component_register(SRCS "tjpgd.c"
                       INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
