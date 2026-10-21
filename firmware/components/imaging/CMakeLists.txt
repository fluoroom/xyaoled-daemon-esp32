idf_component_register(SRCS "img_core.c" "img_frames.c" "img_gif_dec.c" "img_gif_enc.c"
                            "img_gif_scan.c" "img_png.c" "img_jpeg.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES pngle tjpgd)
