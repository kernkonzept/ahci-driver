/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/cxx/unique_ptr>
#include <l4/re/error_helper>
#include <l4/sys/factory>
#include <l4/sys/cxx/ipc_epiface>

#include <l4/vbus/vbus>

#include <vector>
#include <algorithm>

#include "devices.h"
#include "hba.h"
#include "virtio_ahci.h"

namespace Ahci {

namespace Impl {

/**
 * A client that is waiting for a device (yet) unknown to the driver.
 */
struct Pending_client
{
  /** The name of the IPC gate assigned to the client. */
  std::string gate;
  /** Device ID requested for the client. */
  std::string device_id;
  /** Number of dataspaces to allocate. */
  int num_ds;

  Pending_client(std::string const &client, std::string const &dev_name,
                 int nds)
  : gate(client), device_id(dev_name), num_ds(nds)
  {}
};


/**
 * A connection between a device and a potential client.
 */
class Connection : public cxx::Ref_obj
{
  /** The device itself. */
  cxx::unique_ptr<Ahci_device> _device;
  /** Client interface. */
  cxx::unique_ptr<Virtio_ahci> _interface;
  /** List of partitions on the device. */
  std::vector<Connection> _subs;

  Connection(); // needed for creating partition list

  /**
   * Check if the device or one of its subdevices match the given name.
   *
   * \param name  HID to search for.
   */
  bool contains_device(std::string const &name) const
  {
    if (name.compare(_device->device_info().hid) == 0)
      return true;

    return std::any_of(_subs.begin(), _subs.end(),
                       [=](Connection const &sub)
                         { return sub.contains_device(name); });
  }


  /**
   * Create sub devices from a partition list.
   *
   * \param parts List of partition descriptions.
   *
   * For more information of partition devices, see Partitioned_device.
   */
  void add_partitions(std::vector<Partition_info> const &parts)
  {
    _subs.reserve(_subs.size() + parts.size());

    for (auto &p : parts)
      {
        Ahci_device *pdev = new Partitioned_device(_device.get(), p);
        _subs.emplace_back(Connection(cxx::unique_ptr<Ahci_device>(pdev)));
      }
  }

public:
  /**
   * Create a new connection with a device without client.
   */
  Connection(cxx::unique_ptr<Ahci_device> dev)
  : _device(cxx::move(dev)), _interface(0)
  {}

  Connection(Connection const &) = delete;

  Connection(Connection &&) = default;

  /**
   * Create a new client interface for the device with the given name.
   *
   * \param name         Name of device to look for, must match with hid.
   * \param num_ds       Maximum of dataspaces the interface should support.
   * \param virtio_iface Newly created client interface.
   *
   * \return L4_EOK or a negative error code in case of error.
   *
   * Checks itself and its partition for a match with the given name.
   * A new interface is only returned if the device is not already used.
   * If the device is already in use, then partitions cannot be reserved.
   * If any of the partitions is in use, then the device itself cannot be
   * reserved.
   */
  int create_interface_for(std::string const &name, unsigned num_ds,
                           Virtio_ahci **virtio_iface)
  {
    if (_interface)
      return contains_device(name) ? -L4_EBUSY : -L4_ENODEV;


    // check for match in partitions
    bool busy = false;
    for (auto &sub : _subs)
      {
        if (sub._interface)
          busy = true;

        int ret = sub.create_interface_for(name, num_ds, virtio_iface);
        if (ret != -L4_ENODEV)
          return ret;
      }

    // Partitions don't match, try to match device itself.
    if (name.compare(_device->device_info().hid) == 0)
      {
        if (busy)
          return -L4_EBUSY;

        _interface = cxx::make_unique<Virtio_ahci>(_device.get(), num_ds);
        *virtio_iface = _interface.get();
        return L4_EOK;
      }

    // no match found
    return -L4_ENODEV;
  }

  /**
   * Remove a client interface, effectively disconnecting the client.
   *
   * \param iface Interface to disconnect, may be for the device or one
   *              of its sub-devices.
   */
  void release_interface(Virtio_ahci *iface)
  {
    if (_interface.get() == iface)
      _interface = 0;
    else
      for (auto &sub : _subs)
        sub.release_interface(iface);
  }

  void unregister_interfaces(L4Re::Util::Object_registry *registry) const
  {
    if (_interface)
      registry->unregister_obj(_interface.get());

    for (auto &sub : _subs)
      sub.unregister_interfaces(registry);
  }

  void start_disk_scan(Errand::Callback const &callback);

};

} // namespace Impl

/**
 * AHCI driver for virtio interface.
 *
 * Implements a complete AHCI driver server with a virtio interface for
 * device communication. The server grabs and manages all AHCI devices visible
 * to it on the VBUS. It also checks for partitions on these devices and is
 * able to expose single partitions.
 *
 * This main class provides a L4::Factory interface, where clients can
 * create connections to one specific device. Devices are assigned to clients
 * in an exclusive manner. No two clients can get access to the same device at
 * the same time. This also includes partitions. If one client has access to a
 * partition of a device then no other client may connect to the complete device
 * and vice versa. (Accessing different partitions in parallel is perfectly
 * fine.)
 *
 * \note The current implementation assumes that a single dispatcher thread
 *       takes care of all requests and interrupts. In particular, using
 *       multiple threads to serve clients that use different partitions of
 *       the same device is not guaranteed to work as expected.
 */
class Ahci_virtio_driver
: public L4::Epiface_t<Ahci_virtio_driver, L4::Factory>
{
  friend class Impl::Connection;

public:
  /**
   * Create a new AHCI driver.
   *
   * \param registry Object registry that handles client requests
   *                 and interrupts.
   *
   * Registers a factory interface where new clients can be registered.
   */
  Ahci_virtio_driver(L4Re::Util::Object_registry *registry,
                     char const *server = 0)
  : _registry(registry), _available_devices(0)
  {
    // register main server (factory interface)
    if (server)
      L4Re::chkcap(registry->register_obj(this, server),
                   "main server capability not found", 0);
    else
      L4Re::chkcap(registry->register_obj(this));
  }

  ~Ahci_virtio_driver()
  {
    for (auto &c : _connpts)
      c->unregister_interfaces(_registry);

    _registry->unregister_obj(this);
  }


  /**
   * Standard factory dispatcher.
   */
  long op_create(L4::Factory::Rights rights,
                 L4::Ipc::Cap<void> &target, l4_mword_t type,
                 L4::Ipc::Varg_list_ref va);

  /**
   * Add a client that with a preallocated IPC gate that will be
   * attached immediately when the device is found.
   *
   * \param client   Name of an initial capability.
   * \param device   Hardware ID of the device to be attached.
   * \param num_ds   Number of dataspaces the client can reserve.
   */
  void add_static_client(std::string const &client,
                         std::string const &device,
                         int num_ds)
  {
    _pending_clients.emplace_back(client, device, num_ds);
  }

  /**
   * Find and map all AHCI capable devices on the given bus.
   *
   * \param bus   Virtual bus to scan for AHCI devices.
   * \param icu   Interrupt controller to request hardware interrupts from.
   *
   * This is an asynchronous function that starts the device scan and
   * then returns immediately.
   */
  void start_device_discovery(L4::Cap<L4vbus::Vbus> bus,
                              L4::Cap<L4::Icu> icu);

private:
  /**
   * Connect potentially waiting clients.
   *
   * Checks the pending clients for matches with the device and its
   * partitions. Therefore, multiple clients may be assigned.
   */
  void connect_static_clients(Impl::Connection *con);


  /** Registry new client connections subscribe to. */
  L4Re::Util::Object_registry *_registry;
  /** List of AHCI HBAs under the control of the driver. */
  std::vector<cxx::unique_ptr<Hba>> _hbas;
  /** List of devices with their potential clients. */
  std::vector<cxx::Ref_ptr<Impl::Connection>> _connpts;
  /** List of clients waiting for a device to appear. */
  std::vector<Impl::Pending_client> _pending_clients;
  /** Number of devices being scanned. */
  l4_size_t _available_devices;
};


}
