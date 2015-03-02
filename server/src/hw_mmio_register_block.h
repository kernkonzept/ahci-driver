/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Alexander Warg <alexander.warg@kernkonzept.com>
 *            Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <endian.h>

#include "hw_register_block.h"

namespace Hw {

/**
 * A memio block with 32 bit registers and little endian byte order.
 */
class Mmio_register_block_base_le
{
protected:
  l4_addr_t _base;
  l4_addr_t _shift;

public:
  explicit Mmio_register_block_base_le(l4_addr_t base = 0, l4_addr_t shift = 0)
  : _base(base), _shift(shift) {}

#if (__BYTE_ORDER == __BIG_ENDIAN)
# error "Big endian byte order not implemented."
#else
  template< typename T >
  T read(l4_addr_t reg) const
  { return *reinterpret_cast<volatile T const *>(_base + (reg << _shift)); }

  template< typename T >
  void write(T value, l4_addr_t reg) const
  { *reinterpret_cast<volatile T *>(_base + (reg << _shift)) = value; }
#endif

  void set_base(l4_addr_t base) { _base = base; }
  void set_shift(l4_addr_t shift) { _shift = shift; }
};

template< unsigned MAX_BITS = 32 >
struct Mmio_register_block_le
: Register_block_impl<Mmio_register_block_le<MAX_BITS>, MAX_BITS>,
  Mmio_register_block_base_le
{
  explicit Mmio_register_block_le(l4_addr_t base = 0, l4_addr_t shift = 0)
  : Mmio_register_block_base_le(base, shift) {}
};


}


