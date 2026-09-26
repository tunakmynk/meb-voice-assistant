idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "audio_in.c"
        "audio_out.c"
        "net.c"
        "ws_client.c"
        "tur.c"
        "touch.c"
        "katalog.c"
        "secim.c"
        "yuz.c"
        "kurulum.c"
    INCLUDE_DIRS "."
    # Türkçe fontu ikiliye gömer; dosya sisteminden okumak yerine doğrudan
    # flash'tan çizilir (bkz. fonts/README.md).
    EMBED_FILES "fonts/DejaVuSans.ttf"
)
