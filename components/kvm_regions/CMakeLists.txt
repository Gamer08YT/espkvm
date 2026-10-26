idf_component_register(
    SRCS "region_core.c" "kvm_regions.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_timer
        kvm_config
        nvs_flash
        espressif__cjson
)
