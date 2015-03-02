PKGDIR ?= ../..
L4DIR  ?= $(PKGDIR)/../..

SYSTEMS    := x86-l4f amd64-l4f arm-l4f

TARGET = ahci-md5-sync ahci-md5-async
SRC_CC_ahci-md5-sync = main_sync.cc md5.cc
SRC_CC_ahci-md5-async = main_async.cc md5.cc

REQUIRES_LIBS  := libstdc++ cxx_libc_io cxx_io l4virtio

SRC_CC_IS_CXX11 = y

include $(L4DIR)/mk/prog.mk
