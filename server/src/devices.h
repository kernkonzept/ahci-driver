/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/cxx/unique_ptr>
#include <l4/re/dma_space>
#include <string>

#include "ahci_port.h"
#include "errand.h"
#include "partition.h"

namespace Ahci {


/**
 * Structure with general information about the device.
 *
 * \note This is just an internal struct that collects information
 *       about the hardware configuration relevant for the driver.
 */
struct Device_info
{
private:
  /**
   * Layout of device info page returned by the identify device command.
   *
   * Use for translation of the info page into this Device_info structure.
   */
  enum Identify_Device_Data
  {
    IID_serialnum_ofs           = 10,
    IID_serialnum_len           = 20,
    IID_firmwarerev_ofs         = 23,
    IID_firmwarerev_len         = 8,
    IID_modelnum_ofs            = 27,
    IID_modelnum_len            = 40,
    IID_capabilities            = 49,
    IID_addressable_sectors     = 60,
    IID_ata_major_rev           = 80,
    IID_ata_minor_rev           = 81,
    IID_enabled_features        = 85,
    IID_lba_addressable_sectors = 100,
    IID_logsector_size          = 117,
  };

public:
  /** Hardware ID string.
   *
   *  For real devices the serial number, for partitions their UUID. */
  std::string hid;
  /** Serial number as reported by hardware device */
  char serial_number[IID_serialnum_len + 1];
  /** Model number as reported by hardware device */
  char model_number[IID_modelnum_len + 1];
  /** Firmware revision as reported by hardware device */
  char firmware_rev[IID_firmwarerev_len + 1];
  /** Bitfield of supported ATA major revisions. */
  l4_uint16_t ata_major_rev;
  /** ATA version implemented by the device */
  l4_uint16_t ata_minor_rev;
  /** Size of a logical sector in bytes */
  l4_size_t sector_size;
  /** Number of logical sectors */
  l4_uint64_t num_sectors;
  /** Feature bitvector */
  struct
  {
    unsigned lba : 1;      ///< Logical block addressing supported
    unsigned dma : 1;      ///< DMA supported
    unsigned longaddr : 1; ///< extended 48-bit addressing enabled
    unsigned s64a : 1;     ///< Bus supports 64bit addressing
    unsigned ro : 1;       ///< device is read=only (XXX not implemented)
  } features;

  /**
   * Fill the structure with information from the device identification page.
   *
   * \param info  Pointer to device info structure supplied by AHCI controller.
   */
  void set(l4_uint16_t const *info);

private:
  /**
   * Helper function to convert AHCI ID strings.
   *
   * param      id   Pointer to the start of the device info structure.
   * param[out] s    Pointer to where the resulting string should be stored.
   * param      ofs  Word (2-byte) offset within the device info structure from
   *                 where the ID string should be retrieved.
   * param      len  The length of the ID string in bytes.
   */
  void id2str(l4_uint16_t const *id, char *s,
              unsigned int ofs, unsigned int len);
};



/**
 * A general device that is attached to an AHCI port.
 *
 */
class Ahci_device
{
public:
  /**
   * Return a reference to the device information.
   */
  Device_info const &device_info() const { return _devinfo; }


  virtual void start_device_scan(Errand::Callback const &callback)
  {
    callback(); // noop
  }

  /**
   * Start a data transfer to or from the device.
   *
   * \param sector  Start sector for the transfer. Normally the logical sector.
   * \param data    Scatter-gather list of data segments in terms of physical
   *                memory.
   * \param sz      Number of segments in scatter-list.
   * \param cb      Optional callback to report when the transfer is finished.
   * \param flags   Transfer flags, see @Command_header_flags.
   *
   * \return L4_EOK on success or an error code otherwise.
   */
  virtual int inout_data(l4_uint64_t sector, Fis::Datablock const data[],
                         unsigned sz, Fis::Callback const &cb,
                         unsigned flags) = 0;


  /** Reset the underlying hardware device, if applicable. */
  virtual void reset_device() = 0;

  /** Create a new device that will be attached to a specific port.
   *
   * \param port  AHCI port the device is connected to.
   */
  static Ahci_device *create_device(Ahci_port *port);

  /**
   * Return the size of the device in bytes.
   */
  l4_uint64_t capacity() const
  {
    return _devinfo.num_sectors * _devinfo.sector_size;
  }

  /**
   * Return true if the device is only readable.
   */
  bool is_read_only() const { return _devinfo.features.ro; }

  virtual L4::Cap<L4Re::Dma_space> dma_space() = 0;

  virtual ~Ahci_device() = default;

protected:
  Device_info _devinfo;
};


/**
 * A partitioned device.
 *
 * \note Currently partitions use the same pool of command slots
 * of the underlying device but different pending queues. That means
 * that one client might still starve the other. This could be avoided
 * by implementing a fixed allocation of command slots per client.
 */
class Partitioned_device : public Ahci_device
{
public:
  /**
   * Create a new device partition.
   * \param parent  Parent device the partition is on.
   * \param pinfo   Structure with detailed partition information.
   */
  Partitioned_device(Ahci_device *parent, Partition_info const &pinfo)
  : _parent(parent), _start(pinfo.first), _size(pinfo.last - pinfo.first + 1)
  {
    if (pinfo.last < pinfo.first)
      throw L4::Runtime_error(-L4_EINVAL,
                              "Last sector of partition before first sector.");

    _devinfo = parent->device_info();
    _devinfo.num_sectors = _size;
    _devinfo.hid = std::string(pinfo.guid);
  }

  /** \copydoc Ahci_device::inout_data() */
  int inout_data(l4_uint64_t sector, Fis::Datablock const data[], unsigned sz,
                 Fis::Callback const &cb, unsigned flags)
  {
    if (sector >= _size)
      return -L4_EINVAL;

    l4_uint64_t total = 0;
    for (unsigned i = 0; i < sz; ++i)
      total += data[i].size;

    if ((total + 511) / 512 > _size - sector)
      return -L4_EINVAL;

    return _parent->inout_data(sector + _start, data, sz, cb, flags);
  }

  L4::Cap<L4Re::Dma_space> dma_space() { return _parent->dma_space(); }

  /** \copydoc Ahci_device::reset_device() */
  void reset_device()
  {
    // TODO noop because resetting might kill ops on other partitions
  }

private:
  Ahci_device *_parent;
  l4_uint64_t _start;
  l4_uint64_t _size;
};

}
