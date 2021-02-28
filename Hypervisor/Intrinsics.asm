; Contains code that cannot be directly written in C, thus assembly is used.

include ksamd64.inc

	LEAF_ENTRY _str, _TEXT$00
		str word ptr [rcx]			; Store TR value
		ret
	LEAF_END _str, _TEXT$00

    LEAF_ENTRY _sldt, _TEXT$00
        sldt word ptr [rcx]         ; Store LDTR value
        ret
    LEAF_END _sldt, _TEXT$00

	LEAF_ENTRY __invept, _TEXT$00
		invept rcx, OWORD PTR[rdx]
		ret
	LEAF_END __invept, _TEXT$00

	LEAF_ENTRY __invvpid, _TEXT$00
		invvpid rcx, OWORD PTR[rdx]
		ret
	LEAF_END __invvpid, _TEXT$00

    LEAF_ENTRY _RestoreContext, _TEXT$00
        movaps  xmm0, CxXmm0[rcx]   ;
        movaps  xmm1, CxXmm1[rcx]   ;
        movaps  xmm2, CxXmm2[rcx]   ;
        movaps  xmm3, CxXmm3[rcx]   ;
        movaps  xmm4, CxXmm4[rcx]   ;
        movaps  xmm5, CxXmm5[rcx]   ;
        movaps  xmm6, CxXmm6[rcx]   ; Restore all XMM registers
        movaps  xmm7, CxXmm7[rcx]   ;
        movaps  xmm8, CxXmm8[rcx]   ;
        movaps  xmm9, CxXmm9[rcx]   ;
        movaps  xmm10, CxXmm10[rcx] ;
        movaps  xmm11, CxXmm11[rcx] ;
        movaps  xmm12, CxXmm12[rcx] ;
        movaps  xmm13, CxXmm13[rcx] ;
        movaps  xmm14, CxXmm14[rcx] ;
        movaps  xmm15, CxXmm15[rcx] ;
        ldmxcsr CxMxCsr[rcx]        ;

        mov     rax, CxRax[rcx]     ;
        mov     rdx, CxRdx[rcx]     ;
        mov     r8, CxR8[rcx]       ; Restore volatile registers
        mov     r9, CxR9[rcx]       ;
        mov     r10, CxR10[rcx]     ;
        mov     r11, CxR11[rcx]     ;

        mov     rbx, CxRbx[rcx]     ;
        mov     rsi, CxRsi[rcx]     ;
        mov     rdi, CxRdi[rcx]     ;
        mov     rbp, CxRbp[rcx]     ; Restore non volatile regsiters
        mov     r12, CxR12[rcx]     ;
        mov     r13, CxR13[rcx]     ;
        mov     r14, CxR14[rcx]     ;
        mov     r15, CxR15[rcx]     ;

        cli                         ; Disable interrupts
        push    CxEFlags[rcx]       ; Push RFLAGS on stack
        popfq                       ; Restore RFLAGS
        mov     rsp, CxRsp[rcx]     ; Restore old stack
        push    CxRip[rcx]          ; Push RIP on old stack
        mov     rcx, CxRcx[rcx]     ; Restore RCX since we spilled it
        ret                         ; Restore RIP
    LEAF_END _RestoreContext, _TEXT$00

    LEAF_ENTRY _RestoreFromLog, _TEXT$00

        mov rax, rcx                ; Store the context struct pointer in RAX

        movaps  xmm0, CxXmm0[rax]   ;
        movaps  xmm1, CxXmm1[rax]   ;
        movaps  xmm2, CxXmm2[rax]   ;
        movaps  xmm3, CxXmm3[rax]   ;
        movaps  xmm4, CxXmm4[rax]   ;
        movaps  xmm5, CxXmm5[rax]   ;
        movaps  xmm6, CxXmm6[rax]   ; Restore all XMM registers
        movaps  xmm7, CxXmm7[rax]   ;
        movaps  xmm8, CxXmm8[rax]   ;
        movaps  xmm9, CxXmm9[rax]   ;
        movaps  xmm10, CxXmm10[rax] ;
        movaps  xmm11, CxXmm11[rax] ;
        movaps  xmm12, CxXmm12[rax] ;
        movaps  xmm13, CxXmm13[rax] ;
        movaps  xmm14, CxXmm14[rax] ;
        movaps  xmm15, CxXmm15[rax] ;
        ldmxcsr CxMxCsr[rax]        ;

        mov     rcx, CxRcx[rax]     ;
        mov     rdx, CxRdx[rax]     ;
        mov     r8, CxR8[rax]       ; Restore volatile registers
        mov     r9, CxR9[rax]       ;
        mov     r10, CxR10[rax]     ;
        mov     r11, CxR11[rax]     ;

        mov     rbx, CxRbx[rax]     ;
        mov     rsi, CxRsi[rax]     ;
        mov     rdi, CxRdi[rax]     ;
        mov     rbp, CxRbp[rax]     ; Restore non volatile regsiters
        mov     r12, CxR12[rax]     ;
        mov     r13, CxR13[rax]     ;
        mov     r14, CxR14[rax]     ;
        mov     r15, CxR15[rax]     ;

        mov     r11, CxRip[rax]     ; Trash R11 as it is a volatile register, we use this to remember RIP.
        mov     rsp, CxRsp[rax]     ; Restore old stack 
        push    r11                 ; Push RIP on stack
        ret                         ; Restore RIP
    LEAF_END _RestoreFromLog, _TEXT$00

	END