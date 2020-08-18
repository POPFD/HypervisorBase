    .code

	extern Handlers_hostToGuest:proc
    extern Handlers_guestToHost:proc
    extern RtlCaptureContext:proc

	; Called when the transition to GUEST takes place. Assembly used for easy breakpointing.
	HandlerShim_hostToGuest PROC

	;int 3

	jmp Handlers_hostToGuest

	HandlerShim_hostToGuest ENDP

	; Called when a GUEST transition to HOST takes place.
    HandlerShim_guestToHost PROC

	;int 3

    push    rcx							; save the RCX register, which we spill below
    lea     rcx, [rsp+8h]				; store the context in the stack, bias for
										; the return address and the push we just did.
    call    RtlCaptureContext			; save the current register state.
										; note that this is a specially written function
										; which has the following key characteristics:
										;   1) it does not taint the value of RCX
										;   2) it does not spill any registers, nor
										;      expect home space to be allocated for it
    jmp     Handlers_guestToHost		; jump to the C code handler. we assume that it
										; compiled with optimizations and does not use
										; home space, which is true of release builds.
    HandlerShim_guestToHost ENDP

	HandlerShim_VMCALL PROC

	; RCX should hold the key (hopefully convention not broken)
	; RDX should hold the context/parameters.
	; Need to fix it so they are ensured.
	vmcall

	; RAX will contain the result NTSTATUS in the end, set by the host.

	ret

	HandlerShim_VMCALL ENDP

    end