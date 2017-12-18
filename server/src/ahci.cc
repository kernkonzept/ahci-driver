/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

#include <l4/re/env>
#include <l4/re/dataspace>
#include <l4/re/error_helper>
#include <l4/re/util/cap_alloc>
#include <cstring>

#include <l4/vbus/vbus>
#include <l4/vbus/vbus_pci>

#include "ahci.h"
#include "errand.h"
#include "partition.h"

static Dbg trace(Dbg::Trace, "ahci");

namespace Ahci {


void
Ahci_virtio_driver::start_device_discovery(L4::Cap<L4vbus::Vbus> bus,
                                           L4::Cap<L4::Icu> icu)
{
  L4vbus::Pci_dev child;

  info.printf("Starting device discovery.\n");

  l4vbus_device_t di;
  auto root = bus->root();

  while (root.next_device(&child, L4VBUS_MAX_DEPTH, &di) == L4_EOK)
    {
      trace.printf("Scanning child 0x%lx.\n", child.dev_handle());
      if (Hba::is_ahci_hba(child, di))
        {
          try
            {
              cxx::unique_ptr<Hba> hba = cxx::make_unique<Hba>(child);
              hba->register_interrupt_handler(icu, _registry);
              _hbas.push_back(cxx::move(hba));

            }
          catch (L4::Runtime_error const &e)
            {
              error.printf("%s: %s\n", e.str(), e.extra_str());
              continue;
            }

          _hbas.back()->scan_ports(
            [=](Ahci_port *port)
              {
                auto d = Ahci_device::create_device(port);

                if (d)
                  {
                    auto conn = cxx::make_ref_obj<Impl::Connection>(
                                  cxx::unique_ptr<Ahci_device>(d));
                    ++_available_devices;
                    conn->start_disk_scan(
                      [=]()
                        {
                          _connpts.push_back(conn);
                          connect_static_clients(conn.get());
                        });
                  }
              });
        }
    }

  info.printf("All devices scanned.\n");
}


void
Impl::Connection::start_disk_scan(Errand::Callback const &callback)
{
  _device.get()->start_device_scan(
    [=]()
      {
        auto reader = cxx::make_ref_obj<Partition_reader>(_device.get());
        reader->read(
          [=]()
            {
              add_partitions(reader->partitions());
              callback();
            });
      });
}

long Ahci_virtio_driver::op_create(L4::Factory::Rights rights,
                                   L4::Ipc::Cap<void> &res, l4_mword_t,
                                   L4::Ipc::Varg_list_ref valist)
{
  trace.printf("Client requests connection.\n");
  if (!(rights & L4_CAP_FPAGE_S))
    return -L4_EPERM;

  L4::Ipc::Varg param = valist.next();

  if (!param.is_of<l4_mword_t>())
    return -L4_EINVAL;

  // Maximum number of dataspaces that can be registered.
  unsigned numds = param.value<l4_mword_t>();
  if (numds == 0 || numds > 256) // sanity check with arbitrary limit
    return -L4_EINVAL;

  param = valist.next();

  // Name of device. This must either be the serial number of the disk,
  // when the entire disk is requested or for partitions their UUID.
  if (!param.is_of<char const *>())
    return -L4_EINVAL;

  std::string name(param.value<char const *>(), param.length() - 1);

  Virtio_ahci *va;
  for (auto &c : _connpts)
    {
      int ret = c->create_interface_for(name, numds, &va);
      if (ret == L4_EOK)
        {
          // found the requested device
          L4::Cap<void> cap = va->register_obj(_registry);

          if (L4_UNLIKELY(!cap.is_valid()))
            {
              c->release_interface(va);
              return -L4_ENOMEM;
            }

          res = L4::Ipc::make_cap(cap, L4_CAP_FPAGE_RWSD);
          return L4_EOK;
        }

      if (ret != -L4_ENODEV)
        return ret;
    }

  return (_available_devices > _connpts.size()) ? -L4_EAGAIN : -L4_ENODEV;
}

void
Ahci_virtio_driver::connect_static_clients(Impl::Connection *con)
{
  auto it = _pending_clients.begin();

  while (it != _pending_clients.end())
    {
      trace.printf("Checking existing client %s/%s\n",
                   it->gate.c_str(), it->device_id.c_str());
      Virtio_ahci *va;
      int ret = con->create_interface_for(it->device_id, it->num_ds, &va);
      if (ret == L4_EOK)
        {
          auto cap = va->register_obj(_registry, it->gate.c_str());
          if (L4_UNLIKELY(!cap.is_valid()))
            {
              info.printf("Invalid capability '%s' for static client.\n",
                          it->gate.c_str());

              con->release_interface(va);
              ++it;
            }
          else
            it = _pending_clients.erase(it);
        }
      else
        ++it;
    }

  if (_available_devices == _connpts.size())
    if (_registry->register_obj(this, "svr") < 0)
      warn.printf("Main server capability 'svr' not found. Client factory not available.");
}

} // namespace Ahci
