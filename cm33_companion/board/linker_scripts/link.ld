OUTPUT_FORMAT ("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)
ENTRY(Reset_Handler)

PROVIDE(__StackSize = DEFINED(APP_MSP_STACK_SIZE) ? APP_MSP_STACK_SIZE : 0x1000);

MEMORY
{
    m33_nvm : ORIGIN = 0x60340000, LENGTH = 0x00200000
    m33_nvm_C : ORIGIN = 0x08340000, LENGTH = 0x00200000
    m33_trailer : ORIGIN = 0x60540000, LENGTH = 0x00040000
    m33_trailer_C : ORIGIN = 0x08540000, LENGTH = 0x00040000
    m33_m55_shared : ORIGIN = 0x26370000, LENGTH = 0x00040000
    m33_m55_shared_C : ORIGIN = 0x06370000, LENGTH = 0x00040000
    m33_code : ORIGIN = 0x24058000, LENGTH = 0x00065000
    m33_code_C : ORIGIN = 0x04058000, LENGTH = 0x00065000
    m33_data : ORIGIN = 0x240BD000, LENGTH = 0x00040000
    m33_data_C : ORIGIN = 0x040BD000, LENGTH = 0x00040000
    m33_allocatable_shared : ORIGIN = 0x240FE000, LENGTH = 0x00001000
    m33_allocatable_shared_C : ORIGIN = 0x040FE000, LENGTH = 0x00001000
}

REGION_ALIAS("m33_nvm_sel", m33_nvm_C)
REGION_ALIAS("m33_trailer_sel", m33_trailer_C)
REGION_ALIAS("m33_m55_shared_sel", m33_m55_shared_C)
REGION_ALIAS("m33_code_sel", m33_code_C)
REGION_ALIAS("m33_data_sel", m33_data_C)
REGION_ALIAS("m33_allocatable_shared_sel", m33_allocatable_shared_C)

VECTORS_ALIGNMENT = 0x400;
m33_code_sel_Offset = ORIGIN(m33_code) - ORIGIN(m33_code_sel);
MCUBOOT_HEADER_SIZE = 0x400;

SECTIONS
{
    .mcu_boot_header : ALIGN(MCUBOOT_HEADER_SIZE)
    {
        . += MCUBOOT_HEADER_SIZE;
    } > m33_nvm_sel

    .app_code_main : ALIGN(VECTORS_ALIGNMENT)
    {
        KEEP(*(.vectors))
        KEEP(*(.init))
        KEEP(*(.fini))
        *crtbegin.*(.ctors)
        *crtbegin?.*(.ctors)
        *(EXCLUDE_FILE(*crtend?.* *crtend.*) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        *crtbegin.*(.dtors)
        *crtbegin?.*(.dtors)
        *(EXCLUDE_FILE(*crtend?.* *crtend.*) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        . = ALIGN(4);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);

        *(.rodata .rodata.* .constdata .constdata.* .conststring .conststring.*)
        *(EXCLUDE_FILE(*cy_syslib.* *cy_syslib_ext.* *cy_smif.* *cy_smif_memnum.* *cy_smif_memslot.* *cy_smif_sfdp.* *cy_smif_hb_flash.* *mtb_hal_memoryspi.* *mtb_serial_memory.* *freertos/Source*.* *cy_clib_support*.* *cy_mutex_pool*.* *cy_time.* *cyabs_*.* *cy_worker_thread.* *cmsis*.*) .text*)
    } > m33_nvm_sel

    .ARM.extab : ALIGN(4)
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > m33_nvm_sel

    .ARM.exidx : ALIGN(4)
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > m33_nvm_sel

    .copy.table : ALIGN(4)
    {
        __copy_table_start__ = .;
        LONG (LOADADDR(.data))
        LONG (ADDR(.data))
        LONG (SIZEOF(.data)/4)

        LONG(LOADADDR(.app_code_ram))
        LONG(ADDR(.app_code_ram) + m33_code_sel_Offset)
        LONG(SIZEOF(.app_code_ram)/4)
        __copy_table_end__ = .;
    } > m33_nvm_sel

    .zero.table : ALIGN(4)
    {
        __zero_table_start__ = .;
        LONG(__bss_start__)
        LONG((__bss_end__ - __bss_start__)/4)
        __zero_table_end__ = .;
    } > m33_nvm_sel

    .app_code_ram : ALIGN(4)
    {
        KEEP(*(.cy_sram_code))
        KEEP(*(.cy_ramfunc))
        KEEP(*(.text.cy_os_common))
        *cy_syslib_ext.*(.text*)
        *cy_syslib.*(.text*)
        *cy_smif.*(.text*)
        *cy_smif_memnum.*(.text*)
        *cy_smif_memslot.*(.text*)
        *cy_smif_sfdp.*(.text*)
        *cy_smif_hb_flash.*(.text*)
        *mtb_hal_memoryspi.*(.text*)
        *mtb_serial_memory.*(.text*)
        *freertos/Source*.*(.text*)
        *cy_clib_support*.*(.text*)
        *cy_mutex_pool*.*(.text*)
        *cy_time.*(.text*)
        *cyabs_*.*(.text*)
        *cy_worker_thread.*(.text*)
        *cmsis*.*(.text*)

        . = ALIGN(4);
        __rt_utest_tc_tab_start = .;
        KEEP(*(UtestTcTab))
        __rt_utest_tc_tab_end = .;

        . = ALIGN(4);
        __fsymtab_start = .;
        KEEP(*(FSymTab))
        __fsymtab_end = .;
        . = ALIGN(4);
        __vsymtab_start = .;
        KEEP(*(VSymTab))
        __vsymtab_end = .;
        . = ALIGN(4);
        __rtmsymtab_start = .;
        KEEP(*(RTMSymTab))
        __rtmsymtab_end = .;

        . = ALIGN(4);
        __rt_init_start = .;
        KEEP(*(SORT(.rti_fn*)))
        __rt_init_end = .;

        . = ALIGN(4);
        __usbh_class_info_start__ = .;
        KEEP(*(.usbh_class_info))
        __usbh_class_info_end__ = .;

        . = ALIGN(4);
    } > m33_code_sel AT > m33_nvm_sel

    .ram_vectors (NOLOAD) : ALIGN(VECTORS_ALIGNMENT)
    {
        KEEP(*(.ram_vectors))
    } > m33_data

    .data : ALIGN(4)
    {
        __data_start__ = .;
        *(.data*)
        KEEP(*(.jcr*))
        . = ALIGN(4);
        __data_end__ = .;
    } > m33_data AT > m33_nvm_sel

    .bss(NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > m33_data

    .noinit (NOLOAD) :
    {
        KEEP(*(.noinit))
    } > m33_data

    .heap(NOLOAD) : ALIGN(8)
    {
        __HeapBase = .;
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
        . = ORIGIN(m33_data) + LENGTH(m33_data) - __StackSize;
        __HeapLimit = .;
    } > m33_data

    __StackTop = ORIGIN(m33_data) + LENGTH(m33_data);
    __StackLimit = __StackTop - __StackSize;
    PROVIDE(__stack = __StackTop);

    ASSERT(
        __HeapLimit > __HeapBase,
        "Error: RAM region overflowed with stack. Ensure that the stack size is within the available RAM."
    )

    .cy_sharedmem(NOLOAD) : ALIGN(4)
    {
        KEEP(*(.cy_sharedmem))
        . = ALIGN(4);
    } > m33_allocatable_shared

    .cy_shared_socmem (NOLOAD) : ALIGN(4)
    {
        KEEP(*(.cy_shared_socmem))
        . = ALIGN(4);
    } > m33_m55_shared

    .cy_trailer(NOLOAD) :
    {
    } > m33_trailer_sel
}
