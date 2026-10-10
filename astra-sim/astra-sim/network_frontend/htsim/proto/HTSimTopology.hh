/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "HTSimSession.hh"

#include "eventlist.h"
#include "pipe.h"
#include "queue.h"
#include "route.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace HTSim {

/// Time to put `bytes` on a link of `speed`, to the nearest picosecond.
simtime_picosec serialization_time(uint64_t bytes, linkspeed_bps speed);

/**
 * Point-to-point links matching the analytical backend's topology.
 *
 * Every dimension is FullyConnected. A message between two NPUs crosses one
 * dedicated link of the lowest dimension in which their addresses differ,
 * exactly the link MultiDimTopology::send charges. A link is an output queue
 * (serialization at the dimension's bandwidth, the configured buffer) and a
 * pipe (the dimension's latency). Each direction is a separate link, so data
 * and the acknowledgements of the opposite direction share a queue.
 *
 * Both are FIFO, so packets leave a link in the order they entered it, and
 * counting both ends tells when every packet that entered before some moment
 * has left (or been dropped). That is how a finished connection is known to
 * be unreachable without waiting for its links to go idle.
 */
class HTSimTopology {
  public:
    HTSimTopology(const HTSimConfig& config, EventList& eventlist);

    /// Queue and pipe from src to dst, created on first use.
    const Route& route(int src, int dst);

    /// Bandwidth of the link from src to dst.
    linkspeed_bps speed(int src, int dst) const;

    /// Packets that have entered the link from src to dst so far.
    uint64_t entered(int src, int dst) const;
    /// Packets that have left the link from src to dst, or were dropped by it.
    uint64_t passed(int src, int dst) const;

    std::size_t links_count() const { return links.size(); }
    /// Packets dropped by full output buffers so far.
    uint64_t packet_drops() const;

  private:
    /// Counts the packets crossing a point of a route and passes them on.
    class Counter final : public PacketSink {
      public:
        explicit Counter(const string& name) : name(name) {}
        void receivePacket(Packet& pkt) override {
            ++count;
            pkt.sendOn();
        }
        const string& nodename() override { return name; }
        uint64_t count = 0;

      private:
        string name;
    };

    /**
     * Upstream's Queue serializes at a whole number of picoseconds per byte,
     * truncated: about 3% fast between 100 and 900 GiB/s, and with no
     * serialization delay at all above ~931 GiB/s. This one rounds each
     * packet's time instead, within 0.5 ps.
     */
    class LinkQueue final : public Queue {
      public:
        LinkQueue(linkspeed_bps speed, mem_b maxsize, EventList& eventlist)
            : Queue(speed, maxsize, eventlist, nullptr) {}

      protected:
        void beginService() override {
            eventlist().sourceIsPendingRel(*this, serialization_time(_enqueued.back()->size(), _bitrate));
        }
    };

    struct Link {
        std::unique_ptr<Counter> entry;
        std::unique_ptr<LinkQueue> queue;
        std::unique_ptr<Pipe> pipe;
        std::unique_ptr<Counter> exit;
        std::unique_ptr<Route> route;
    };

    const Link& link(int src, int dst) const;

    int64_t key(int src, int dst) const { return static_cast<int64_t>(src) * npus_count + dst; }
    int link_dim(int src, int dst) const;

    EventList& eventlist;
    std::vector<int> npus_count_per_dim;
    std::vector<linkspeed_bps> speed_per_dim;
    std::vector<simtime_picosec> latency_per_dim;
    mem_b queue_bytes;
    int npus_count;
    std::unordered_map<int64_t, Link> links;
};

}  // namespace HTSim
