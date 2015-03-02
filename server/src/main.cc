/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#include <l4/cxx/exceptions>
#include <l4/cxx/iostream>

#include <l4/util/util.h>
#include <l4/re/util/object_registry>
#include <l4/re/util/br_manager>

#include <unistd.h>

#include "ahci.h"
#include "devices.h"
#include "virtio_ahci.h"
#include "debug.h"
#include "errand.h"

static Dbg trace(Dbg::Trace, "main");

static char const *usage_str = "Usage: %s [-v] [cap,disk_id,num_ds] ...\n";

class Loop_hooks
: public L4::Ipc_svr::Timeout_queue_hooks<Loop_hooks, L4Re::Util::Br_manager>,
  public L4::Ipc_svr::Ignore_errors
{
public:
  l4_kernel_clock_t
  now() { return l4_kip_clock(l4re_kip()); }
};


static int
parse_args(int argc, char *const *argv)
{
  for (;;)
    {
      int opt = getopt(argc, argv, "v");
      if (opt == -1)
        break;

      switch (opt)
        {
        case 'v':
          Dbg::set_level(0xff);
          break;
        default:
          info.printf(usage_str, argv[0]);
          return -1;
        }
    }

  return optind;
}

static int
add_client(Ahci::Ahci_virtio_driver *ahcidrv, char const *entry)
{
  char *sep1 = index(entry, ',');
  if (!sep1)
    {
      info.printf("Missing disk_id in static cap specification.");
      return -1;
    }

  char *sep2 = index(sep1 + 1, ',');
  if (!sep2)
    {
      info.printf("Missing number of dataspaces for static capability.");
      return -1;
    }

  int numds;
  try
    {
      numds = std::stoi(sep2 + 1);
    }
  catch (std::invalid_argument const &e)
    {
      info.printf("Cannot parse number of dataspaces in static capability.");
      return -1;
    }
  catch (std::out_of_range const &e)
    {
      info.printf("Number of dataspaces out of range in static capability.");
      return -1;
    }

  if (numds <= 0 || numds > 255)
    {
      info.printf("Number of dataspaces out of range in static capability.");
      return -1;
    }

  std::string client(entry, sep1 - entry);
  std::string device(sep1 + 1, sep2 - sep1 - 1);

  trace.printf("Adding static client. cap: %s device: %s, numds: %i\n",
               client.c_str(), device.c_str(), numds);
  ahcidrv->add_static_client(client, device, numds);

  return L4_EOK;
}


static int
run(int argc, char *const *argv)
{
  Dbg::set_level(0xfe);

  int arg_idx = parse_args(argc, argv);
  if (arg_idx < 0)
    return arg_idx;

  warn.printf("AHCI driver says hello.\n");

  L4Re::Util::Registry_server<Loop_hooks> server;
  Ahci::Ahci_virtio_driver ahcidrv(server.registry(), "svr");
  Errand::set_server_iface(ahcidrv.server_iface());

  // add static clients as stated on the command line
  for (int i = arg_idx; i < argc; ++i)
    if (add_client(&ahcidrv, argv[i]) < 0)
      info.printf("Invalid client description ignored: %s", argv[i]);

  // set up the hardware devices
  auto vbus = L4Re::chkcap(L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus"),
                           "Error getting vm_bus capability", -L4_ENOENT);

  // XXX ICU allocation really should be wrapped in libvbus somewhere instead
  // of being duplicated all over the place.
  L4vbus::Icu icudev;
  L4Re::chksys(vbus->root().device_by_hid(&icudev, "L40009"), "requesting ICU");
  L4::Cap<L4::Icu> icu = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Icu>(),
                                      "allocating ICU cap");
  L4Re::chksys(icudev.vicu(icu), "requesting ICU cap");

  ahcidrv.start_device_discovery(vbus, icu);

  trace.printf("Beginning server loop...\n");

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
  catch (L4::Base_exception const &e)
    {
      info.printf("Error: %s", e.str());
    }
  catch (std::exception const &e)
    {
      info.printf("Error: %s", e.what());
    }

  return -1;
}
