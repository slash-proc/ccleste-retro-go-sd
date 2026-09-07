/*
 * Celeste Classic homebrew linker script.
 *
 * Segment 0 = RAM_EMU (entry + cold .text/.rodata/.data/.bss).
 * Segment 1 = ITCM (.core_itcm) — hot .text only; firmware loads it via the
 *             same GWHB segments[] path as CORE (pack_homebrew / pack_core).
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
    /* Cold code — hot game/APU .text is claimed by .core_itcm.
     * EXCLUDE_FILE applies to one pattern; use .text* (function-sections).
     * Path wildcards do not cross '/'. */
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

  . = ALIGN(8);
  .data :
  {
    *(.data*)
  } >RAM_EMU :data

  /* Segment 0 file payload ends here (ITCM is a separate GWHB segment). */
  __CORE_CODE_END__ = .;

  . = ALIGN(8);
  .bss (NOLOAD) :
  {
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
  } >RAM_EMU :data

  __CORE_BSS_END__ = .;

  ASSERT(__CORE_BSS_END__ <= __RAM_EMU_END__,
         "Error: homebrew RAM_EMU (code+data+bss) overflows RAM_EMU")

  /*
   * Segment 1: ITCM hot code. Firmware memcpy's this blob to
   * __ITCM_CORE_START__ before jumping to app_main (no self-copy).
   *
   * 0x100 trap pad keeps real functions off address 0 (NULL BX → ARM mode
   * HardFault is otherwise indistinguishable from P8music@0).
   */
  .core_itcm __ITCM_CORE_START__ :
  {
    FILL(0xe7fee7fe)
    . = . + 0x100;
    FILL(0)

    *celeste.o(.text*)
    */celeste.o(.text*)
    */*/celeste.o(.text*)
    *celeste_audio.o(.text*)
    */celeste_audio.o(.text*)
    */*/celeste_audio.o(.text*)
    *(.itcm_text*)

    . = ALIGN(4);
    __CORE_ITCM_CODE_END__ = .;
  } >ITCM AT>RAM_EMU :itcm

  __CORE_ITCM_BSS_END__ = __CORE_ITCM_CODE_END__;

  ASSERT(__CORE_ITCM_BSS_END__ <= __ITCM_CORE_START__ + __ITCM_CORE_LENGTH__,
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
