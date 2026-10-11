TARGET = shell
ifeq ($(PACK_SHELL),true)
TYPE = ps-exe

shell.bin: shell.ps-exe
	../../../ps1-packer shell.ps-exe -tload 0x80030000 -raw -o shell.bin
else
TYPE = bin
LDSCRIPT = shell.ld
endif

# AutoBleem: ab_main.c (the AutoBleem boot screen) in place of main.c (the cube); ab_assets.c is written by
# ab_mkassets.py from ab_assets/, and blip.hit (the tune) by ab_modconv.py from ab_assets/autobleem-blip.mod.
SRCS = \
../common/crt0/crt0.s \
../common/crt0/memory-c.c \
../common/crt0/memory-s.s \
../common/syscalls/printf.s \
ab_assets.c \
ab_main.c \
ab_splash.c \
blip.hit \
cdrom.c \
dcos.c \
gpu.c \
math.c \
spu.c \
../modplayer/modplayer.c \

include ../common.mk

shell_data.o: shell.bin
	$(PREFIX)-objcopy -I binary --set-section-alignment .data=4 --rename-section .data=.rodata,alloc,load,readonly,data,contents -O $(FORMAT) -B mips $< $@

blip.o: blip.hit
	$(PREFIX)-objcopy -I binary --set-section-alignment .data=4 --rename-section .data=.rodata,alloc,load,readonly,data,contents -O $(FORMAT) -B mips $< $@
