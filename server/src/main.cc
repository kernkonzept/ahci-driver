/*
 * Copyright (C) 2018 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>
#include <l4/re/util/shared_cap>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_pci>

#include "ahci_port.h"
#include "ahci_device.h"
#include "hba.h"

#include "debug.h" // needs to come before liblock-dev includes
#include <l4/libblock-device/block_device_mgr.h>
#include <l4/libblock-device/virtio_client.h>

static char const *usage_str =
"Usage: %s [-vqA] [cap,disk_id,num_ds] ...\n\n"
"Options:\n"
" -v   Verbose mode.\n"
" -q   Quite mode (do not print any warnings).\n"
" -A   Disable check for address width of device.\n"
"      Only do this if all physical memory is guaranteed to be below 4GB\n";

struct Blk_mgr
: Block_device::Device_mgr<Block_device::Virtio_client>,
  Block_device::Device_factory<Blk_mgr>
{
  Blk_mgr(L4Re::Util::Object_registry *registry)
  : Block_device::Device_mgr<Block_device::Virtio_client>(registry)
  {}
};

static Block_device::Errand::Errand_server server;
static Blk_mgr drv(server.registry());
std::vector<cxx::unique_ptr<Ahci::Hba>> _hbas;
unsigned static devices_in_scan = 0;

static int
parse_args(int argc, char *const *argv)
{
  int debug_level = 1;

  for (;;)
    {
      int opt = getopt(argc, argv, "vqA");
      if (opt == -1)
        break;

      switch (opt)
        {
        case 'v':
          debug_level <<= 1;
          ++debug_level;
          break;
        case 'q':
          debug_level = 0;
          break;
        case 'A':
          Ahci::Hba::check_address_width = false;
          break;
        default:
          Dbg::warn().printf(usage_str, argv[0]);
          return -1;
        }
    }

  Dbg::set_level(debug_level);
  return optind;
}

static void
device_scan_finished()
{
  if (--devices_in_scan > 0)
    return;

  drv.scan_finished();
  if (!server.registry()->register_obj(&drv, "svr").is_valid())
    Dbg::warn().printf("Capability 'svr' not found. No dynamic clients accepted.\n");
  else
    Dbg::trace().printf("Device now accepts new clients.\n");
}

static void
device_discovery(L4::Cap<L4vbus::Vbus> bus, L4::Cap<L4::Icu> icu,
                 L4Re::Util::Shared_cap<L4Re::Dma_space> const &dma)
{
  Dbg::info().printf("Starting device discovery.\n");

  L4vbus::Pci_dev child;

  l4vbus_device_t di;
  auto root = bus->root();

  // make sure that we don't finish device scan before the while loop is done
  ++devices_in_scan;

  while (root.next_device(&child, L4VBUS_MAX_DEPTH, &di) == L4_EOK)
    {
      Dbg::trace().printf("Scanning child 0x%lx.\n", child.dev_handle());
      if (Ahci::Hba::is_ahci_hba(child, di))
        {
          try
            {
              auto hba = cxx::make_unique<Ahci::Hba>(child, dma);
              hba->register_interrupt_handler(icu, server.registry());
              _hbas.push_back(cxx::move(hba));

            }
          catch (L4::Runtime_error const &e)
            {
              Err().printf("%s: %s\n", e.str(), e.extra_str());
              continue;
            }

          ++devices_in_scan;

          _hbas.back()->scan_ports(
            [=](Ahci::Ahci_port *port)
              {
                if (Ahci::Ahci_device::is_compatible_device(port))
                  drv.add_disk(cxx::make_ref_obj<Ahci::Ahci_device>(port),
                               device_scan_finished);
                else
                  device_scan_finished();
              });
        }
    }

  // marks the end of the device detection loop
  device_scan_finished();

  Dbg::info().printf("All devices scanned.\n");
}

static void
setup_hardware()
{
  auto vbus = L4Re::chkcap(L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus"),
                           "Get 'vbus' capability.", -L4_ENOENT);

  L4vbus::Icu icudev;
  L4Re::chksys(vbus->root().device_by_hid(&icudev, "L40009"),
               "Look for ICU device.");
  auto icu = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Icu>(),
                          "Allocate ICU capability.");
  L4Re::chksys(icudev.vicu(icu), "Request ICU capability.");

  Dbg::trace().printf("Creating DMA domain for VBUS.\n");

  auto dma = L4Re::chkcap(L4Re::Util::make_shared_cap<L4Re::Dma_space>(),
                          "Allocate capability for DMA space.");
  L4Re::chksys(L4Re::Env::env()->user_factory()->create(dma.get()),
               "Create DMA space.");

  L4Re::chksys(
      l4vbus_assign_dma_domain(vbus.cap(), -1U,
                               L4VBUS_DMAD_BIND | L4VBUS_DMAD_L4RE_DMA_SPACE,
                               dma.get().cap()),
      "Assignment of DMA domain.");

  device_discovery(vbus, icu, dma);
}

static int
run(int argc, char *const *argv)
{
  Dbg::set_level(3);

  int arg_idx = parse_args(argc, argv);
  if (arg_idx < 0)
    return arg_idx;

  Dbg::info().printf("AHCI driver says hello.\n");

  Block_device::Errand::set_server_iface(&server);
  setup_hardware();

  // add static clients as stated on the command line
  for (int i = arg_idx; i < argc; ++i)
    if (drv.add_static_client(argv[i]) < 0)
      Dbg::info().printf("Invalid client description ignored: %s", argv[i]);

  Dbg::trace().printf("Beginning server loop...\n");
  server.loop();

  return 0;
}


int
main(int argc, char *const *argv)
{
  try
    {
      return run(argc, argv);
    }
  catch (L4::Runtime_error const &e)
    {
      Err().printf("%s: %s\n", e.str(), e.extra_str());
    }
  catch (L4::Base_exception const &e)
    {
      Err().printf("Error: %s\n", e.str());
    }
  catch (std::exception const &e)
    {
      Err().printf("Error: %s\n", e.what());
    }

  return -1;
}
