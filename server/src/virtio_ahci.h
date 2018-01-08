/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/cxx/unique_ptr>
#include <l4/re/dma_space>
#include <l4/l4virtio/server/virtio-block>

#include <queue>

#include "devices.h"

namespace Ahci {

struct Ds_info {};

/**
 * Virtio interface for the AHCI driver.
 *
 * Drives one single device using the virtio interface specification.
 * This class assumes that it is the only driver for its device. If other
 * interfaces access the same device then the behaviour is unspecified.
 */
class Virtio_ahci : public L4virtio::Svr::Block_dev<Ds_info>
{
  // a pending request contains the request proper and the prepared data block list
  struct Pending_request
  {
    std::vector<Fis::Datablock> blocks;
    cxx::unique_ptr<Request> request;
    L4Re::Dma_space::Attributes attrs;
    L4Re::Dma_space::Direction dir;
  };

public:
  /**
   * Create a new interface for an existing device.
   *
   * \param dev    Device to drive with this interface. The device must
   *               have been initialized already.
   * \param numds  Maximum number of dataspaces the client is allowed to share.
   */
  Virtio_ahci(Ahci::Ahci_device *dev, unsigned numds)
  : L4virtio::Svr::Block_dev<Ds_info>(0x44, 0x100, dev->capacity() >> 9,
                                      dev->is_read_only()),
    _ahcidev(dev)
  {
    init_mem_info(numds);
    set_seg_max(Command_table::Max_entries);
    set_size_max(0x400000); // 4MB
  }

  /**
   * Reset the hardware device driven by this interface.
   */
  void reset_device() override { _ahcidev->reset_device(); }

  bool process_request(cxx::unique_ptr<Request> &&req) override;

  void task_finished(Pending_request *preq, int error, l4_size_t sz)
  {
    // unmap from Dma_Space (ignore errors)
    for (auto const &blk : preq->blocks)
      _ahcidev->dma_space()->unmap(blk.addr, blk.size, preq->attrs, preq->dir);

    // move on to the next request
    finalize_request(cxx::move(preq->request), sz, error);
    check_pending();

    // pending request can be dropped
    cxx::unique_ptr<Pending_request> ureq(preq);
  }

  bool queue_stopped() override { return !_pending.empty(); }

private:
  int build_datablocks(Pending_request *preq);

  int inout_request(Pending_request *preq, unsigned flags);

  void check_pending()
  {
    if (_pending.empty())
        return;

    while (!_pending.empty())
      {
        auto &pending = _pending.front();
        unsigned flags = 0;
        if (pending->request->header().type == L4VIRTIO_BLOCK_T_OUT)
          flags = Fis::Chf_write;
        int ret = inout_request(pending.get(), flags);
        if (ret == -L4_EBUSY)
          return; // still no unit available, keep element in queue

        if (ret < 0)
          // on any other error, send a response to the client immediately
          finalize_request(cxx::move(pending->request), 0,
                           L4VIRTIO_BLOCK_S_IOERR);
        else
          // request has been successfully sent to hardware
          // which now has ownership of Request pointer, so release here
          pending.release();

        // remove element from queue
        _pending.pop();
      }

    // clean out requests in the virtqueue
    kick();
  }

  Ahci::Ahci_device *_ahcidev;
  std::queue<cxx::unique_ptr<Pending_request>> _pending;
};

}
