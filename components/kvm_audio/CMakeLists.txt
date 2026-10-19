idf_component_register(
    SRCS "kvm_audio.c" "opus.c" "aac.c" "resample.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_driver_i2s
        esp_driver_gpio
        esp_timer
        video_pipeline
        kvm_config
        esp_ringbuf
)
