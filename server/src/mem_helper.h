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
#include <l4/re/rm>
#include <l4/re/dma_space>
#include <l4/cxx/ref_ptr>

/**
 *  Helper class that temporarily allocates memory with a physical address
 */
class Phys_region : public cxx::Ref_obj
{
public:
  Phys_region() : _paddr(0) {}
  Phys_region(l4_size_t sz, L4::Cap<L4Re::Dma_space> dma_space,
              L4Re::Dma_space::Direction dir)
  : _paddr(0)
  {
    auto lcap = L4Re::chkcap(L4Re::Util::make_auto_cap<L4Re::Dataspace>(),
                             "Out of capability memory.");

    auto *e = L4Re::Env::env();
    L4Re::chksys(e->mem_alloc()->alloc(sz, lcap.get(),
                                       L4Re::Mem_alloc::Continuous
                                       | L4Re::Mem_alloc::Pinned),
                 "Cannot allocate pinned memory.");

    L4Re::chksys(e->rm()->attach(&_region, sz, L4Re::Rm::Search_addr,
                                 L4::Ipc::make_cap_rw(lcap.get()), 0,
                                 L4_PAGESHIFT),
                 "Out of virtual memory.");

    _cap = lcap;

    map(dma_space, dir);
  }

  ~Phys_region()
  {
    if (_paddr)
      unmap();
  }

  void map(L4::Cap<L4Re::Dma_space> dma_space, L4Re::Dma_space::Direction dir)
  {
    if (_paddr)
      unmap();

    l4_size_t phys_sz = _cap->size();
    L4Re::chksys(dma_space->map(L4::Ipc::make_cap_rw(_cap.get()), 0, &phys_sz,
                                L4Re::Dma_space::Attributes::None, dir,
                                &_paddr),
                 "Unable to lock memory region for DMA.");
    if (phys_sz < (l4_size_t) _cap->size())
      L4Re::chksys(-L4_ENOMEM, "Dataspace memory not continous.");

    _dma_space = dma_space;
    _dir = dir;

  }

  void unmap()
  {
    L4Re::chksys(_dma_space->unmap(L4::Ipc::make_cap_rw(_cap.get()), 0,
                                   _cap->size(),
                                   L4Re::Dma_space::Attributes::None, _dir));
    _paddr = 0;
  }

  L4Re::Dma_space::Dma_addr phys() const { return _paddr; }

  template <class T>
  L4Re::Dma_space::Dma_addr phys_elem(unsigned idx) const
  {
    return _paddr + idx * sizeof(T);
  }


  bool is_valid() const { return _region.get(); }

  template <class T>
  T *get(unsigned offset) const
  {
    return reinterpret_cast<T *>(_region.get() + offset);
  }

  template <class T>
  T *get() const { return reinterpret_cast<T *>(_region.get()); }

  template <class T>
  T *get_elem(unsigned idx) const
  {
    return &reinterpret_cast<T *>(_region.get())[idx];
  }

  Phys_region(Phys_region const &) = delete;
  Phys_region(Phys_region &&) = delete;

  Phys_region &operator=(Phys_region &&rhs)
  {
    if (this != &rhs)
      {
        _cap = rhs._cap;
        _region = rhs._region;
        _paddr = rhs._paddr;
        _dma_space = rhs._dma_space;
        _dir = rhs._dir;
        rhs._paddr = 0;
      }

    return *this;
  }

private:
  L4Re::Util::Auto_cap<L4Re::Dataspace>::Cap _cap;
  L4Re::Rm::Auto_region<char *> _region;
  L4::Cap<L4Re::Dma_space> _dma_space;
  L4Re::Dma_space::Dma_addr _paddr;
  L4Re::Dma_space::Direction _dir;
};

