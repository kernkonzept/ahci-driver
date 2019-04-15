/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/re/error_helper>
#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/re/util/unique_cap>
#include <l4/re/rm>
#include <l4/re/dataspace>
#include <l4/l4virtio/virtqueue>

template <typename T, l4_size_t _sector_sz = 512>
class Dma_region
{
public:
  Dma_region() : _paddr(0) {}

  Dma_region(l4_size_t sz) { alloc(sz); }

  void alloc(l4_size_t sz)
  {
    sz *= sizeof(T);
    auto lcap = L4Re::chkcap(L4Re::Util::make_unique_cap<L4Re::Dataspace>(),
                             "Out of capability memory.");

    auto *e = L4Re::Env::env();
    L4Re::chksys(e->mem_alloc()->alloc(sz, lcap.get(),
                                       L4Re::Mem_alloc::Continuous
                                       | L4Re::Mem_alloc::Pinned),
                 "Cannot allocate pinned memory.");

    L4Re::chksys(e->rm()->attach(&_region, sz,
                                 L4Re::Rm::F::Search_addr | L4Re::Rm::F::RW,
                                 L4::Ipc::make_cap_rw(lcap.get()), 0,
                                 L4_PAGESHIFT),
                 "Out of virtual memory.");

    _cap = cxx::move(lcap);
  }

  T *get() const { return _region.get(); }

  L4virtio::Ptr<void> sector_ptr(l4_uint64_t idx) const
  {
    return L4virtio::Ptr<void>(_paddr + idx * _sector_sz);
  }

  L4::Cap<L4Re::Dataspace> ds() const { return _cap.get(); }

  void set_devaddr(l4_addr_t devaddr) { _paddr = devaddr; }

private:
  L4Re::Util::Unique_cap<L4Re::Dataspace> _cap;
  L4Re::Rm::Unique_region<T *> _region;
  l4_addr_t _paddr;
};
