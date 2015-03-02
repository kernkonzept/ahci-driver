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

#include <queue>

#include "virtio_block.h"
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
  typedef L4virtio::Svr::Block_request<Ds_phys_info> Request;
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
  void reset_device() { _ahcidev->reset_device(); }

  bool process_request(cxx::unique_ptr<Request> req);

  void task_finished(Request *req, int error, l4_size_t sz)
  {
    // XXX unmap from Dma_Space
    cxx::unique_ptr<Request> ureq(req);
    ureq->status = error;
    finalize_request(std::move(ureq), sz);
    check_pending();
  }

  bool queue_stopped() { return !_pending.empty(); }

private:
  int inout_request(Request *req, unsigned flags);

  void check_pending()
  {
    if (_pending.empty())
        return;

    while (!_pending.empty())
      {
        auto req = _pending.front().get();
        unsigned flags = 0;
        if (req->header.type == L4VIRTIO_BLOCK_T_OUT)
          flags = Fis::Chf_write;
        int ret = inout_request(req, flags);
        if (ret == -L4_EBUSY)
          return; // still no unit available
        else
          {
            // clean up first element from queue
            auto ureq = std::move(_pending.front());
            _pending.pop();
            if (ret < 0)
              {
                req->status = L4VIRTIO_BLOCK_S_IOERR;
                finalize_request(std::move(ureq));
              }
            else
              ureq.release();
          }
      }

    // clean out requests in the virtqueue
    kick();
  }


  Ahci::Ahci_device *_ahcidev;
  std::queue<cxx::unique_ptr<Request>> _pending;
};

}
