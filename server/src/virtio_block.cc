/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

#include <l4/l4virtio/virtio_block.h>

#include "virtio_block.h"
#include "debug.h"

static Dbg trace(Dbg::Trace, "virtio-block");

namespace L4virtio { namespace Svr {

void
Block_dev::kick()
{
  if (queue_stopped())
    return;

  trace.printf("KICK.\n");
  Request_processor rp;
  Virtqueue::Request r;

  while (!_dev_config.status().failed())
    {
      r = _queue.next_avail();
      if (!r)
        return;

      auto current = cxx::make_unique<Block_request>(r);
      try
        {
          trace.printf("Reading next block\n");
          // first block should be the header
          Block_request::Data_block data;
          rp.start(this, r, &data);
          unsigned processed = 1;

          if (data.len >= Block_request::Header_size)
            current->header = *(static_cast<l4virtio_block_header_t *>(data.addr));
          else
            {
              warn.printf("Header is of bad length\n");
              throw Bad_descriptor(&rp, Bad_descriptor::Bad_size);
            }

          // if there is no space for status bit we cannot really recover
          if (!rp.has_more() && data.len == Block_request::Header_size)
            {
              warn.printf("Cannot find status bit.\n");
              throw Bad_descriptor(&rp, Bad_descriptor::Bad_size);
            }

          // remaining blocks contain the scatter-gather list

          while (rp.next(this, &data))
            {
              trace.printf("Datablock @%p (sz: 0x%x, offset: 0x%lx)\n",
                           data.addr, data.len, data.ds_offset);

              if (data.len > _max_block_size)
                current->status = L4VIRTIO_BLOCK_S_UNSUPP;

              if (!rp.has_more())
                --data.len;

              // cannot exceed queue size with a single request
              if (++processed > _vq_max)
                throw Bad_descriptor(&rp, Bad_descriptor::Bad_size);

              // in case of error skip over blocks to status bit
              if (current->status == L4VIRTIO_BLOCK_S_OK && data.len > 0)
                current->data.push_back(data);
            }

          // and let the status bit point to the last byte of the last block
          // Remember that data.len is already one short at this point.
          current->device_status = static_cast<l4_uint8_t *>(data.addr)
                                   + data.len;

          if (current->status != L4VIRTIO_BLOCK_S_OK)
            finalize_request(std::move(current));
          else if (!process_request(std::move(current)))
            return;
        }
      catch (Bad_descriptor const &e)
        {
          warn.printf("Bad descriptor received (%d).\n", e.error);
          _dev_config.set_failed();
          _queue.consumed(r);
        }
    }
}


void
Block_dev::finalize_request(cxx::unique_ptr<Block_request> req, unsigned sz)
{
  trace.printf("%llu finalised with status %d.\n",
               req->header.sector, req->status);
  // write the status bit back
  *req->device_status = req->status;

  // now release the head
  _queue.consumed(req->request, sz);

  // XXX not implemented
  // _dev_config->irq_status |= 1;
  _kick_guest_irq->trigger();

  // Request can be dropped here.
}


} }
