/*
 * Copyright (C) 2019 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/cxx/minmax>

#include <l4/libblock-device/part_device.h>

namespace Ahci {

class Partitioned_device : public Block_device::Partitioned_device
{
public:
  Partitioned_device(cxx::Ref_ptr<Device> const &dev, unsigned partition_id,
                     Block_device::Partition_info const &pi)
  : Block_device::Partitioned_device(dev, partition_id, pi),
    _current_in_flight(0), _max_in_flight(_parent->max_in_flight())
  {}

  unsigned max_in_flight() const override
  { return _max_in_flight; }

  int inout_data(l4_uint64_t sector, Block_device::Inout_block const &blocks,
                 Block_device::Inout_callback const &cb,
                 L4Re::Dma_space::Direction dir) override
  {
    if (_current_in_flight >= _max_in_flight)
      return -L4_EBUSY;

    ++_current_in_flight;
    return Block_device::Partitioned_device::inout_data(
             sector, blocks,
             [this, cb](int error, l4_size_t sz)
               {
                 --_current_in_flight;
                 cb(error, sz);
               }, dir);
  }

  int flush(Block_device::Inout_callback const &cb) override
  {
    if (_current_in_flight >= _max_in_flight)
      return -L4_EBUSY;

    ++_current_in_flight;

    return Block_device::Partitioned_device::flush(
             [this, cb](int error, l4_size_t sz)
               {
                 --_current_in_flight;
                 cb(error, sz);
               });
  }

  /**
   * Set the number of request that may be in flight in parallel.
   *
   * \param mx  Number of parallel requests. When larger than 0, then it
   *            is considered the absolute number of slots to use. When smaller
   *            or equal 0 then all available slots but the number given will be
   *            used.
   */
  void set_max_in_flight(int mx)
  {
    if (mx > 0)
      _max_in_flight = cxx::min((unsigned)mx, _parent->max_in_flight());
    else
      _max_in_flight = cxx::max(1, (int)_parent->max_in_flight() + mx);
  }

private:
  unsigned _current_in_flight;
  unsigned _max_in_flight;
};

} // namespace Ahci