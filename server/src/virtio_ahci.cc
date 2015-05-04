/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

#include "virtio_ahci.h"

#include <vector>

#include "debug.h"

static Dbg trace(Dbg::Trace, "virtio-ahci");

namespace Ahci {

int
Virtio_ahci::inout_request(Request *req, unsigned flags)
{
  std::vector<Fis::Datablock> blocks;

  int ret;
  for (auto &b : req->data)
    {
      l4_size_t off = b.mem->ds_offset() + (l4_addr_t) b.addr
                      - (l4_addr_t) b.mem->local_base();

      if (!b.mem->_phys)
        {
          auto ds = _ahcidev->dma_space();
          l4_size_t ds_size = b.mem->ds()->size();
          ret = ds->map(L4::Ipc::make_cap_rw(b.mem->ds()), 0, &ds_size,
                        L4Re::Dma_space::Attributes::None,
                        (flags & Fis::Chf_write)
                        ? L4Re::Dma_space::Direction::To_device
                        : L4Re::Dma_space::Direction::From_device,
                        &b.mem->_phys);
          if (ret < 0 || ds_size < (l4_size_t) b.mem->ds()->size())
            {
              b.mem->_phys = 0;
              info.printf("Cannot resolve physical address for 0x%zx (ret = %u, %zu < %zu.\n",
                          off, ret, ds_size, (l4_size_t) b.mem->ds()->size());
              return ret;
            }
        }

      blocks.push_back(Fis::Datablock(b.mem->_phys + off, b.len));
    }

  l4_uint64_t sector = req->header.sector
                       / (_ahcidev->device_info().sector_size >> 9);

  using namespace std::placeholders;
  auto callback = std::bind(&Virtio_ahci::task_finished, this, req, _1, _2);

  return _ahcidev->inout_data(sector, blocks.data(), blocks.size(),
                              callback, flags);
}


bool
Virtio_ahci::process_request(cxx::unique_ptr<Request> req)
{
  trace.printf("request received: type 0x%x, sector 0x%llx\n",
               req->header.type, req->header.sector);
  unsigned flags = 0;
  switch (req->header.type)
    {
    case L4VIRTIO_BLOCK_T_OUT:
      flags |= Fis::Chf_write;
    case L4VIRTIO_BLOCK_T_IN:
      {
        int ret = inout_request(req.get(), flags);
        if (ret == -L4_EBUSY)
          {
            trace.printf("Port busy, queueing request.\n");
            _pending.push(std::move(req));
            return false;
          }
        else if (ret < 0)
          {
            trace.printf("Got IO error: %d\n", ret);
            req->status = L4VIRTIO_BLOCK_S_IOERR;
            finalize_request(std::move(req), 0);
          }
        else
          req.release();
        break;
      }
    default:
      {
        req->status = L4VIRTIO_BLOCK_S_UNSUPP;
        finalize_request(std::move(req), 0);
      }
    }

  return true;
}

}
