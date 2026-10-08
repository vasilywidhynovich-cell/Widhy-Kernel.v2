; isr.asm - "pintu masuk" semua interrupt.
; CPU melompat ke sini saat ada interrupt. Tugas kita: simpan register,
; panggil fungsi C (interrupt_dispatch), kembalikan register, lalu iretq.

[bits 64]
extern interrupt_dispatch

; Stub untuk interrupt TANPA error code dari CPU:
; kita push angka 0 palsu supaya bentuk stack-nya selalu sama.
%macro ISR_NOERR 1
isr%1:
    push 0              ; error code palsu
    push %1             ; nomor interrupt (vector)
    jmp isr_common
%endmacro

; Stub untuk interrupt YANG mem-push error code sendiri (CPU sudah push).
%macro ISR_ERR 1
isr%1:
    push %1             ; nomor interrupt (vector)
    jmp isr_common
%endmacro

section .text

isr_common:
    ; simpan semua register umum (64-bit tidak punya "pusha")
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, [rsp + 15*8]   ; argumen 1 untuk fungsi C = nomor interrupt
    cld                     ; aturan ABI C: direction flag harus bersih
    call interrupt_dispatch

    ; kembalikan register (urutan terbalik)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16             ; buang nomor interrupt + error code
    iretq                   ; kembali ke program yang tadi diinterupsi

; ---- 48 stub: 0-31 exception CPU, 32-47 IRQ hardware ----
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR 29
ISR_ERR 30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; Tabel alamat semua stub, dibaca oleh kernel.c untuk mengisi IDT
section .data
global isr_stub_table
isr_stub_table:
    dq isr0
    dq isr1
    dq isr2
    dq isr3
    dq isr4
    dq isr5
    dq isr6
    dq isr7
    dq isr8
    dq isr9
    dq isr10
    dq isr11
    dq isr12
    dq isr13
    dq isr14
    dq isr15
    dq isr16
    dq isr17
    dq isr18
    dq isr19
    dq isr20
    dq isr21
    dq isr22
    dq isr23
    dq isr24
    dq isr25
    dq isr26
    dq isr27
    dq isr28
    dq isr29
    dq isr30
    dq isr31
    dq isr32
    dq isr33
    dq isr34
    dq isr35
    dq isr36
    dq isr37
    dq isr38
    dq isr39
    dq isr40
    dq isr41
    dq isr42
    dq isr43
    dq isr44
    dq isr45
    dq isr46
    dq isr47
