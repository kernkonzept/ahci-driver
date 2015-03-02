/*
 * Copyright (C) 2014 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */
#pragma once

#include <l4/cxx/unique_ptr>

#include <vector>
#include <climits>

#include <l4/l4virtio/virtio.h>
#include <l4/l4virtio/virtio_block.h>
#include <l4/l4virtio/server/l4virtio>
#include <l4/re/util/object_registry>
#include <l4/sys/cxx/ipc_epiface>

namespace L4virtio { namespace Svr {

/**
 * A request to read or write data.
 */
template<typename Ds_data>
struct Block_request
{
  enum { Header_size = sizeof(struct l4virtio_block_header_t) };

  /**
   * Single data block in a scatter-gather list
   */
  struct Data_block
  {
    /** Virtual address of the data block (in device space) */
    void *addr;
    /** Length of datablock in bytes (max 4MB) */
    l4_uint32_t len;
    /** Pointer to virtio memory descriptor */
    Driver_mem_region_t<Ds_data> *mem;
  };

  /** Type and destination information */
  l4virtio_block_header_t header;
  /** Scatter-gather list of data blocks to process */
  std::vector<Data_block> data;
  /** Status of request that will be returned to client */
  l4_uint8_t status;

  /** Original virtio request */
  Virtqueue::Request request;
  /** Pointer to memory where status will be returned */
  l4_uint8_t *device_status;

  Block_request(Virtqueue::Request req)
  : status(L4VIRTIO_BLOCK_S_OK),
    request(req)
  {}
};

struct Block_features : public Dev_config::Features
{
  Block_features() = default;
  Block_features(l4_uint32_t raw) : Dev_config::Features(raw) {}

  /** Maximum size of any single segment is in size_max. */
  CXX_BITFIELD_MEMBER( 1,  1, size_max, raw);
  /** Maximum number of segments in a request is in seg_max. */
  CXX_BITFIELD_MEMBER( 2,  2, seg_max, raw);
  /** Disk-style geometry specified in geometry. */
  CXX_BITFIELD_MEMBER( 4,  4, geometry, raw);
  /** Device is read-only. */
  CXX_BITFIELD_MEMBER( 5,  5, ro, raw);
  /** Block size of disk is in blk_size. */
  CXX_BITFIELD_MEMBER( 6,  6, blk_size, raw);
  /** Device exports information about optimal IO alignment. */
  CXX_BITFIELD_MEMBER(10, 10, topology, raw);
};


/**
 * A general virtio block device.
 */
template <typename Ds_data>
class Block_dev
: public L4virtio::Svr::Device_t<Ds_data>,
  public L4::Epiface_t<Block_dev<Ds_data>, L4virtio::Device>
{
private:
  class Irq_object : public L4::Irqep_t<Irq_object>
  {
  public:
    Irq_object(Block_dev<Ds_data> *parent) : _parent(parent) {}

    void handle_irq()
    {
      _parent->kick();
    }

  private:
    Block_dev<Ds_data> *_parent;
  };
  Irq_object _irq_handler;

  L4Re::Util::Auto_cap<L4::Irq>::Cap _kick_guest_irq;
  Virtqueue _queue;
  unsigned _vq_max;
  l4_uint32_t _max_block_size = UINT_MAX;
  Dev_config_t<l4virtio_block_config_t> _dev_config;

public:
  typedef Block_request<Ds_data> Request;

protected:

  Block_features device_features() const
  { return _dev_config.host_features(0); }

  void set_device_features(Block_features df)
  { _dev_config.host_features(0) = df.raw; }

  /**
   * Sets the maximum size of any single segment reported to client.
   *
   * The limit is also applied to any incomming requests.
   * Requests with larger segments result in an IO error being
   * reported to the client. That means that process_request() can
   * safely make the assumption that all segments in the received
   * request are smaller.
   */
  void set_size_max(l4_uint32_t sz)
  {
    _dev_config.priv_config()->size_max = sz;
    Block_features df = device_features();
    df.size_max() = true;
    set_device_features(df);

    _max_block_size = sz;
  }

  /**
   * Sets the maximum number of segments in a request
   * that is reported to client.
   */
  void set_seg_max(l4_uint32_t sz)
  {
    _dev_config.priv_config()->seg_max = sz;
    Block_features df = device_features();
    df.seg_max() = true;
    set_device_features(df);
  }

  /**
   * Set disk geometry that is reported to the client.
   */
  void set_geometry(l4_uint16_t cylinders, l4_uint8_t heads, l4_uint8_t sectors)
  {
    l4virtio_block_config_t volatile *pc = _dev_config.priv_config();
    pc->geometry.cylinders = cylinders;
    pc->geometry.heads = heads;
    pc->geometry.sectors = sectors;
    Block_features df = device_features();
    df.geometry() = true;
    set_device_features(df);
  }

  /**
   * Sets block disk size to be reported to the client.
   *
   * Setting this does not change the logical sector size used
   * for addressing the device.
   */
  void set_blk_size(l4_uint32_t sz)
  {
    _dev_config.priv_config()->blk_size = sz;
    Block_features df = device_features();
    df.blk_size() = true;
    set_device_features(df);
  }

  /**
   * Sets the I/O alignment information reported back to the client.
   *
   * \param physical_block_exp Number of logical blocks per physical block(log2)
   * \param alignment_offset Offset of the first aligned logical block
   * \param min_io_size Suggested minimum I/O size in blocks
   * \param opt_io_size Optimal I/O size in blocks
   */
  void set_topology(l4_uint8_t physical_block_exp,
                    l4_uint8_t alignment_offset,
                    l4_uint32_t min_io_size,
                    l4_uint32_t opt_io_size)
  {
    l4virtio_block_config_t volatile *pc = _dev_config.priv_config();
    pc->topology.physical_block_exp = physical_block_exp;
    pc->topology.alignment_offset = alignment_offset;
    pc->topology.min_io_size = min_io_size;
    pc->topology.opt_io_size = opt_io_size;
    Block_features df = device_features();
    df.topology() = true;
    set_device_features(df);
  }


public:
  /**
   * Create a new virtio block device.
   *
   * \param vendor     Vendor ID
   * \param queue_size Number of entries to provide in avail and used queue.
   * \param capacity   Size of the device in 512-byte sectors.
   * \param read_only  True, if the device should not be writable.
   */
  Block_dev(l4_uint32_t vendor, unsigned queue_size,
            l4_uint64_t capacity, bool read_only)
  : L4virtio::Svr::Device_t<Ds_data>(&_dev_config),
    _irq_handler(this), _vq_max(queue_size),
    _dev_config(vendor, L4VIRTIO_ID_BLOCK, 1)
  {
    _kick_guest_irq = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Irq>());
    this->reset_queue_config(0, queue_size);

    Block_features df(0);
    df.ring_indirect_desc() = true;
    df.ro() = read_only;
    set_device_features(df);

    _dev_config.priv_config()->capacity = capacity;
  }


  /**
   * Implements the actual processing of data in the device.
   *
   * \param req  The request to be processed.
   * \return If false, no further requests will be scheduled.
   *
   * Synchronous and asynchronous processing of the data is supported.
   * For asynchronous mode, the function should set up the worker
   * and then return false. In synchronous mode, the function should
   * return true, once processing is complete. If there is an error
   * and processing is aborted, the status flag of @req needs to be set
   * accordingly and the request immediately finished with finish_request()
   * if the client is to be answered.
   */
  virtual bool process_request(cxx::unique_ptr<Request> req) = 0;

  /**
   * Reset the actual hardware device.
   */
  virtual void reset_device() = 0;

  /**
   * Return true, if the queues should not be processed further.
   */
  virtual bool queue_stopped() = 0;

  /**
   * Releases resources related to a request and notifies the client.
   *
   * This function must be called when an asynchronous request finishes,
   * either successfully or with an error. The status byte in the request
   * must have been set prior to calling it.
   *
   * \param req Pointer to request that has finished.
   */
  void finalize_request(cxx::unique_ptr<Request> req, unsigned sz = 0)
  {
    // write the status bit back
    *req->device_status = req->status;

    // now release the head
    _queue.consumed(req->request, sz);

    // XXX not implemented
    // _dev_config->irq_status |= 1;
    _kick_guest_irq->trigger();

    // Request can be dropped here.
  }


  int reconfig_queue(unsigned idx)
  {
    if (idx == 0 && this->setup_queue(&_queue, 0, _vq_max))
      return 0;

    return -L4_EINVAL;
  }

  /**
   * Attach device to an object registry.
   *
   * \param registry Object registry that will be responsible for dispatching
   *                 requests.
   * \param service  Name of an existing capability the device should use.
   *
   * This functions registers the general virtio interface as well as the
   * interrupt handler which is used for receiving client notifications.
   */
  L4::Cap<void> register_obj(L4Re::Util::Object_registry *registry,
                             char const *service = 0)
  {
    L4Re::chkcap(registry->register_irq_obj(&_irq_handler));
    L4::Cap<void> ret;
    if (service)
      ret = registry->register_obj(this, service);
    else
      ret = registry->register_obj(this);
    L4Re::chkcap(ret);

    return ret;
  }

  void load_desc(L4virtio::Virtqueue::Desc const &desc,
                 Request_processor const *proc,
                 L4virtio::Virtqueue::Desc const **table)
  {
    this->_mem_info.load_desc(desc, proc, table);
  }

  void load_desc(L4virtio::Virtqueue::Desc const &desc,
                 Request_processor const *proc,
                 typename Request::Data_block *data)
  {
    auto *region = this->_mem_info.find(desc.addr.get(), desc.len);
    if (L4_UNLIKELY(!region))
      throw Bad_descriptor(proc, Bad_descriptor::Bad_address);

    data->addr = region->local(desc.addr);
    data->len = desc.len;
    data->mem = region;
  }

protected:
  L4::Ipc_svr::Server_iface *server_iface() const
  {
    return L4::Epiface::server_iface();
  }

  void kick()
  {
    if (queue_stopped())
      return;

    Request_processor rp;
    Virtqueue::Request r;

    while (!_dev_config.status().failed())
      {
        r = _queue.next_avail();
        if (!r)
          return;

        auto current = cxx::make_unique<Request>(r);
        try
          {
            // first block should be the header
            typename Request::Data_block data;
            rp.start(this, r, &data);
            unsigned processed = 1;

            if (data.len >= Request::Header_size)
              current->header = *(static_cast<l4virtio_block_header_t *>(data.addr));
            else
              {
                throw Bad_descriptor(&rp, Bad_descriptor::Bad_size);
              }

            // if there is no space for status bit we cannot really recover
            if (!rp.has_more() && data.len == Request::Header_size)
              {
                throw Bad_descriptor(&rp, Bad_descriptor::Bad_size);
              }

            // remaining blocks contain the scatter-gather list

            while (rp.next(this, &data))
              {
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
            _dev_config.set_failed();
            _queue.consumed(r);
          }
      }
  }



private:
  L4::Cap<L4::Irq> device_notify_irq() const
  {
    return L4::cap_cast<L4::Irq>(_irq_handler.obj_cap());
  }


  void register_single_driver_irq()
  {
    _kick_guest_irq = L4Re::chkcap(server_iface()->template rcv_cap<L4::Irq>(0));
    L4Re::chksys(server_iface()->realloc_rcv_cap(0));
  }


  void reset()
  {
    _queue.disable();
    reset_device();
  }

  bool check_queues()
  {
    if (!_queue.ready())
      {
        reset();
        return false;
      }

    return true;
  }
};

} }
