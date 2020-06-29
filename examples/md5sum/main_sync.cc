/* SPDX-License-Identifier: GPL-2.0-only or License-Ref-kk-custom */
/*
 * Copyright (C) 2014-2020 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 */

/**
 * \file
 *
 * Simple example that computes the md5sum over an entire AHCI disk
 * found behind capability 'dsk'.
 */
#include <l4/util/util.h>

#include <l4/l4virtio/client/virtio-block>

#include "md5.h"


class Dbg : public L4Re::Util::Dbg
{
public:
  Dbg(unsigned long mask) : L4Re::Util::Dbg(mask, "ahci-md5sum", "") {}
};

struct Err : L4Re::Util::Err
{
  explicit Err(Level l = Normal) : L4Re::Util::Err(l, "ahci-mmap") {}
};


static Dbg info(2);
static Dbg trace(1);


static void run()
{

  auto cap = L4Re::chkcap(L4Re::Env::env()->get_cap<L4virtio::Device>("dsk"),
                          "expecting disk driver at capability 'dsk'.", 0);

  static_assert(L4_PAGESIZE % 512 == 0,
                "Not implemented for page sizes not a multiple of 512 bytes.");

  L4virtio::Driver::Block_device c;
  void *block;
  L4virtio::Ptr<void> devaddr;
  c.setup_device(cap, L4_PAGESIZE, &block, devaddr);

  l4_uint64_t dsksz = c.device_config().capacity;
  unsigned secperpage = L4_PAGESIZE / 512;

  info.printf("Disk size: %llu sectors (page size: %lu)\n", dsksz, L4_PAGESIZE);

  L4virtio::Driver::Block_device::Handle h;
  Md5_hash md5sum;
  l4_uint64_t pos;

  for (pos = 0; pos + secperpage < dsksz; pos += secperpage)
    {
      auto &out = (pos % 100 == 0) ? info : trace;
      out.printf("Reading sector %llu.\n", pos);
      h = c.start_request(pos, L4VIRTIO_BLOCK_T_IN, 0);
      if (!h.valid())
        L4Re::chksys(-L4_ENOMEM, "Starting new request");
      L4Re::chksys(c.add_block(h, devaddr, L4_PAGESIZE),
                   "Add receiver block");
      L4Re::chksys(c.process_request(h),
                   "Process incoming block");
      md5sum.update(static_cast<unsigned char *>(block), L4_PAGESIZE);
    }

  if (pos < dsksz)
    {
      l4_size_t remain = 512 * (dsksz - pos);
      trace.printf("Reading remaining sector %llu with size %zu.\n", pos, remain);
      h = c.start_request(pos, L4VIRTIO_BLOCK_T_IN, 0);
      if (!h.valid())
        L4Re::chksys(-L4_ENOMEM);
      L4Re::chksys(c.add_block(h, devaddr, remain));
      L4Re::chksys(c.process_request(h));
      md5sum.update(static_cast<unsigned char *>(block), remain);
    }

  printf("MD5SUM of device content: %s\n", md5sum.get().c_str());
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
