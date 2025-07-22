global-incdirs-y += .

srcs-y += main.c
srcs-y += stm32mp_pm.c
subdirs-y += drivers
subdirs-$(CFG_STM32_PSA_SERVICE) += psa_service

srcs-$(CFG_DISPLAY) += display.c
srcs-$(CFG_SCMI_SCPFW) += scmi_server_scpfw.c

subdirs-$(CFG_PSA_ADAC) += psa-adac
