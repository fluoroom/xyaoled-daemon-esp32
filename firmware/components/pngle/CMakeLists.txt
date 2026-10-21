# pngle by kikuchan (MIT), vendored from github.com/kikuchan/pngle at
# b1c68193f1d3f8642b3e0e095d457a828038e6fb, unchanged.
idf_component_register(SRCS "pngle.c" "miniz.c"
                       INCLUDE_DIRS ".")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PNGLE_NO_GAMMA_CORRECTION)
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
