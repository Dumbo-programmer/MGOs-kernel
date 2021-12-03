ENTRY(eirEntry)

SECTIONS {
	. = 0x100000;

	.text : ALIGN(0x1000) {
		(.header*)
		*(.text*)
	}
	
	.trampoline : ALIGN(0x1000) {
		trampolineStart = .;
		*(.trampoline)
		. = ALIGN(0x1000);
		trampolineEnd = .;
	}

	.rodata : ALIGN(0x1000) {
		*(.rodata*)
	}
	.data : ALIGN(0x1000) {
		*(.data*)
	}
	.bss : ALIGN(0x1000) {
		*(.bss*)
		*(COMMON)
	}

	eirImageCeiling = .;
}

