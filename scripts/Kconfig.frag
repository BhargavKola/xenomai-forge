config XENOMAI
	depends on (X86_TSC || !X86) && (!HPET_TIMER || !X86 || X86_LOCAL_APIC)
	bool "Xenomai"
        select IPIPE
	default y
        help
          Xenomai is a real-time extension to the Linux kernel. Note
          that Xenomai relies on Adeos interrupt pipeline (CONFIG_IPIPE
          option) to be enabled, so enabling this option selects the
          CONFIG_IPIPE option.

if XENOMAI
source "arch/@LINUX_ARCH@/xenomai/Kconfig"
endif

if APM || CPU_FREQ || ACPI_PROCESSOR || INTEL_IDLE
comment "WARNING! You enabled APM, CPU Frequency scaling, ACPI 'processor'"
comment "or Intel cpuidle option. These options are known to cause troubles"
comment "with Xenomai, disable them."
endif

comment "NOTE: Xenomai needs either X86_LOCAL_APIC enabled or HPET_TIMER disabled."
	depends on !X86_LOCAL_APIC && X86 && HPET_TIMER
comment "(menu Processor type and features)"
	depends on !X86_LOCAL_APIC && X86 && HPET_TIMER
