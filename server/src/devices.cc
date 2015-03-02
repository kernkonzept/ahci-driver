/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

#include <l4/re/error_helper>

#include <cstring>

#include "devices.h"
#include "debug.h"
#include "mem_helper.h"
#include "partition.h"

static Dbg trace(Dbg::Trace, "devices");

namespace Ahci {

namespace Ata { namespace Cmd {

// only contains commands used in this file
enum Ata_commands
{
  Id_device         = 0xec,
  Id_packet_device  = 0xa1,
  Read_dma          = 0xc8,
  Read_dma_ext      = 0x25,
  Read_sector       = 0x20,
  Read_sector_ext   = 0x24,
  Write_dma         = 0xca,
  Write_dma_ext     = 0x35,
  Write_sector      = 0x30,
  Write_sector_ext  = 0x34,
};

}

/**
 * A device that speaks the ATA protocol.
 */
class Device : public Ahci_device
{
public:
  Device(Ahci_port *port);

  /** \copydoc Ahci_device::inout_data() */
  int inout_data(l4_uint64_t sector, Fis::Datablock const data[], unsigned sz,
                 Fis::Callback const &cb, unsigned flags);

  /** \copydoc Ahci_device::start_device_scan() */
  void start_device_scan(Errand::Callback const &callback);

  /** \copydoc Ahci_device::reset_device() */
  void reset_device()
  {
    _port->reset([]{});
  }

  L4::Cap<L4Re::Dma_space> dma_space() { return _port->dma_space(); }


private:
  Ahci_port *_port;
};

} // namespace Ata


void
Device_info::set(l4_uint16_t const *info)
{
  id2str(info, serial_number, IID_serialnum_ofs, IID_serialnum_len);
  id2str(info, firmware_rev, IID_firmwarerev_ofs, IID_firmwarerev_len);
  id2str(info, model_number, IID_modelnum_ofs, IID_modelnum_len);
  ata_major_rev = info[IID_ata_major_rev];
  // normalise unreported version to 0
  if (ata_major_rev == 0xFFFF)
    ata_major_rev = 0;
  ata_minor_rev = info[IID_ata_minor_rev];

  // create HID from serial number
  for (char *end = &serial_number[IID_serialnum_len-1];
       end > serial_number;
       --end)
    {
      if (*end != ' ')
        {
          hid = std::string(serial_number, end - serial_number + 1);
          break;
        }
    }

  features.lba = info[IID_capabilities] >> 9;
  features.dma = info[IID_capabilities] >> 8;
  features.longaddr = info[IID_enabled_features + 1] >> 10;
  // XXX where is the read-only bit hiding again?
  features.ro = 0;


  sector_size = 2 * (l4_size_t(info[IID_logsector_size + 1]) << 16
                     | l4_size_t(info[IID_logsector_size]));
  if (sector_size < 512)
    sector_size = 512;
  if (features.longaddr)
    num_sectors = (l4_uint64_t(info[IID_lba_addressable_sectors + 2]) << 32)
                  | (l4_uint64_t(info[IID_lba_addressable_sectors + 1]) << 16)
                  | l4_uint64_t(info[IID_lba_addressable_sectors]);
  else
    num_sectors = (l4_uint64_t(info[IID_addressable_sectors + 1]) << 16)
                  | l4_uint64_t(info[IID_addressable_sectors]);
}


void
Device_info::id2str(l4_uint16_t const *id, char *s,
                    unsigned int ofs, unsigned int len)
{
  unsigned int c;

  for (; len > 0; --len, ++ofs)
    {
      c = id[ofs] >> 8;
      s[0] = c;
      c = id[ofs] & 0xff;
      s[1] = c;
      s += 2;
    }
  s[0] = 0;
}


Ahci_device *
Ahci_device::create_device(Ahci_port *port)
{
  switch (port->device_type())
    {
    case Ahci_port::Ahcidev_ata:
      return new Ata::Device(port);
    default:
      // ignore unknown device type
      return 0;
    }
}


Ata::Device::Device(Ahci_port *port) : _port(port) {}


void
Ata::Device::start_device_scan(Errand::Callback const &callback)
{
  auto infopage
    = cxx::make_ref_obj<Phys_region>(512, dma_space(),
                                     L4Re::Dma_space::Direction::From_device);
  Fis::Datablock data(infopage->phys(), 512);
  Fis::Taskfile task;
  task.command = Ata::Cmd::Id_device;
  task.num_blocks = 1;
  task.flags = 0;
  task.icc = 0;
  task.control = 0;
  task.device = 0;

  trace.printf("Reading device info...(infopage at %p)\n", infopage->get<void>());

  auto cb = [=] (int error, l4_size_t)
              {
                infopage->unmap();
                if (error == L4_EOK)
                  {
                    _devinfo.features.s64a = _port->bus_width() == 64;
                    _devinfo.set(infopage->get<l4_uint16_t>());

                    info.printf("Serial number: <%s>\n", _devinfo.serial_number);
                    info.printf("Model number: <%s>\n", _devinfo.model_number);
                    info.printf("LBA: %s  DMA: %s\n",
                                _devinfo.features.lba ? "yes": "no",
                                _devinfo.features.dma ? "yes": "no");
                    info.printf("Number of sectors: %llu sector size: %u\n",
                                _devinfo.num_sectors, _devinfo.sector_size);
                  }
                callback();
              };

  // XXX should go in some kind of queue, if busy, instead of polling
  Errand::poll(10, 10000,
               [=] () mutable
                 {
                   task.data = &data;
                   int ret = _port->send_command(task, cb);
                   if (ret < 0 && ret != -L4_EBUSY)
                     callback();
                   return ret != -L4_EBUSY;
                 },
               [=] (bool ret)
                 {
                   if (!ret)
                     callback();
                 }
              );
}


int
Ata::Device::inout_data(l4_uint64_t sector, Fis::Datablock const data[],
                           unsigned sz, Fis::Callback const &cb, unsigned flags)
{
  l4_size_t numsec = 0;
  for (unsigned i = 0; i < sz; ++i)
    {
      numsec += data[i].size;

      // data blocks must write full sectors
      if (numsec % _devinfo.sector_size)
        return -L4_EINVAL;

      // check that 32bit devices get only 32bit addresses
      if ((sizeof(l4_addr_t) == 8) && !_devinfo.features.s64a
          && (sector >= 0x100000000L))
        return -L4_EINVAL;
    }

  numsec /= _devinfo.sector_size;

  if (_devinfo.features.longaddr)
    {
      if (numsec <= 0 || numsec > 65536 || sector > ((l4_uint64_t)1 << 48))
        return -L4_EINVAL;

      if (numsec == 65536)
        numsec = 0;
    }
  else
    {
      if (numsec <= 0 || numsec > 256 || sector > (1 << 28))
        return -L4_EINVAL;

      if (numsec == 256)
        numsec = 0;
    }

  Fis::Taskfile task;

  if (flags & Fis::Chf_write)
    {
      if (_devinfo.features.dma)
        task.command = _devinfo.features.longaddr ? Ata::Cmd::Write_dma
                                                  : Ata::Cmd::Write_dma_ext;
      else
        task.command = _devinfo.features.longaddr ? Ata::Cmd::Write_sector
                                                  : Ata::Cmd::Write_sector_ext;
    }
  else
    {
      if (_devinfo.features.dma)
        task.command = _devinfo.features.longaddr ? Ata::Cmd::Read_dma
                                                  : Ata::Cmd::Read_dma_ext;
      else
        task.command = _devinfo.features.longaddr ? Ata::Cmd::Read_sector
                                                  : Ata::Cmd::Read_sector_ext;
    }

  task.lba = sector;
  task.count = numsec;
  task.device = 0x40;
  task.data = data;
  task.num_blocks = sz;
  task.icc = 0;
  task.control = 0;
  task.flags = flags;

  int ret = _port->send_command(task, cb);
  trace.printf("IO to disk starting sector %llu via slot %d\n", sector, ret);

  return (ret >= 0) ? L4_EOK : ret;
}

}
