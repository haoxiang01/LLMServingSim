/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "HTSimProtoRaw.hh"

#include "tcppacket.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <stdexcept>

namespace HTSim {

/// Emits a message's packets one serialization time apart.
class HTSimProtoRaw::Source final : public EventSource {
  public:
    Source(EventList& eventlist,
           PacketFlow& flow,
           const Route& route,
           const uint64_t packets,
           const int mtu,
           const simtime_picosec interval)
        : EventSource(eventlist, "raw_src"),
          flow(flow),
          route(route),
          remaining(packets),
          mtu(mtu),
          interval(interval) {}

    void doNextEvent() override {
        auto* const packet = TcpPacket::newpkt(flow, route, next_seqno, mtu);
        next_seqno += mtu;
        packet->sendOn();
        if (--remaining > 0) {
            eventlist().sourceIsPendingRel(*this, interval);
        }
    }

  private:
    PacketFlow& flow;
    const Route& route;
    uint64_t remaining;
    const int mtu;
    const simtime_picosec interval;
    uint64_t next_seqno = 1;
};

/// Counts a message's packets and reports the last one.
class HTSimProtoRaw::Sink final : public PacketSink {
  public:
    Sink(HTSimProtoRaw& owner, const flowid_t flow_id, const uint64_t packets)
        : owner(owner), flow_id(flow_id), remaining(packets) {}

    void receivePacket(Packet& pkt) override {
        pkt.free();
        assert(remaining > 0);
        if (--remaining == 0) {
            owner.arrived(flow_id);
        }
    }

    const string& nodename() override {
        return name;
    }

  private:
    HTSimProtoRaw& owner;
    const flowid_t flow_id;
    uint64_t remaining;
    string name = "raw_sink";
};

HTSimProtoRaw::HTSimProtoRaw(const HTSimConfig& config, EventList& eventlist)
    : eventlist(eventlist), topology(config, eventlist), mtu(config.mtu_bytes) {
    if (config.queue_bytes != 0) {
        throw std::invalid_argument(
            "The raw HTSim transport needs unbounded buffers: nothing retransmits a dropped packet");
    }
    if (mtu <= 0) {
        throw std::invalid_argument("HTSim MTU must be positive");
    }
    Packet::set_packet_size(mtu);
}

HTSimProtoRaw::~HTSimProtoRaw() = default;

void HTSimProtoRaw::start_flow(const flowid_t flow_id,
                               const int src,
                               const int dst,
                               const uint64_t size) {
    const uint64_t packets = std::max<uint64_t>(1, (size + mtu - 1) / mtu);
    auto message = Message();
    message.flow = std::make_unique<PacketFlow>(nullptr);
    message.flow->set_flowid(flow_id);
    message.sink = std::make_unique<Sink>(*this, flow_id, packets);
    message.route = std::make_unique<Route>(topology.route(src, dst), *message.sink);
    // The pace of the link itself: alone on it, a message never queues.
    const auto interval = serialization_time(mtu, topology.speed(src, dst));
    message.source = std::make_unique<Source>(eventlist, *message.flow, *message.route, packets,
                                              mtu, interval);
    // Start from the event list rather than inside sim_send.
    EventList::sourceIsPending(*message.source, EventList::now());
    active.emplace(flow_id, std::move(message));
}

void HTSimProtoRaw::arrived(const flowid_t flow_id) {
    // Every packet of an earlier finished message has arrived and its source
    // has stopped, and none of their endpoints is on the call stack now.
    finished.clear();
    auto it = active.find(flow_id);
    assert(it != active.end());
    finished.push_back(std::move(it->second));
    active.erase(it);
    ++completed;

    auto& session = HTSimSession::instance();
    session.flow_received(flow_id);
    session.flow_sent(flow_id);
}

std::string HTSimProtoRaw::summary() const {
    auto out = std::ostringstream();
    out << "Raw: " << active.size() << " active, " << completed << " completed; "
        << topology.links_count() << " links";
    return out.str();
}

}  // namespace HTSim
