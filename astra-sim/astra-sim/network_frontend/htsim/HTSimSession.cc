/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "HTSimSession.hh"
#include "HTSimProtoRaw.hh"
#include "HTSimProtoTcp.hh"

#include "config.h"
#include "eventlist.h"

#include <cassert>
#include <queue>
#include <stdexcept>

namespace HTSim {

HTSimSession* HTSimSession::session = nullptr;

/**
 * Runs ASTRA-sim callbacks from the HTSim event list.
 *
 * One event source carries every pending callback, so a long run does not
 * allocate (and register with HTSim's logger table) an event source per
 * compute node. Callbacks due at one timestamp run in scheduling order,
 * including ones scheduled at that timestamp while it is being processed,
 * as the analytical backend's EventQueue does.
 */
class HTSimSession::Dispatcher final : public EventSource {
  public:
    explicit Dispatcher(EventList& eventlist) : EventSource(eventlist, "astra_dispatcher") {}

    void schedule(simtime_picosec when, std::function<void()> fn) {
        entries.push(Entry{when, next_seq++, std::move(fn)});
        if (!running) {
            arm();
        }
    }

    void doNextEvent() override {
        armed = false;
        running = true;
        const auto now = EventList::now();
        while (!entries.empty() && entries.top().when <= now) {
            // Move out before running: the callback may schedule more.
            auto fn = std::move(const_cast<Entry&>(entries.top()).fn);
            entries.pop();
            fn();
        }
        running = false;
        arm();
    }

  private:
    struct Entry {
        simtime_picosec when;
        uint64_t seq;
        std::function<void()> fn;
    };
    struct Later {
        bool operator()(const Entry& a, const Entry& b) const {
            return a.when != b.when ? a.when > b.when : a.seq > b.seq;
        }
    };

    /// Keep exactly one pending HTSim event, at the earliest entry's time.
    void arm() {
        if (entries.empty()) {
            return;
        }
        const auto when = entries.top().when;
        if (armed) {
            if (armed_at <= when) {
                return;
            }
            EventList::cancelPendingSourceByHandle(*this, handle);
        }
        handle = EventList::sourceIsPendingGetHandle(*this, when);
        armed = true;
        armed_at = when;
    }

    std::priority_queue<Entry, std::vector<Entry>, Later> entries;
    uint64_t next_seq = 0;
    bool running = false;
    bool armed = false;
    simtime_picosec armed_at = 0;
    EventList::Handle handle;
};

HTSimSession& HTSimSession::init(const HTSimConfig& config) {
    if (session != nullptr) {
        throw std::logic_error("HTSim session initialized twice");
    }
    static HTSimSession session_(config);
    session = &session_;
    return session_;
}

HTSimSession& HTSimSession::instance() {
    assert(session != nullptr && "HTSim session not initialized");
    return *session;
}

HTSimSession::HTSimSession(const HTSimConfig& config) {
    auto& eventlist = EventList::getTheEventList();
    // Never stop on a time limit: the frontend decides when the run ends.
    EventList::setEndtime(0);
    dispatcher = std::make_unique<Dispatcher>(eventlist);
    switch (config.transport) {
    case HTSimTransport::Tcp:
        protocol = std::make_unique<HTSimProtoTcp>(config, eventlist);
        break;
    case HTSimTransport::Raw:
        protocol = std::make_unique<HTSimProtoRaw>(config, eventlist);
        break;
    }
}

HTSimSession::~HTSimSession() = default;

bool HTSimSession::has_pending_events() const {
    return EventList::hasPendingEvents();
}

void HTSimSession::proceed() {
    assert(EventList::hasPendingEvents());
    const auto when = EventList::nextEventTime();
    while (EventList::hasPendingEvents() && EventList::nextEventTime() == when) {
        EventList::doNextEvent();
    }
}

void HTSimSession::advance_idle(const double delta_ns) {
    assert(!EventList::hasPendingEvents());
    schedule(delta_ns, [] {});
    proceed();
}

double HTSimSession::get_time_ns() const {
    return timeAsNs(EventList::now());
}

void HTSimSession::schedule(const double delta_ns, std::function<void()> fn) {
    assert(delta_ns >= 0);
    dispatcher->schedule(EventList::now() + timeFromNs(delta_ns), std::move(fn));
}

void HTSimSession::send(const int src,
                        const int dst,
                        const uint64_t size,
                        const int tag,
                        EventHandler handler,
                        void* arg) {
    if (size == 0 || src == dst) {
        // Nothing crosses a link. Complete both sides at this timestamp.
        schedule(0, [this, src, dst, size, tag, handler, arg] {
            deliver(src, dst, tag, size);
            handler(arg);
        });
        return;
    }
    const auto flow_id = ++next_flow_id;
    flows.emplace(flow_id, Flow{src, dst, size, tag, handler, arg, false});
    protocol->start_flow(flow_id, src, dst, size);
}

void HTSimSession::recv(const int src,
                        const int dst,
                        const uint64_t size,
                        const int tag,
                        EventHandler handler,
                        void* arg) {
    const auto key = MsgKey(tag, {src, dst});
    recv_queues[key].pending.push_back(PendingRecv{size, handler, arg});
    // Data that already arrived completes the receive at this timestamp, but
    // not from inside sim_recv itself, matching the analytical backend.
    drain(key, false);
}

void HTSimSession::flow_received(const flowid_t flow_id) {
    auto it = flows.find(flow_id);
    assert(it != flows.end());
    auto& flow = it->second;
    if (!flow.received) {
        flow.received = true;
        deliver(flow.src, flow.dst, flow.tag, flow.size);
    }
}

void HTSimSession::flow_sent(const flowid_t flow_id) {
    auto it = flows.find(flow_id);
    assert(it != flows.end());
    // Copy out first: delivering and the handler may start new flows.
    const auto flow = it->second;
    flows.erase(it);
    if (!flow.received) {
        // Every byte acknowledged implies every byte arrived.
        deliver(flow.src, flow.dst, flow.tag, flow.size);
    }
    flow.handler(flow.arg);
}

std::string HTSimSession::summary() const {
    auto out = std::ostringstream();
    out << "HTSim: " << next_flow_id << " flows, " << flows.size() << " in flight, "
        << recv_queues.size() << " receive queues; " << protocol->summary();
    return out.str();
}

void HTSimSession::deliver(const int src, const int dst, const int tag, const uint64_t size) {
    const auto key = MsgKey(tag, {src, dst});
    recv_queues[key].arrived += size;
    drain(key, true);
}

void HTSimSession::drain(const MsgKey& key, const bool immediate) {
    auto it = recv_queues.find(key);
    assert(it != recv_queues.end());
    auto ready = std::vector<PendingRecv>();
    auto& queue = it->second;
    while (!queue.pending.empty() && queue.arrived >= queue.pending.front().size) {
        queue.arrived -= queue.pending.front().size;
        ready.push_back(queue.pending.front());
        queue.pending.pop_front();
    }
    if (queue.pending.empty() && queue.arrived == 0) {
        recv_queues.erase(it);
    }
    for (const auto& recv : ready) {
        if (immediate) {
            recv.handler(recv.arg);
        } else {
            const auto handler = recv.handler;
            const auto arg = recv.arg;
            schedule(0, [handler, arg] { handler(arg); });
        }
    }
}

}  // namespace HTSim
