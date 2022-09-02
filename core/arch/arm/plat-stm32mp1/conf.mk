# 1GB and 512MB DDR targets do not locate secure DDR at the same place.
flavor_dts_file-157A_DK1 = stm32mp157a-dk1.dts
flavor_dts_file-157A_ED1 = stm32mp157a-ed1.dts
flavor_dts_file-157A_EV1 = stm32mp157a-ev1.dts
flavor_dts_file-157C_DK2 = stm32mp157c-dk2.dts
flavor_dts_file-157C_ED1 = stm32mp157c-ed1.dts
flavor_dts_file-157C_EV1 = stm32mp157c-ev1.dts
flavor_dts_file-157D_DK1 = stm32mp157d-dk1.dts
flavor_dts_file-157D_ED1 = stm32mp157d-ed1.dts
flavor_dts_file-157D_EV1 = stm32mp157d-ev1.dts
flavor_dts_file-157F_DK2 = stm32mp157f-dk2.dts
flavor_dts_file-157F_ED1 = stm32mp157f-ed1.dts
flavor_dts_file-157F_EV1 = stm32mp157f-ev1.dts
flavor_dts_file-135D_DK = stm32mp135d-dk.dts
flavor_dts_file-135F_DK = stm32mp135f-dk.dts
# Digi platforms
flavor_dts_file-ccmp13-dvk = ccmp13-dvk.dts
flavor_dts_file-ccmp15-dvk = ccmp15-dvk.dts

flavorlist-512M = $(flavor_dts_file-157A_DK1) \
		  $(flavor_dts_file-157C_DK2) \
		  $(flavor_dts_file-157D_DK1) \
		  $(flavor_dts_file-157F_DK2) \
		  $(flavor_dts_file-135D_DK) \
		  $(flavor_dts_file-135F_DK) \
		  $(flavor_dts_file-ccmp15-dvk)

flavorlist-1G = $(flavor_dts_file-157A_ED1) \
		$(flavor_dts_file-157A_EV1) \
		$(flavor_dts_file-157D_ED1) \
		$(flavor_dts_file-157D_EV1) \
		$(flavor_dts_file-157C_ED1) \
		$(flavor_dts_file-157C_EV1) \
		$(flavor_dts_file-157F_ED1) \
		$(flavor_dts_file-157F_EV1)

flavorlist-MP15 = $(flavor_dts_file-157A_DK1) \
		  $(flavor_dts_file-157C_DK2) \
		  $(flavor_dts_file-157D_DK1) \
		  $(flavor_dts_file-157F_DK2) \
		  $(flavor_dts_file-157A_ED1) \
		  $(flavor_dts_file-157A_EV1) \
		  $(flavor_dts_file-157D_ED1) \
		  $(flavor_dts_file-157D_EV1) \
		  $(flavor_dts_file-157C_ED1) \
		  $(flavor_dts_file-157C_EV1) \
		  $(flavor_dts_file-157F_ED1) \
		  $(flavor_dts_file-157F_EV1) \
		  $(flavor_dts_file-ccmp15-dvk)

flavorlist-MP13 = $(flavor_dts_file-135D_DK) \
		  $(flavor_dts_file-135F_DK) \
		  $(flavor_dts_file-ccmp13-dvk)

ifneq ($(PLATFORM_FLAVOR),)
ifeq ($(flavor_dts_file-$(PLATFORM_FLAVOR)),)
$(error Invalid platform flavor $(PLATFORM_FLAVOR))
endif
CFG_EMBED_DTB_SOURCE_FILE ?= $(flavor_dts_file-$(PLATFORM_FLAVOR))
endif

ifneq ($(filter $(CFG_EMBED_DTB_SOURCE_FILE),$(flavorlist-MP13)),)
$(call force,CFG_STM32MP13,y)
endif

ifneq ($(filter $(CFG_EMBED_DTB_SOURCE_FILE),$(flavorlist-MP15)),)
$(call force,CFG_STM32MP15,y)
endif

ifeq ($(filter $(CFG_STM32MP15) $(CFG_STM32MP13),y),)
$(error STM32 Platform must be defined)
endif

include core/arch/arm/cpu/cortex-a7.mk

$(call force,CFG_ARM_GIC_PM,y)
$(call force,CFG_GIC,y)
$(call force,CFG_INIT_CNTVOFF,y)
$(call force,CFG_PM,y)
$(call force,CFG_PM_ARM32,y)
$(call force,CFG_PM_STUBS,y)
$(call force,CFG_PSCI_ARM32,y)
$(call force,CFG_SECURE_TIME_SOURCE_CNTPCT,y)
$(call force,CFG_SM_PLATFORM_HANDLER,y)

ifeq ($(CFG_STM32MP13),y)
$(call force,CFG_CORE_ASYNC_NOTIF,y)
$(call force,CFG_CORE_ASYNC_NOTIF_GIC_INTID,31)
$(call force,CFG_BOOT_SECONDARY_REQUEST,n)
$(call force,CFG_DRIVERS_CLK,y)
$(call force,CFG_DRIVERS_CLK_FIXED,y)
$(call force,CFG_RPROC_PTA,n)
$(call force,CFG_REGULATOR_DRIVERS,y)
$(call force,CFG_SECONDARY_INIT_CNTFRQ,n)
$(call force,CFG_STM32_CRYP,n)
$(call force,CFG_STM32_EXTI,y)
$(call force,CFG_STM32_GPIO,y)
$(call force,CFG_STM32_HSE_MONITORING,y)
$(call force,CFG_STM32MP_CLK_CORE,y)
$(call force,CFG_STM32MP1_SCMI_SIP,n)
$(call force,CFG_STM32MP13_CLK,y)
$(call force,CFG_STM32MP15,n)
$(call force,CFG_TEE_CORE_NB_CORE,1)
$(call force,CFG_TZSRAM_START,0x2ffe0000)
$(call force,CFG_TZSRAM_SIZE,0x0001f000)
$(call force,CFG_WITH_NSEC_GPIOS,n)
CFG_NUM_THREADS ?= 5
CFG_WITH_PAGER ?= n
CFG_WITH_TUI ?= y
else # Assume CFG_STM32MP15
$(call force,CFG_BOOT_SECONDARY_REQUEST,y)
$(call force,CFG_DDR_LOWPOWER,y)
$(call force,CFG_DRIVERS_CLK,y)
$(call force,CFG_DRIVERS_CLK_FIXED,n)
$(call force,CFG_REGULATOR_DRIVERS,y)
$(call force,CFG_SCMI_MSG_PERF_DOMAIN,n)
$(call force,CFG_SECONDARY_INIT_CNTFRQ,y)
$(call force,CFG_STM32_PKA,n)
$(call force,CFG_STM32_SAES,n)
$(call force,CFG_STM32_VREFBUF,n)
$(call force,CFG_STM32MP13,n)
$(call force,CFG_STM32MP15,y)
$(call force,CFG_STM32MP13_CLK,n)
$(call force,CFG_STM32MP15_CLK,y)
$(call force,CFG_WITH_NSEC_GPIOS,y)
CFG_NUM_THREADS ?= 3
CFG_STM32MP1_CPU_OPP ?= y
CFG_TEE_CORE_NB_CORE ?= 2
CFG_WITH_PAGER ?= y
endif # CFG_STM32MPx

# Trusted User Interface
ifeq ($(CFG_WITH_TUI),y)
$(call force,CFG_DISPLAY,y,Mandated by CFG_WITH_TUI)
$(call force,CFG_FRAME_BUFFER,y,Mandated by CFG_WITH_TUI)
$(call force,CFG_STM32_LTDC,y,Mandated by CFG_WITH_TUI)
# Provision virtual space to fit 10MByte plus the TUI frame buffer
CFG_TUI_FRAME_BUFFER_SIZE_MAX ?= 0x01000000
CFG_RESERVED_VASPACE_SIZE ?= (10 * 1024 * 1024 + $(CFG_TUI_FRAME_BUFFER_SIZE_MAX))
endif

ifneq ($(filter $(CFG_EMBED_DTB_SOURCE_FILE),$(flavorlist-512M)),)
CFG_DRAM_SIZE    ?= 0x20000000
endif

CFG_DRAM_BASE    ?= 0xc0000000
CFG_TZSRAM_START ?= 0x2ffc0000
CFG_TZSRAM_SIZE  ?= 0x0003f000
CFG_STM32MP1_SCMI_SHM_BASE ?= 0x2ffff000
CFG_STM32MP1_SCMI_SHM_SIZE ?= 0x00001000
CFG_TZDRAM_SIZE  ?= 0x01e00000
CFG_SHMEM_SIZE   ?= 0x00200000
CFG_DRAM_SIZE    ?= 0x40000000
CFG_TZDRAM_START ?= ($(CFG_DRAM_BASE) + $(CFG_DRAM_SIZE) - $(CFG_TZDRAM_SIZE))
CFG_SHMEM_START  ?= ($(CFG_TZDRAM_START) - $(CFG_SHMEM_SIZE))

CFG_WITH_LPAE ?= y
CFG_MMAP_REGIONS ?= 30
CFG_DTB_MAX_SIZE ?= (256 * 1024)

# Default disable RPC command shared memory allocation caching due to
# side effect on TEE session release by the Linux tee & optee drivers.
CFG_PREALLOC_RPC_CACHE ?= n

# Disable early TA compression to limit HEAP size
CFG_EARLY_TA_COMPRESS ?= n

# Embed public part of this key in OP-TEE OS
CFG_RPROC_SIGN_KEY ?= keys/default_rproc.pem

ifeq ($(CFG_EMBED_DTB_SOURCE_FILE),)
# Some drivers mandate DT support
$(call force,CFG_DRIVERS_CLK_DT,n)
$(call force,CFG_REGULATOR_FIXED,n)
$(call force,CFG_STM32_CRYP,n)
$(call force,CFG_STM32_GPIO,n)
$(call force,CFG_STM32_HASH,n)
$(call force,CFG_STM32_I2C,n)
$(call force,CFG_STM32_IWDG,n)
$(call force,CFG_STM32_LPTIMER,n)
$(call force,CFG_STM32_PKA,n)
$(call force,CFG_STM32_REGULATOR_GPIO,n)
$(call force,CFG_STM32_RTC,n)
$(call force,CFG_STM32_SAES,n)
$(call force,CFG_STM32_TAMP,n)
$(call force,CFG_STM32_TIM,n)
$(call force,CFG_STM32_VREFBUF,y)
$(call force,CFG_STPMIC1,n)
$(call force,CFG_STM32MP1_SCMI_SIP,n)
$(call force,CFG_SCMI_PTA,n)
else
$(call force,CFG_DRIVERS_CLK_DT,y)
endif

# Enable Early TA NVMEM for provisioning management
CFG_TA_STM32MP_NVMEM ?= y
ifeq ($(CFG_TA_STM32MP_NVMEM),y)
$(call force,CFG_BSEC_PTA,y,Mandated by CFG_TA_STM32MP_NVMEM)
CFG_IN_TREE_EARLY_TAS += stm32mp_nvmem/1a8342cc-81a5-4512-99fe-9e2b3e37d626
endif

# Enable BSEC Pseudo TA for fuses access management
CFG_BSEC_PTA ?= y
ifeq ($(CFG_BSEC_PTA),y)
$(call force,CFG_STM32_BSEC,y,Mandated by CFG_BSEC_PTA)
endif

# Remoteproc early TA for coprocessor firmware management
CFG_RPROC_PTA ?= n
ifeq ($(CFG_RPROC_PTA),y)
CFG_IN_TREE_EARLY_TAS += remoteproc/80a4c275-0a47-4905-8285-1486a9771a08
endif

CFG_REGULATOR_FIXED ?= y
CFG_STM32_BSEC ?= y
CFG_STM32_CLKCALIB ?=y
CFG_STM32_CRYP ?= y
CFG_STM32_ETZPC ?= y
CFG_STM32_GPIO ?= y
CFG_STM32_HASH ?= y
CFG_STM32_I2C ?= y
CFG_STM32_IWDG ?= y
CFG_STM32_LPTIMER ?= y
CFG_STM32_PKA ?= y
CFG_STM32_REGULATOR_GPIO ?= y
CFG_STM32_RNG ?= y
CFG_STM32_RTC ?= y
CFG_STM32_SAES ?= y
CFG_STM32_TAMP ?= y
CFG_STM32_TIM ?= y
CFG_STM32_UART ?= y
CFG_STM32_VREFBUF ?= y
CFG_STM32MP1_CPU_OPP ?= y
CFG_STPMIC1 ?= y
CFG_SYSCFG ?= y
CFG_TZC400 ?= y

CFG_WITH_SOFTWARE_PRNG ?= n
ifeq ($(CFG_WITH_SOFTWARE_PRNG),y)
$(call force,CFG_STM32_RNG,y,Mandated by CFG_WITH_SOFTWARE_PRNG)
endif

ifeq ($(CFG_STM32_ETZPC),y)
$(call force,CFG_STM32_FIREWALL,y)
endif

ifeq ($(CFG_STPMIC1),y)
$(call force,CFG_STM32_I2C,y)
$(call force,CFG_STM32_GPIO,y)
endif

ifeq ($(CFG_STM32_HSE_MONITORING),y)
$(call force,CFG_STM32_LPTIMER,y)
endif

ifeq ($(call cfg-one-enabled, CFG_STM32_LPTIMER CFG_STM32_TIM),y)
$(call force,CFG_COUNTER_DRIVER,y)
endif

# if any crypto driver is enabled, enable the crypto-framework layer
ifeq ($(call cfg-one-enabled, CFG_STM32_CRYP \
	                      CFG_STM32_SAES \
			      CFG_STM32_PKA \
			      CFG_STM32_HASH),y)
$(call force,CFG_STM32_CRYPTO_DRIVER,y)
endif

# Platform specific configuration
CFG_STM32MP_PANIC_ON_TZC_PERM_VIOLATION ?= y

# SiP/OEM service for non-secure world
CFG_STM32_LOWPOWER_SIP ?= $(CFG_PM)
CFG_STM32_PWR_SIP ?= y
CFG_STM32MP1_SCMI_SIP ?= n
ifeq ($(CFG_STM32MP1_SCMI_SIP),y)
$(call force,CFG_SCMI_MSG_DRIVERS,y,Mandated by CFG_STM32MP1_SCMI_SIP)
$(call force,CFG_SCMI_MSG_SMT_FASTCALL_ENTRY,y,Mandated by CFG_STM32MP1_SCMI_SIP)
endif

# Default enable SCMI PTA support
CFG_SCMI_PTA ?= y
ifeq ($(CFG_SCMI_PTA),y)
$(call force,CFG_SCMI_MSG_DRIVERS,y,Mandated by CFG_SCMI_PTA)
$(call force,CFG_SCMI_MSG_SMT_THREAD_ENTRY,y,Mandated by CFG_SCMI_PTA)
CFG_CORE_OCALL ?= y
endif

# Default enable HWRNG PTA support
CFG_HWRNG_PTA ?= y
ifeq ($(CFG_HWRNG_PTA),y)
$(call force,CFG_STM32_RNG,y,Mandated by CFG_HWRNG_PTA)
$(call force,CFG_WITH_SOFTWARE_PRNG,n,Mandated by CFG_HWRNG_PTA)
CFG_HWRNG_QUALITY ?= 1024
endif

CFG_SCMI_MSG_DRIVERS ?= n
ifeq ($(CFG_SCMI_MSG_DRIVERS),y)
$(call force,CFG_SCMI_MSG_CLOCK,y)
$(call force,CFG_SCMI_MSG_RESET_DOMAIN,y)
$(call force,CFG_SCMI_MSG_SMT,y)
$(call force,CFG_SCMI_MSG_REGULATOR_CONSUMER,y)
$(call force,CFG_SCMI_MSG_VOLTAGE_DOMAIN,y)
CFG_SCMI_MSG_PERF_DOMAIN ?= y
endif

# Default use stm32mp1 PM mailbox context version 3
# Use CFG_STM32MP1_PM_CONTEXT_VERSION=1 to force version 0 when dealing with
# a TF-A firmware that supports version 1 of the context mailbox.
CFG_STM32MP1_PM_CONTEXT_VERSION ?= 3

# Default enable some test facitilites
CFG_ENABLE_EMBEDDED_TESTS ?= y
CFG_WITH_STATS ?= y
CFG_WERROR ?= y

# Enable to allow debug
ifeq ($(CFG_TEE_CORE_DEBUG),y)
CFG_STM32_BSEC_WRITE ?= y
endif

# Default disable some support for pager memory size constraint
ifeq ($(CFG_WITH_PAGER),y)
CFG_TEE_CORE_DEBUG ?= n
CFG_UNWIND ?= n
CFG_LOCKDEP ?= n
CFG_TA_BGET_TEST ?= n
CFG_CORE_HEAP_SIZE ?= 49152
endif

# Default disable ASLR
CFG_CORE_ASLR ?= n

# Non-secure UART for the output console
CFG_WITH_NSEC_UARTS ?= y
# UART instance used for early console (0 disables early console)
CFG_STM32_EARLY_CONSOLE_UART ?= 0

# Generate the STM32 files
CFG_STM32MP15x_STM32IMAGE ?= n
