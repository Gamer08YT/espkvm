idf_component_register(
    SRCS "kvm_notify.c" "tg_chats.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        kvm_regions
        esp_http_client
        esp-tls
        esp_timer
        kvm_config
        kvm_log
        kvm_screentext
        kvm_storage
        video_pipeline
)
