scpfw-incdirs-y += $(scpfw-path)/product/optee-stm32mp2/include

srcs-y += $(scpfw-path)/product/optee-stm32mp2/fw/config_all.c

$(eval $(call scpfw-embed-product-module,psu_optee_regulator))
$(eval $(call scpfw-embed-product-module,stm32_opp))
$(eval $(call scpfw-embed-product-module,stm32_pd))
