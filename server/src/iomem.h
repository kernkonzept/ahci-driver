/*
 * Copyright (C) 2014-2015, 2018-2022, 2024-2025 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/dataspace>
#include <l4/re/rm>

#include <tuple>

namespace Ahci {

/**
 * Self-attaching IO memory.
 */
struct Iomem
{
  L4Re::Rm::Unique_region<l4_addr_t> vaddr;
  l4_size_t size;

  Iomem(L4::Cap<L4Re::Dataspace> iocap, l4_addr_t phys_addr, l4_size_t size)
  : size(size)
  {
    L4Re::chksys(L4Re::Env::env()->rm()->attach(&vaddr, size,
                                                L4Re::Rm::F::Search_addr
                                                | L4Re::Rm::F::Cache_uncached
                                                | L4Re::Rm::F::RW,
                                                L4::Ipc::make_cap_rw(iocap),
                                                phys_addr,
                                                L4_PAGESHIFT));
  }

  Iomem(L4::Cap<L4Re::Dataspace> iocap, std::tuple<l4_addr_t, l4_size_t> abar)
  : Iomem(iocap, std::get<0>(abar), std::get<1>(abar))
  {}

  l4_addr_t port_base_address(unsigned num) const
  {
    return vaddr.get() + Port_base + Port_size * num;
  }

  unsigned max_ports() const
  {
    unsigned ports = (size - Port_base) / Port_size;
    return cxx::min(ports, 32U);
  }

  enum Mem_config
  {
    Port_base = 0x100,
    Port_size = 0x80,
  };
};

}
