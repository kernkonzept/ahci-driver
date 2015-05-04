PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

TEST_GROUP    := ahci-driver/examples

EXTRA_TEST    := ahci_md5_sync ahci_md5_async

TEST_TARGET_ahci_md5_sync  := ahci-md5-sync
TEST_TARGET_ahci_md5_async := ahci-md5-async

TEST_EXPECTED := md5sum.output

REQUIRED_MODS := $(PKGDIR)/examples/md5sum/ahci.io io ahci-drv
MOE_CFG       := $(PKGDIR)/examples/md5sum/ahci.cfg

IMAGE_FILE    = $(OBJ_DIR)/test_ahci.img
QEMU_ARGS     = -device ahci,id=ahci0 -drive if=none,file=$(IMAGE_FILE),format=raw,id=drive-sata0-0-0 -device ide-drive,bus=ahci0.0,drive=drive-sata0-0-0,id=sata0-0-0

include $(L4DIR)/mk/test.mk

all :: $(IMAGE_FILE)

$(OBJ_DIR)/test_ahci.img:
	seq 400000 | dd bs=1024 count=2048 of=$(OBJ_DIR)/test_ahci.img
	/sbin/sgdisk -o -N 1 -u 1:88E59675-4DC8-469A-98E4-B7B021DC7FBE $(OBJ_DIR)/test_ahci.img
