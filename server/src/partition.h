/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <vector>
#include <string>

#include <l4/cxx/ref_ptr>

#include "mem_helper.h"
#include "ahci_types.h"
#include "errand.h"

namespace Ahci {

/**
 * Information about a single partition.
 */
struct Partition_info
{
  char           guid[37];  ///< ID of the partition.
  l4_uint64_t    first;     ///< First valid sector.
  l4_uint64_t    last;      ///< Last valid sector.
  l4_uint64_t    flags;     ///< Additional flags, depending on partition type.
};


class Ahci_device;

/**
 * Partition table reader for AHCI devices.
 */
class Partition_reader : public cxx::Ref_obj
{
public:
  Partition_reader(Ahci_device *dev);

  void read(Errand::Callback const &callback);

  void get_gpt(int error, l4_size_t processed);
  void read_gpt(int error, l4_size_t processed);


  std::vector<Partition_info> const &partitions() const { return _partitions; }

private:
  void read_sectors(l4_uint64_t sector,
                    void (Partition_reader::*func)(int, l4_size_t));

  Fis::Datablock _db;
  int _state;

  Ahci_device *_dev;
  Phys_region _header;
  Phys_region _parray;
  std::vector<Partition_info> _partitions;
  Errand::Callback _callback;
};

}
