# SPDX-License-Identifier: Apache-2.0
# settings: the saved settings of both firmwares in NVS.
# Host tests: make -C tests
idf_component_register(SRCS "src/settings.c" "src/settings_core.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES nvs_flash esp_hw_support log freertos)
