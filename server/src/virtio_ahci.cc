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
Virtio_ahci::build_datablocks(Pending_request *preq)
{
  auto *req = preq->request.get();

  while (req->has_more())
    {
      Request::Data_block b;

      try
        {
          b = req->next_block();
        }
      catch (L4virtio::Svr::Bad_descriptor const &e)
        {
          return -L4_EIO;
        }

      l4_size_t off = b.mem->ds_offset() + (l4_addr_t) b.addr
                      - (l4_addr_t) b.mem->local_base();

      L4Re::Dma_space::Dma_addr phys;
      l4_size_t sz = b.len;

      long ret =
        _ahcidev->dma_space()->map(L4::Ipc::make_cap_rw(b.mem->ds()), off, &sz,
                                   preq->attrs, preq->dir, &phys);

      if (ret < 0 || sz < (l4_size_t) b.len)
        {
          info.printf("Cannot resolve physical address for 0x%zx (ret = %ld, %zu < %u.\n",
              off, ret, sz, b.len);
          return ret;
        }

      preq->blocks.push_back(Fis::Datablock(phys, b.len));
    }

  return L4_EOK;
}

int
Virtio_ahci::inout_request(Pending_request *preq, unsigned flags)
{
  l4_uint64_t sector = preq->request->header().sector
                       / (_ahcidev->device_info().sector_size >> 9);

  using namespace std::placeholders;
  auto callback = std::bind(&Virtio_ahci::task_finished, this, preq, _1, _2);

  return _ahcidev->inout_data(sector, preq->blocks.data(), preq->blocks.size(),
                              callback, flags);
}


bool
Virtio_ahci::process_request(cxx::unique_ptr<Request> &&req)
{
  trace.printf("request received: type 0x%x, sector 0x%llx\n",
               req->header().type, req->header().sector);
  unsigned flags = 0;
  switch (req->header().type)
    {
    case L4VIRTIO_BLOCK_T_OUT:
      flags |= Fis::Chf_write;
    case L4VIRTIO_BLOCK_T_IN:
      {
        auto pending = cxx::make_unique<Pending_request>();
        pending->attrs = L4Re::Dma_space::Attributes::None;
        pending->dir = req->header().type == L4VIRTIO_BLOCK_T_OUT
                         ? L4Re::Dma_space::Direction::To_device
                         : L4Re::Dma_space::Direction::From_device;
        pending->request = std::move(req);

        int ret = build_datablocks(pending.get());
        if (ret >= 0)
          ret = inout_request(pending.get(), flags);
        if (ret == -L4_EBUSY)
          {
            trace.printf("Port busy, queueing request.\n");
            _pending.push(cxx::move(pending));
            return false;
          }
        else if (ret < 0)
          {
            trace.printf("Got IO error: %d\n", ret);
            finalize_request(cxx::move(pending->request), 0, L4VIRTIO_BLOCK_S_IOERR);
          }
        else
          // request has been successfully sent to hardware
          // which now has ownership of Request pointer, so release here
          pending.release();
        break;
      }
    default:
      finalize_request(cxx::move(req), 0, L4VIRTIO_BLOCK_S_UNSUPP);
    }

  return true;
}

}
