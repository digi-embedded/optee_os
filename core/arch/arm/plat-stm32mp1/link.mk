ifeq ($(CFG_EMBED_DTB),y)
# Specific hack for stm32mp1: get DDR size from the generated DTB to be
# embedded in core. This force CFG_DRAM_SIZE value when build config
# files are generated.

define get_memory_node
$(shell fdtget -l $(core-embed-fdt-dtb) / | grep memory@)
endef
define get_memory_size
$(shell fdtget -t u $(core-embed-fdt-dtb) /$(get_memory_node) reg | cut -d ' ' -f 2)
endef

$(conf-file): $(core-embed-fdt-dtb)
$(conf-file): CFG_DRAM_SIZE = $(get_memory_size)
$(conf-mk-file): $(core-embed-fdt-dtb)
$(conf-mk-file): CFG_DRAM_SIZE = $(get_memory_size)
$(conf-cmake-file): $(core-embed-fdt-dtb)
$(conf-cmake-file): CFG_DRAM_SIZE = $(get_memory_size)
endif #CFG_EMBED_DTB

include core/arch/arm/kernel/link.mk

ifeq ($(CFG_STM32MP15x_STM32IMAGE),y)
# Create stm32 formatted images from the native binary images

define stm32image_cmd
	@$(cmd-echo-silent) '  GEN     $@'
	$(q)./core/arch/arm/plat-stm32mp1/scripts/stm32image.py \
		--load 0 --entry 0
endef

all: $(link-out-dir)/tee-header_v2.stm32
cleanfiles += $(link-out-dir)/tee-header_v2.stm32
$(link-out-dir)/tee-header_v2.stm32: $(link-out-dir)/tee-header_v2.bin
	$(stm32image_cmd) --source $< --dest $@ --bintype 0x20

all: $(link-out-dir)/tee-pager_v2.stm32
cleanfiles += $(link-out-dir)/tee-pager_v2.stm32
$(link-out-dir)/tee-pager_v2.stm32: $(link-out-dir)/tee-pager_v2.bin
	$(stm32image_cmd) --source $< --dest $@ --bintype 0x21

all: $(link-out-dir)/tee-pageable_v2.stm32
cleanfiles += $(link-out-dir)/tee-pageable_v2.stm32
$(link-out-dir)/tee-pageable_v2.stm32: $(link-out-dir)/tee-pageable_v2.bin
	$(stm32image_cmd) --source $< --dest $@ --bintype 0x22
endif
