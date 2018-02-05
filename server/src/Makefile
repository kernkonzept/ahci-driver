PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

# Big endian currently not implemented.
# Missing parts marked with preprocessor errors in code.
SYSTEMS    := x86-l4f amd64-l4f arm-l4f

TARGET = ahci-drv
SRC_CC = main.cc ahci.cc hba.cc ahci_port.cc devices.cc partition.cc \
         virtio_ahci.cc errand.cc

REQUIRES_LIBS  := libstdc++ cxx_libc_io cxx_io libio-vbus

include $(L4DIR)/mk/prog.mk
