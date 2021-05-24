PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

# force static to avoid dependencies to stdlib-sh
# as we don't build anything in here at all
MODE = static

SYSTEMS    := x86-l4f amd64-l4f arm-l4f arm64-l4f

TEST_GROUP    := ahci-driver/examples

EXTRA_TEST    := ahci_md5_sync ahci_md5_async

# The test image with GPT, will be created in the test setup script
IMAGE_FILE    = $(OBJ_DIR)/test_ahci_$${TEST_TARGET}.img
TEST_SETUP    = $(SRC_DIR)/create-test-disk.sh $(IMAGE_FILE)

TEST_TARGET_ahci_md5_sync  := ahci-md5-sync
TEST_TARGET_ahci_md5_async := ahci-md5-async

TEST_EXPECTED := md5sum.output

TEST_TAGS     = !hardware

REQUIRED_MODULES := $(PKGDIR)/examples/md5sum/ahci.io io ahci-drv
NED_CFG       := $(PKGDIR)/examples/md5sum/ahci.cfg

QEMU_ARGS     = -device ahci,id=ahci0 \
		-drive if=none,file=$(IMAGE_FILE),format=raw,id=drive-sata0-0-0 \
		-device ide-hd,bus=ahci0.0,drive=drive-sata0-0-0,id=sata0-0-0

ifeq ($(ARCH),arm)
  # We are not able to access the 64-bit MMIO region on ARM32
  QEMU_ARGS += -machine virt,highmem=off
endif
ifneq ($(filter arm arm64,$(ARCH)),)
  REQUIRED_MODULES += $(PKGDIR)/examples/md5sum/pcie-ecam.io
endif

include $(L4DIR)/mk/test.mk

