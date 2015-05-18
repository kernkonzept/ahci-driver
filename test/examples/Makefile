PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

SYSTEMS    := x86-l4f amd64-l4f arm-l4f

TEST_GROUP    := ahci-driver/examples

EXTRA_TEST    := ahci_md5_sync ahci_md5_async

# The test image with GPT, will be created in the test setup script
IMAGE_FILE    = $(OBJ_DIR)/test_ahci.img
TEST_SETUP    = $(SRC_DIR)/create-test-disk.sh $(IMAGE_FILE)

TEST_TARGET_ahci_md5_sync  := ahci-md5-sync
TEST_TARGET_ahci_md5_async := ahci-md5-async

TEST_EXPECTED := md5sum.output

REQUIRED_MODS := $(PKGDIR)/examples/md5sum/ahci.io io ahci-drv
MOE_CFG       := $(PKGDIR)/examples/md5sum/ahci.cfg

QEMU_ARGS     = -device ahci,id=ahci0 -drive if=none,file=$(IMAGE_FILE),format=raw,id=drive-sata0-0-0 -device ide-drive,bus=ahci0.0,drive=drive-sata0-0-0,id=sata0-0-0

include $(L4DIR)/mk/test.mk

