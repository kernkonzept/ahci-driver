/*
 * Copyright (C) 2015 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

/*
 * Maps the entire device into memory by requesting as many sectors in
 * parallel as possible.
 */
#include <l4/cxx/exceptions>
#include <l4/cxx/iostream>

#include <l4/util/util.h>
#include <l4/re/util/object_registry>
#include <l4/re/util/br_manager>

#include "virtio_block.h"
#include "dma_mem.h"
#include "md5.h"

class Dbg : public L4Re::Util::Dbg
{
public:
  Dbg(unsigned long mask) : L4Re::Util::Dbg(mask, "ahci-mmap", "") {}
};


static l4_uint64_t disk_size = 0;
static l4_uint64_t next_sector = 0;
static l4_uint64_t sectors_done = 0;

static Dbg info(2);
static Dbg trace(1);

static L4virtio::Driver::Block_device c;
static Dma_region<unsigned char> data;

void finish_reading_sector(unsigned char status);

/// Request the next sector from the device.
static int read_next_sector()
{
  // All sectors read? Then return.
  if (next_sector >= disk_size)
    return -L4_ENOSYS;

  L4virtio::Driver::Block_device::Handle h;

  // Three steps to set up a request
  // 1. Create a header.
  h = c.start_request(next_sector, L4VIRTIO_BLOCK_T_IN,
                      &finish_reading_sector);
  if (!h.valid())
    {
      trace.printf("Could not write header.\n");
      return -L4_ENOMEM;
    }

  // 2. Add the payload.
  int ret = c.add_block(h, data.sector_ptr(next_sector), 512);
  if (ret != L4_EOK)
    {
      trace.printf("Could not add block\n");
      c.free_request(h);
      return ret;
    }

  // 3. Notify the device that a new request is ready.
  ret = c.send_request(h);
  if (ret != L4_EOK)
    {
      trace.printf("Could not write status block.\n");
      c.free_request(h);
      return ret;
    }

  auto &out = (next_sector % 100 == 0) ? info : trace;
  out.printf("Done writing sector %llu\n", next_sector);
  ++next_sector;

  return L4_EOK;
}

/// Callback for finished requests.
void finish_reading_sector(unsigned char status)
{
  // Request succeeded?
  if (status != L4VIRTIO_BLOCK_S_OK)
    L4Re::chksys(-L4_EIO, "Driver reports IO error. Aborting.");

  // Device still alive and kicking?
  if (c.failed())
    L4Re::chksys(-L4_EIO, "Driver failed. Aborting.");

  sectors_done++;

  trace.printf("Done sector %llu of %llu\n", sectors_done, disk_size);

  if (sectors_done == disk_size)
    {
      // If all sectors have been read, compute the MD5 sum over the
      // entire device.
      Md5_hash md5sum;
      md5sum.update(data.get(), disk_size * 512);
      info.printf("MD5SUM of device content: %s\n", md5sum.get().c_str());
      exit(0);
    }
  else
    // Otherwise send the next requests to the device.
    while (read_next_sector() == L4_EOK) {}
}


static void setup(L4Re::Util::Object_registry *registry)
{
  auto cap = L4Re::chkcap(L4Re::Env::env()->get_cap<L4virtio::Device>("dsk"),
                          "expecting disk driver at capability 'dsk'.", 0);

  info.printf("Mmap example started. Listening to cap dsk.");
  // Set up the client side of the driver.
  // No additional user space is requested because we want to
  // use a special dataspace for the disk below.
  L4virtio::Ptr<void> devaddr;
  c.setup_device(cap, 0, 0, devaddr);
  info.printf("Disk size: %llu sectors\n", c.device_config().capacity);
  c.register_server(registry);
  // As asynchronous requests are sent, a server loop will be needed for
  // receiving notifications when a request has been processed.
  registry->register_obj(&c);

  disk_size = c.device_config().capacity;
  next_sector = 0;

  // Allocate a dataspace that can hold the entire disk.
  l4_uint64_t dataaddr;
  data.alloc(disk_size * 512);
  // Register the dataspace with the driver.
  L4Re::chksys(c.register_ds(data.ds(), 0, disk_size * 512, &dataaddr));
  data.set_devaddr(dataaddr);

  // Read as many sectors as is space in the queue.
  // More sectors will be read later.
  while (read_next_sector() == L4_EOK) {}

  trace.printf("Initial loading finished.\n");
}


int main(int , char *const *)
{
  L4Re::Util::Dbg::set_level(0xfe);
  try
    {
      L4Re::Util::Registry_server<> server;
      setup(server.registry());

      server.loop();
    }
  catch (L4::Base_exception const &e)
    {
      L4::cerr << "Error: " << e << '\n';
    }
  catch (std::exception const &e)
    {
      L4::cerr << "Error: " << e.what() << '\n';
    }

  return -1;
}
