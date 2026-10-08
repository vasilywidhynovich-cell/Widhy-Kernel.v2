; Widhy OS v2 (64-bit, grafis) - Bootloader buatan sendiri, 512 byte
; Real mode: load kernel, salin font BIOS, set mode grafis 640x480 (VBE)
; lalu Protected mode -> Long mode (64-bit) -> kernel
;
; Build : nasm -f bin boot.asm -DKERNEL_SECTORS=<n> -o boot.bin
;         (opsional: -DCHECK_LM untuk cek dukungan CPU 64-bit, +~30 byte)
; Batas : kernel + bss harus < 320 KB (0x10000..0x5FFFF), karena font
;         disalin ke 0x60000.
[bits 16]
[org 0x7C00]

%ifndef KERNEL_SECTORS
  %define KERNEL_SECTORS 64
%endif

PML4 equ 0x70000
PDPT equ 0x71000
PD   equ 0x72000            ; 4 halaman PD berurutan: 0x72000..0x75FFF
FONT equ 0x60000            ; font BIOS 8x16 (4 KB)
CHUNK equ 64                ; sektor per pembacaan (32 KB, tidak melewati batas 64 KB)

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov [boot_drive], dl

    mov si, msg_boot
    call print16

%ifdef CHECK_LM
    ; ---- 0. Pastikan CPU mendukung long mode ----
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb boot_error
    mov eax, 0x80000001
    cpuid
    bt edx, 29
    jnc boot_error
%endif

    ; ---- 1. Load kernel dari disk (LBA 1..) ke 0x10000, per CHUNK sektor ----
    ; (banyak BIOS menolak >127 sektor sekaligus, dan buffer tidak boleh
    ;  melewati batas 64 KB -> dibaca bertahap)
.load:
    mov ax, [remain]
    test ax, ax
    jz .loaded
    cmp ax, CHUNK
    jbe .go
    mov ax, CHUNK
.go:
    mov [dap_cnt], ax
    sub [remain], ax
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc boot_error
    add word [dap_seg], CHUNK * 512 / 16     ; geser buffer tujuan
    add dword [dap_lba], CHUNK               ; sektor berikutnya
    jmp .load
.loaded:

    ; ---- 2. Salin font 8x16 dari ROM VGA BIOS ke 0x60000 ----
    mov ax, 0x1130
    mov bh, 0x06            ; 06 = font ROM 8x16
    int 0x10                ; hasil: ES:BP menunjuk ke font
    push es
    pop ds                  ; DS = ES
    mov si, bp              ; DS:SI = sumber
    mov ax, 0x6000
    mov es, ax              ; ES:DI = 0x6000:0000 = 0x60000
    xor di, di
    mov cx, 2048            ; 2048 word = 4096 byte = 256 huruf x 16
    cld
    rep movsw
    xor ax, ax
    mov ds, ax
    mov es, ax

    ; ---- 3. Mode grafis VBE 0x101 = 640x480, 256 warna ----
    mov ax, 0x4F01          ; ambil info mode -> disimpan di 0x8000
    mov cx, 0x101
    mov di, 0x8000
    int 0x10
    cmp ax, 0x004F
    jne boot_error
    test byte [0x8000], 0x80    ; bit 7 = linear framebuffer didukung?
    jz boot_error
    mov ax, 0x4F02          ; aktifkan mode (bit 14 = pakai linear framebuffer)
    mov bx, 0x4101
    int 0x10
    cmp ax, 0x004F
    jne boot_error

    ; ---- 4. A20 (coba BIOS dulu, lalu fast gate) ----
    mov ax, 0x2401
    int 0x15
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_start

boot_error:
    mov si, msg_err
    call print16
.hang:
    hlt
    jmp .hang

print16:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    jmp print16
.done:
    ret

; ---------------- 32-bit protected mode ----------------
[bits 32]
pm_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000

    ; Bersihkan 6 halaman tabel paging (PML4, PDPT, 4x PD)
    mov edi, PML4
    xor eax, eax
    mov ecx, 0x1800
    rep stosd

    ; PML4[0] -> PDPT ; PDPT[0..3] -> 4 buah PD  (present + writable)
    mov dword [PML4], PDPT | 3
    mov dword [PDPT],      PD | 3
    mov dword [PDPT + 8],  (PD + 0x1000) | 3
    mov dword [PDPT + 16], (PD + 0x2000) | 3
    mov dword [PDPT + 24], (PD + 0x3000) | 3

    ; 2048 entri x 2 MB = identity map 0..4 GB
    mov edi, PD
    mov eax, 0x83           ; present + writable + page size (2MB)
    mov ecx, 2048
.fill:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .fill

    mov eax, PML4
    mov cr3, eax

    mov eax, cr4            ; PAE
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080     ; EFER
    rdmsr
    or eax, 1 << 8          ; Long Mode Enable
    wrmsr

    mov eax, cr0            ; paging on -> long mode aktif
    or eax, 1 << 31
    mov cr0, eax

    jmp 0x18:long_start

; ---------------- 64-bit long mode ----------------
[bits 64]
long_start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, 0x90000
    mov rax, 0x10000
    jmp rax

; ---------------- data ----------------
boot_drive: db 0
remain:     dw KERNEL_SECTORS
msg_boot:   db "Widhy OS v2", 13, 10, 0
msg_err:    db "Boot error!", 0

align 4
dap:
    db 0x10, 0
dap_cnt: dw 0
dap_off: dw 0x0000
dap_seg: dw 0x1000
dap_lba: dq 1

align 8
gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF       ; 0x08 code 32-bit
    dq 0x00CF92000000FFFF       ; 0x10 data
    dq 0x00AF9A000000FFFF       ; 0x18 code 64-bit
gdt_end:

gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

times 510 - ($ - $$) db 0
dw 0xAA55
