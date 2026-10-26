idf_component_register(
    SRCS "runbook_script.c" "runbook.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        espressif__cjson
        esp_timer
        kvm_config
        kvm_hid
        kvm_screentext
        kvm_record
        kvm_cec
        kvm_regions
)
