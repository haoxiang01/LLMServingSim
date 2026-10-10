/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "HTSimSession.hh"
#include "HTSimTopology.hh"

#include "eventlist.h"
#include "network.h"
#include "route.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace HTSim {

/**
 * Raw transport: no congestion control and no acknowledgements.
 *
 * A message is sent as back-to-back packets paced at its link's full
 * bandwidth, so alone on a link it takes `latency + packets * mtu / bandwidth`,
 * the analytical cost rounded up to whole packets. Messages sharing a link
 * still interleave in its queue and split the bandwidth. A message completes,
 * for its sender and its receiver alike, when its last packet arrives, which
 * is when the analytical backend completes both sides.
 *
 * Nothing retransmits, so buffers must be unbounded (lossless).
 */
class HTSimProtoRaw final : public HTSimProtocol {
  public:
    HTSimProtoRaw(const HTSimConfig& config, EventList& eventlist);
    ~HTSimProtoRaw() override;

    void start_flow(flowid_t flow_id, int src, int dst, uint64_t size) override;
    std::string summary() const override;

  private:
    class Source;
    class Sink;

    struct Message {
        std::unique_ptr<PacketFlow> flow;
        std::unique_ptr<Route> route;
        std::unique_ptr<Sink> sink;
        std::unique_ptr<Source> source;
    };

    /// Called by a message's sink when its last packet arrives.
    void arrived(flowid_t flow_id);

    EventList& eventlist;
    HTSimTopology topology;
    int mtu;
    std::unordered_map<flowid_t, Message> active;
    // Messages whose last packet arrived. The newest is still on the call
    // stack while it completes, so they are freed at the next arrival.
    std::vector<Message> finished;
    uint64_t completed = 0;
};

}  // namespace HTSim
