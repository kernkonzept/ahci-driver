/* SPDX-License-Identifier: GPL-2.0-only or License-Ref-kk-custom */
/*
 * Copyright (C) 2015-2020 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 */

/*
 * Maps the entire device into memory by requesting as many sectors in
 * parallel as possible.
 */
#include <l4/util/util.h>

#include "virtio_block.h"
#include "dma_mem.h"
#include "md5.h"

class Dbg : public L4Re::Util::Dbg
{
public:
  Dbg(unsigned long mask) : L4Re::Util::Dbg(mask, "ahci-mmap", "") {}
};

struct Err : L4Re::Util::Err
{
  explicit Err(Level l = Normal) : L4Re::Util::Err(l, "ahci-mmap") {}
};


static l4_uint64_t disk_size = 0;
static l4_uint64_t sectors_done = 0;

static Dbg info(2);
static Dbg trace(1);

static L4virtio::Driver::Block_device c;
static Dma_region<unsigned char> data;

void finish_reading_sector(unsigned char status);

/// Request a sector from the device.
static int read_sector(l4_uint64_t sector)
{
  L4virtio::Driver::Block_device::Handle h;

  // Three steps to set up a request
  // 1. Create a header.
  h = c.start_request(sector, L4VIRTIO_BLOCK_T_IN,
                      &finish_reading_sector);
  if (!h.valid())
    {
      trace.printf("Could not write header.\n");
      return -L4_ENOMEM;
    }

  // 2. Add the payload.
  int ret = c.add_block(h, data.sector_ptr(sector), 512);
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

  return L4_EOK;
}

/// Callback for finished requests.
void finish_reading_sector(unsigned char status)
{
  // Request succeeded?
  if (status != L4VIRTIO_BLOCK_S_OK)
    L4Re::chksys(-L4_EIO, "Driver reports IO error. Aborting.");

  // Device still alive and kicking?
  if (c.fail_state())
    L4Re::chksys(-L4_EIO, "Driver failed. Aborting.");

  sectors_done++;

  trace.printf("Done sector %llu of %llu\n", sectors_done, disk_size);

}


static void run()
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

  disk_size = c.device_config().capacity;

  // Allocate a dataspace that can hold the entire disk.
  l4_uint64_t dataaddr;
  data.alloc(disk_size * 512);
  // Register the dataspace with the driver.
  L4Re::chksys(c.register_ds(data.ds(), 0, disk_size * 512, &dataaddr));
  data.set_devaddr(dataaddr);

  for (l4_uint64_t sector = 0; sector < disk_size; ++sector)
    {
      for (;;)
        {
          int err = read_sector(sector);
          if (err >= 0)
            break;

          if (err == -L4_EAGAIN)
            {
              c.wait(0);
              c.process_used_queue();
            }
          else
            {
              L4Re::chksys(err, "Schedule sector for reading");
            }
        }

      auto &out = (sector % 100 == 0) ? info : trace;
      out.printf("Done reading sector %llu\n", sector);
    }

  trace.printf("All sectors sent.\n");

  // Now wait for the final requests to arrive
  while (sectors_done < disk_size)
    {
      c.wait(0);
      c.process_used_queue();
    }

  // After all sectors have been read,
  // compute the MD5 sum over the entire device.
  Md5_hash md5sum;
  md5sum.update(data.get(), disk_size * 512);
  info.printf("MD5SUM of device content: %s\n", md5sum.get().c_str());
}


int main(int , char *const *)
{
  L4Re::Util::Dbg::set_level(0xfe);
  try
    {
      run();

      return 0;
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
