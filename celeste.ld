/*
 * Celeste Classic homebrew linker script.
 *
 * Segment 0 = RAM_EMU (entry + cold .text/.rodata/.data/.bss).
 *
 * Hot .text from celeste.o / celeste_audio.o (+ main .itcm_text) is linked at
 * ITCM VMA with LMA packed after .data in the GWHB payload. GWHB has no
 * multi-segment loader — app_main() copies the LMA blob into ITCM and
 * reserves it with itc_malloc before calling any ITCM code.
 *
 * Hot *data* stays out of ITCM (DTCM via dtc_* / RAM_EMU for assets).
 */

INCLUDE gnw_ram_emu.ld
INCLUDE gnw_itcm_core.ld

MEMORY
{
  RAM_EMU (xrw) : ORIGIN = __RAM_EMU_START__, LENGTH = __RAM_EMU_LENGTH__
  ITCM    (xrw) : ORIGIN = __ITCM_CORE_START__, LENGTH = __ITCM_CORE_LENGTH__
}

PHDRS
{
  text PT_LOAD FLAGS(5); /* PF_R | PF_X */
  data PT_LOAD FLAGS(6); /* PF_R | PF_W */
  itcm PT_LOAD FLAGS(5);
}

SECTIONS
{
  .core_entry __RAM_EMU_START__ :
  {
    KEEP(*(.core_entry))
    KEEP(*(.gw_core_bridge_probe))

    . = ALIGN(4);
    /* Cold code only — hot game/APU .text is claimed by .itcm_text below.
     * With -ffunction-sections, code lives in .text.* — EXCLUDE must use
     * .text* (EXCLUDE_FILE only applies to the immediately following pattern
     * when listed as ".text .text.*"). Path wildcards do not cross '/'. */
    *(EXCLUDE_FILE(*celeste.o */celeste.o */*/celeste.o
                   *celeste_audio.o */celeste_audio.o */*/celeste_audio.o)
      .text*)
    *(.rodata*)
    *(.glue_7)
    *(.glue_7t)

    . = ALIGN(4);
    __CORE_INIT_ARRAY_START__ = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    __CORE_INIT_ARRAY_END__ = .;
  } >RAM_EMU :text

  .data :
  {
    *(.data*)
  } >RAM_EMU :data

  /* LMA cursor for the ITCM image inside the flat payload. */
  __itcm_lma_start__ = .;

  .itcm_text __ITCM_CORE_START__ : AT (__itcm_lma_start__)
  {
    . = ALIGN(4);
    __itcm_vma_start__ = .;

    *celeste.o(.text*)
    */celeste.o(.text*)
    */*/celeste.o(.text*)
    *celeste_audio.o(.text*)
    */celeste_audio.o(.text*)
    */*/celeste_audio.o(.text*)
    *(.itcm_text*)

    . = ALIGN(4);
    __itcm_vma_end__ = .;
  } >ITCM :itcm

  __itcm_lma_end__ = __itcm_lma_start__ + SIZEOF(.itcm_text);
  __CORE_ITCM_CODE_END__ = __itcm_vma_end__;
  __CORE_ITCM_BSS_END__ = __itcm_vma_end__;

  /* Resume the RAM_EMU VMA after the ITCM LMA blob for NOLOAD .bss. */
  . = __itcm_lma_end__;
  __CORE_CODE_END__ = .;

  .bss (NOLOAD) :
  {
    . = ALIGN(8);
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
  } >RAM_EMU :data

  __CORE_BSS_END__ = .;

  ASSERT(__CORE_BSS_END__ <= __RAM_EMU_END__,
         "Error: core code+data+bss overflows RAM_EMU")
  ASSERT(SIZEOF(.itcm_text) <= __ITCM_CORE_LENGTH__,
         "Error: hot .text overflows ITCM (64 KiB)")

  __exidx_start = .;
  __exidx_end = .;

  /DISCARD/ :
  {
    *(.fini_array*)
    *(.preinit_array*)
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.comment)
    *(.ARM.attributes)
  }
}
