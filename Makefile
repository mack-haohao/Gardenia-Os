# Gardenia OS Makefile

ARCH        := x86_64
KERNEL_DIR  := kernel/linux-7.3.1
KERNEL_IMG  := $(KERNEL_DIR)/arch/x86/boot/bzImage
ROOTFS      := build/rootfs
INITRAMFS   := build/initramfs.cpio.gz
CC          := musl-gcc
CFLAGS      := -static -O2 -Wall -Wextra

#---OpenSSL---
SSL_DIR     := vendor/openssl-libs
SSL_INC     := $(SSL_DIR)/include
SSL_LIB     := $(SSL_DIR)
SSL_LIBS    := -lssl -lcrypto -lpthread


#---Fine files automaticly---

COMMON_SRCS  := $(wildcard src/librender.c)
SRCS         := $(wildcard src/*.c)

#---Special files that is not been included above---
SPECIAL_SRCS := src/init.c src/curl.c src/shell.c src/shellTools.c src/textTools.c src/librender.c


AUTO_SRCS    := $(filter-out $(SPECIAL_SRCS),$(SRCS))
AUTO_BINS    := $(patsubst src/%.c,$(ROOTFS)/bin/%,$(AUTO_SRCS))

SPECIAL_BINS := $(ROOTFS)/init \
                $(ROOTFS)/bin/gash \
                $(ROOTFS)/bin/curl \
                $(ROOTFS)/bin/coreutils \
                $(ROOTFS)/bin/textTools


# shellTools.c
CORE_APPLETS := touch mkdir pwd sleep wc head tail printf env
CORE_BINS    := $(addprefix $(ROOTFS)/bin/,$(CORE_APPLETS))

# textTools.c
TEXT_APPLETS := grep sed sort
TEXT_BINS    := $(addprefix $(ROOTFS)/bin/,$(TEXT_APPLETS))

# mount.c
MOUNT_APPLETS := umount
MOUNT_BINS    := $(addprefix $(ROOTFS)/bin/,$(MOUNT_APPLETS))

#---All bins---
ALL_BINS := $(AUTO_BINS) $(SPECIAL_BINS) $(CORE_BINS) $(TEXT_BINS) $(MOUNT_BINS)


#---make rules---

all: dirs $(ALL_BINS)
	@echo "====================================="
	@echo "Gardenia OS built successfully!"
	@echo "====================================="

$(ROOTFS)/bin/%: src/%.c $(COMMON_SRCS)
	@mkdir -p $(ROOTFS)/bin
	$(CC) $(CFLAGS) -o $@ $< $(COMMON_SRCS)

$(ROOTFS)/init: src/init.c
	@mkdir -p $(ROOTFS)
	$(CC) $(CFLAGS) -o $@ $<

$(ROOTFS)/bin/gash: src/shell.c
	@mkdir -p $(ROOTFS)/bin
	$(CC) $(CFLAGS) -o $@ $<

$(ROOTFS)/bin/curl: src/curl.c
	@mkdir -p $(ROOTFS)/bin
	$(CC) $(CFLAGS) -o $@ $< -I$(SSL_INC) -L$(SSL_LIB) $(SSL_LIBS)

$(ROOTFS)/bin/coreutils: src/shellTools.c $(COMMON_SRCS)
	@mkdir -p $(ROOTFS)/bin
	$(CC) $(CFLAGS) -o $@ $< $(COMMON_SRCS)

$(ROOTFS)/bin/textTools: src/textTools.c $(COMMON_SRCS)
	@mkdir -p $(ROOTFS)/bin
	$(CC) $(CFLAGS) -o $@ $< $(COMMON_SRCS)



$(CORE_BINS): $(ROOTFS)/bin/coreutils
	@ln -f $(ROOTFS)/bin/coreutils $@

$(TEXT_BINS): $(ROOTFS)/bin/textTools
	@ln -f $(ROOTFS)/bin/textTools $@

$(MOUNT_BINS): $(ROOTFS)/bin/mount
	@ln -f $(ROOTFS)/bin/mount $@



dirs:
	@mkdir -p $(ROOTFS)/bin $(ROOTFS)/etc $(ROOTFS)/dev \
	          $(ROOTFS)/proc $(ROOTFS)/sys $(ROOTFS)/tmp \
	          $(ROOTFS)/etc/gardenia/syntax \
	          $(ROOTFS)/etc/gardenia/completions
	@echo "nameserver 10.0.2.3" > $(ROOTFS)/etc/resolv.conf
	@echo "127.0.0.1 localhost"   > $(ROOTFS)/etc/hosts
	@echo "Copying Gardenia configs and completions..."
	@cp config/syntax/*.json $(ROOTFS)/etc/gardenia/syntax/ 2>/dev/null || true
	@cp config/completions/*.json $(ROOTFS)/etc/gardenia/completions/ 2>/dev/null || true


#---make all---
image: all
	@echo "Packing initramfs..."
	@cd $(ROOTFS) && find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > ../../$(INITRAMFS)
	@echo "Done: $(INITRAMFS)"

run: image
	@echo "Starting QEMU..."
	qemu-system-x86_64 \
		-kernel $(KERNEL_IMG) \
		-initrd $(INITRAMFS) \
		-append "console=ttyS0 rdinit=/init quiet loglevel=3 zswap.enabled=1 zswap.compressor=lz4" \
		-nographic -m 256M \
		-netdev user,id=net0 -device e1000,netdev=net0


ISO_DIR     := build/isodir
ISO_IMG     := build/gardenia.iso

#---make iso---
iso: image
	@echo "Building ISO image..."
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/grub

	@cp $(KERNEL_IMG) $(ISO_DIR)/boot/bzImage
	@cp $(INITRAMFS)  $(ISO_DIR)/boot/initramfs.cpio.gz
	
	@echo 'set timeout=0' > $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'set default=0' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo 'menuentry "Gardenia OS" {' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    linux /boot/bzImage console=tty0 earlyprintk=vga,keep ignore_loglevel nomodeset' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '    initrd /boot/initramfs.cpio.gz' >> $(ISO_DIR)/boot/grub/grub.cfg
	@echo '}' >> $(ISO_DIR)/boot/grub/grub.cfg
	
	@grub2-mkrescue -o $(ISO_IMG) $(ISO_DIR) 2>/dev/null

	@echo "ISO built successfully at $(ISO_IMG)"

#---make run-iso---
run-iso: iso
	@echo "Starting QEMU from ISO..."
	qemu-system-x86_64 \
		-cdrom $(ISO_IMG) \
		-nographic -m 256M \
		-netdev user,id=net0 -device e1000,netdev=net0

.PHONY: iso run-iso

#---make clean---
clean:
	@echo "Cleaning up..."
	rm -rf $(ROOTFS)
	rm -f $(INITRAMFS)
	@echo "Clean complete."

.PHONY: all dirs image run clean
