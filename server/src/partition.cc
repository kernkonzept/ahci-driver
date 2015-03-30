/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

#include <l4/re/error_helper>
#include <l4/re/rm>

#include <cstring>
#include <ios>

#include "partition.h"
#include "devices.h"
#include "debug.h"

static Dbg trace(Dbg::Trace, "partition");

struct Gpt_header
{
  char         signature[8];
  l4_uint32_t  version;
  l4_uint32_t  header_size;
  l4_uint32_t  crc;
  l4_uint32_t  _reserved;
  l4_uint64_t  current_lba;
  l4_uint64_t  backup_lba;
  l4_uint64_t  first_lba;
  l4_uint64_t  last_lba;
  char         disk_guid[16];
  l4_uint64_t  partition_array_lba;
  l4_uint32_t  partition_array_size;
  l4_uint32_t  entry_size;
  l4_uint32_t  crc_array;
};

struct Gpt_entry
{
  unsigned char type_guid[16];
  unsigned char partition_guid[16];
  l4_uint64_t   first;
  l4_uint64_t   last;
  l4_uint64_t   flags;
  l4_uint16_t   name[36];
};


namespace Ahci {

Partition_reader::Partition_reader(Ahci_device *dev)
  : _dev(dev), _header(2 * dev->device_info().sector_size, dev->dma_space(),
                       L4Re::Dma_space::Direction::From_device)
  {}

void
Partition_reader::read(Errand::Callback const &callback)
{
  _callback = callback;

  _partitions.clear();

  // preparation: read the first two sectors
  _db = Fis::Datablock(_header.phys(), 2 * _dev->device_info().sector_size);
  read_sectors(0, &Partition_reader::get_gpt);
}


void
Partition_reader::get_gpt(int error, l4_size_t)
{
  _header.unmap();

  if (error < 0)
    {
      // can't read from device, we are done
      _callback();
      return;
    }

  // prepare reading of the table from disk
  unsigned secsz = _dev->device_info().sector_size;
  Gpt_header const *header = _header.get<Gpt_header>(secsz);

  if (strncmp(header->signature, "EFI PART", 8) != 0)
    {
      _callback();
      return;
    }

  // XXX check CRC32 of header

  info.printf("GUID partition header found with %d partitions.\n",
              header->partition_array_size);

  l4_size_t arraysz = (header->partition_array_size * header->entry_size);

  l4_size_t numsec = arraysz / secsz;
  if (arraysz & secsz)
    ++numsec;

  _parray = Phys_region(numsec * secsz, _dev->dma_space(),
                        L4Re::Dma_space::Direction::From_device);
  trace.printf("Reading GPT table @ 0x%p\n", _parray.get<void>());

  _db.addr = _parray.phys();
  _db.size = numsec * secsz;

  read_sectors(header->partition_array_lba, &Partition_reader::read_gpt);
}


void
Partition_reader::read_gpt(int error, l4_size_t)
{
  _parray.unmap();

  if (error == L4_EOK)
    {
      unsigned secsz = _dev->device_info().sector_size;
      Gpt_header const *header = _header.get<Gpt_header>(secsz);

      // XXX check CRC32 of table
      for (unsigned i = 0, off = 0;
           i < header->partition_array_size;
           ++i, off += header->entry_size)
        {
          Gpt_entry *e = _parray.get<Gpt_entry>(off);
          if (e->first > 0 && e->last >= e->first)
            {
              Partition_info inf;

              l4_uint32_t *guid = (l4_uint32_t *) e->partition_guid;

#if (__BYTE_ORDER == __BIG_ENDIAN)
#error("Big endian not implemented.")
#else
              snprintf(inf.guid, 37, "%08X-%04X-%04X-%02X%02X-"
                                     "%02X%02X%02X%02X%02X%02X",
                       guid[0], guid[1] & 0xffff, (guid[1] >> 16) & 0xffff,
                       guid[2] & 0xff, (guid[2] >> 8) & 0xff,
                       (guid[2] >> 16) & 0xff, (guid[2] >> 24) & 0xff,
                       guid[3] & 0xff, (guid[3] >> 8) & 0xff,
                       (guid[3] >> 16) & 0xff, (guid[3] >> 24) & 0xff);
#endif

              inf.first = e->first;
              inf.last = e->last;
              inf.flags = e->flags;
              trace.printf("Found partition: %16s 0x%llx - 0x%llx\n",
                           inf.guid, inf.first, inf.last);
              _partitions.push_back(inf);
            }
        }
    }

  _callback();
}


void
Partition_reader::read_sectors(l4_uint64_t sector,
                               void (Partition_reader::*func)(int, l4_size_t))
{
  using namespace std::placeholders;
  auto next = std::bind(func, this, _1, _2);

  Errand::poll(10, 10000,
               [=]()
                 {
                   int ret = _dev->inout_data(sector, &_db, 1, next, 0);
                   if (ret < 0 && ret != -L4_EBUSY)
                     _callback();
                   return ret != -L4_EBUSY;
                 },
               [=](bool ret) { if (!ret) _callback(); }
              );
}

}
