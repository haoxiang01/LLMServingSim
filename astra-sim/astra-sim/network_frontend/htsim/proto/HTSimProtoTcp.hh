/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "HTSimSession.hh"
#include "HTSimTopology.hh"

#include "eventlist.h"
#include "route.h"
#include "tcp.h"

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace HTSim {

/**
 * TCP transport: one TCP flow per ASTRA-sim message.
 */
class HTSimProtoTcp final : public HTSimProtocol {
  public:
    HTSimProtoTcp(const HTSimConfig& config, EventList& eventlist);
    ~HTSimProtoTcp() override;

    void start_flow(flowid_t flow_id, int src, int dst, uint64_t size) override;
    std::string summary() const override;

  private:
    struct Connection {
        int src_id;
        int dst_id;
        TcpSrc* src;
        TcpSink* sink;
        Route* route_out;
        Route* route_back;
        // Set when the flow completes: every data packet entered the forward
        // link before it had seen `data_mark` packets. Once they have all
        // passed, every acknowledgement has entered the reverse link before
        // `ack_mark` (0 until then).
        uint64_t data_mark = 0;
        uint64_t ack_mark = 0;
        bool data_passed = false;
    };

    /// Checks the retransmission timers of in-flight flows only, in flow id
    /// order so that runs with losses stay deterministic.
    class RtxScanner final : public EventSource {
      public:
        RtxScanner(simtime_picosec period, EventList& eventlist);
        void add(flowid_t flow_id, TcpSrc* tcp);
        void remove(flowid_t flow_id);
        void doNextEvent() override;

      private:
        simtime_picosec period;
        bool armed = false;
        EventList::Handle handle;
        std::map<flowid_t, TcpSrc*> tcps;
    };

    static void on_flow_received(flowid_t flow_id);
    static void on_flow_sent(flowid_t flow_id);

    /// Free finished connections that nothing can reach any more.
    void collect_garbage();

    EventList& eventlist;
    HTSimTopology topology;
    uint32_t initial_cwnd_bytes;
    simtime_picosec initial_rto;
    bool tcp_handshake;
    std::unique_ptr<RtxScanner> rtx_scanner;
    std::unordered_map<flowid_t, Connection> active;
    // Finished connections. One is freed once no packet of it is left in the
    // network and no retransmission timer of it is pending.
    std::vector<Connection> finished;
    size_t collect_threshold;
    uint64_t freed = 0;

    static HTSimProtoTcp* instance;
};

}  // namespace HTSim
