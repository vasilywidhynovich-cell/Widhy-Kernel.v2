KERNEL_SECTORS = 320

CFLAGS = -m64 -ffreestanding -fno-pic -fno-pie -mno-red-zone -mno-sse -mno-mmx \
         -fno-stack-protector -fno-asynchronous-unwind-tables -nostdlib -O2 -Wall \
         -fno-tree-loop-distribute-patterns -fno-strict-aliasing \
         -mno-80387 -mgeneral-regs-only

all: widhy.img

disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=1

boot.bin: boot.asm Makefile
	nasm -f bin -DKERNEL_SECTORS=$(KERNEL_SECTORS) $< -o $@

kernel_entry.o: kernel_entry.asm
	nasm -f elf64 $< -o $@

isr.o: isr.asm
	nasm -f elf64 $< -o $@

kernel.o: kernel.c
	gcc $(CFLAGS) -c $< -o $@

kernel.bin: kernel_entry.o kernel.o isr.o linker.ld Makefile
	ld -m elf_x86_64 -T linker.ld --oformat binary kernel_entry.o kernel.o isr.o -o $@
	@test $$(stat -c %s $@) -le $$(( $(KERNEL_SECTORS) * 512 )) || \
	  { echo "ERROR: kernel melebihi $(KERNEL_SECTORS) sektor, naikkan KERNEL_SECTORS"; rm -f $@; exit 1; }
	truncate -s $$(( $(KERNEL_SECTORS) * 512 )) $@

widhy.img: boot.bin kernel.bin
	cat boot.bin kernel.bin > $@

run: widhy.img disk.img
	qemu-system-x86_64 \
	  -drive format=raw,file=widhy.img,if=ide,index=0 \
	  -drive format=raw,file=disk.img,if=ide,index=1

debug: widhy.img
	qemu-system-x86_64 -drive format=raw,file=widhy.img -d int -no-reboot

clean:
	rm -f *.o *.bin widhy.img

.PHONY: all run debug clean
