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

struct Ds_phys_info
{
  L4Re::Dma_space::Dma_addr _phys;

  Ds_phys_info() : _phys(0) {}
};

/**
 * \brief Virtio interface for the AHCI driver.
 *
 * Drives one single device using the virtio interface specification.
 * This class assumes that it is the only driver for its device. If other
 * interfaces access the same device then the behaviour is unspecified.
 */
class Virtio_ahci : public L4virtio::Svr::Block_dev<Ds_phys_info>
{
public:
  /**
   * Create a new interface for an existing device.
   *
   * \param dev    Device to drive with this interface. The device must
   *               have been initialized already.
   * \param numds Maximum number of dataspaces the client is allowed to share.
   */
  Virtio_ahci(Ahci::Ahci_device *dev, unsigned numds)
  : L4virtio::Svr::Block_dev<Ds_phys_info>(0x44, 0x100, dev->capacity() >> 9,
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

  void task_finished(Request *req, int error, l4_size_t sz)
  {
    // XXX unmap from Dma_Space
    cxx::unique_ptr<Request> ureq(req);
    finalize_request(cxx::move(ureq), sz, error);
    check_pending();
  }

  bool queue_stopped() override { return !_pending.empty(); }

private:
  int build_datablocks(Request *req, std::vector<Fis::Datablock> *blocks);

  int inout_request(Request *req, std::vector<Fis::Datablock> const &blocks,
                    unsigned flags);

  void check_pending()
  {
    if (_pending.empty())
        return;

    while (!_pending.empty())
      {
        auto &pending = _pending.front();
        auto req = pending.request.get();
        unsigned flags = 0;
        if (req->header().type == L4VIRTIO_BLOCK_T_OUT)
          flags = Fis::Chf_write;
        int ret = inout_request(req, pending.blocks, flags);
        if (ret == -L4_EBUSY)
          return; // still no unit available, keep element in queue

        if (ret < 0)
          // on any other error, send a response to the client immediately
          finalize_request(cxx::move(pending.request), 0,
                           L4VIRTIO_BLOCK_S_IOERR);
        else
          // request has been successfully sent to hardware
          // which now has ownership of Request pointer, so release here
          pending.request.release();

        // remove element from queue
        _pending.pop();
      }

    // clean out requests in the virtqueue
    kick();
  }

  // a pending request contains the request proper and the prepared data block list
  struct Pending_request
  {
    cxx::unique_ptr<Request> request;
    std::vector<Fis::Datablock> blocks;
  };

  Ahci::Ahci_device *_ahcidev;
  std::queue<Pending_request> _pending;
};

}
