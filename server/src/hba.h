/*
 * Copyright (C) 2014-2015, 2018-2022, 2024-2025 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <l4/drivers/hw_mmio_register_block>
#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/shared_cap>
#include <l4/re/util/object_registry>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_pci>
#include <l4/cxx/minmax>

#include <array>
#include <tuple>
#include <vector>
#include <stdio.h>
#include <cassert>

#include "ahci_port.h"
#include "ahci_types.h"
#include "icu.h"
#include "iomem.h"
#include "pci.h"

namespace Ahci {

/**
 * Encapsulates one single AHCI host bridge adapter.
 *
 * Includes a server loop for handling device interrupts.
 */
class Hba : public L4::Irqep_t<Hba>
{
public:
  /**
   * Create a new AHCI HBA from a vbus PCI device.
   */
  Hba(L4vbus::Pci_dev const &dev,
      l4vbus_device_t const &di,
      cxx::Ref_ptr<Icu> icu,
      L4Re::Util::Shared_cap<L4Re::Dma_space> const &dma);

  Hba(Hba const &) = delete;
  Hba(Hba &&) = delete;

  /**
   * Return the capability register of the HBA.
   */
  Hba_features features() const { return Hba_features(_regs[Regs::Hba::Cap]); }

  /**
   * Return a pointer to the given port
   *
   * Note that a port object is always returned, even when no
   * device is attached. It is the responsibility of the caller
   * to check for the state of the port.
   *
   * \param portno Port number.
   */
  Ahci_port *port(int portno) { return &_ports[portno]; }


  /**
   * Dispatch interrupts for the HBA to the ports.
   */
  void handle_irq();

  /**
   * Register the interrupt handler with a registry.
   *
   * \param icu      ICU to request the capability for the hardware interrupt.
   * \param registry Registry that dispatches the interrupt IPCs.
   *
   * \throws L4::Runtime_error Resources are not available or accessible.
   */
  void register_interrupt_handler(L4Re::Util::Object_registry *registry);


  /**
   * Check ports for devices and initialize the ones that are found.
   *
   * \param callback Function called for each port that was successfully
   *                 initialized.
   */
  void scan_ports(std::function<void(Ahci_port *)> callback);

  int num_ports() { return _ports.size(); }

  /**
   * Test if a VBUS device is a AHCI HBA.
   *
   * \param dev      VBUS device to test.
   * \param dev_info Device information as returned by next_device()
   */
  static bool is_ahci_hba(L4vbus::Device const &dev,
                          l4vbus_device_t const &dev_info);

  /**
   * Check that address width of CPU and device are compatible.
   *
   * At the moment the HBA cannot specifically request memory below 4GB
   * from the dataspace manager. Therefore, it refuses to drive devices
   * on 64bit systems that are only capable of 32-bit addressing.
   * In practice, most systems will have their physical memory below
   * 4GB anyway, so this flag may be used to explicitly skip this check.
   */
  static bool check_address_width;
  static bool use_msis;
  static bool use_msixs;

private:
  bool msis_enabled() const
  {
    return _icu->msis_supported()
           && ((use_msixs && _pci_dev.msixs_supported())
               || (use_msis && _pci_dev.msis_supported()));
  }

  bool enable_msi(int irq, l4_icu_msi_info_t msi_info)
  {
    if (!(irq & L4::Icu::F_msi))
      return false;

    if (use_msixs && _pci_dev.msixs_supported())
      _pci_dev.enable_msix(irq, msi_info);
    else if (use_msis && _pci_dev.msis_supported())
      _pci_dev.enable_msi(irq, msi_info);

    return true;
  }

  L4::Cap<L4::Irq> bind_irq(unsigned irq, L4Re::Util::Object_registry *registry);

  void enable_irq(unsigned irq, L4::Cap<L4::Irq> cap);

  l4_uint32_t cfg_read(l4_uint32_t reg, char const *extra = "") const
  {
    return _pci_dev.cfg_read_32(reg, extra);
  }

  l4_uint16_t cfg_read_16(l4_uint32_t reg, char const *extra = "") const
  {
    return _pci_dev.cfg_read_16(reg, extra);
  }

  void cfg_write(l4_uint32_t reg, l4_uint32_t val, char const *extra = "")
  {
    _pci_dev.cfg_write_32(reg, val, extra);
  }

  void cfg_write_16(l4_uint32_t reg, l4_uint16_t val, char const *extra = "")
  {
    _pci_dev.cfg_write_16(reg, val, extra);
  }

  std::tuple<l4_addr_t, l4_size_t>
  get_abar_size(L4vbus::Pci_dev const &dev, l4vbus_device_t const &di);

  L4vbus::Pci_dev _dev;
  Pci_dev _pci_dev;
  cxx::Ref_ptr<Icu> _icu;
  Iomem _iomem;
  L4drivers::Register_block<32> _regs;
  unsigned _irq;
  unsigned char _irq_trigger_type;
  bool _unmask_via_icu;
  std::vector<Ahci_port> _ports;
};

}
