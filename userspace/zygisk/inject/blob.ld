ENTRY(yz_native_resolve)
SECTIONS {
  . = 0;
  .text : { *(.text.entry) *(.text.start) *(.text .text.*) *(.rodata .rodata.*) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) *(.ARM.extab*) }
  .forbidden : { *(.data*) *(.bss*) *(.got*) *(.init*) *(.fini*) *(.tdata*) *(.tbss*) }
  ASSERT(SIZEOF(.forbidden) == 0, "native blob contains runtime storage/relocations")
}
