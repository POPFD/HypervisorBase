    .code

	VMCALL_actionHost PROC

	; RCX should hold the key (hopefully convention not broken)
	; RDX should hold a pointer to VMCALL_COMMAND parameters (same as above)
	; Need to fix it so they are ensured.
	vmcall

	; RAX will contain the result NTSTATUS in the end, set by the host.

	ret

	VMCALL_actionHost ENDP

    end