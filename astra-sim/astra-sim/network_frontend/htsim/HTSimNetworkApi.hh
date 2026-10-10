/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include "common/CommonNetworkApi.hh"
#include <astra-network-analytical/congestion_unaware/Topology.h>
#include <cstdint>
#include <memory>

namespace HTSim {

/**
 * AstraNetworkAPI backed by an HTSimSession: point-to-point messages become
 * packet-level flows, and every ASTRA-sim callback runs from HTSim's event
 * list, so compute, memory and network share one clock.
 */
class HTSimNetworkApi final : public AstraSimAnalytical::CommonNetworkApi {
  public:
    /**
     * Set the logical topology. Only its dimensions and per-dimension
     * bandwidths are read here (ASTRA-sim's collective scheduler asks for
     * them); the links themselves are simulated by HTSim.
     */
    static void set_topology(
        std::shared_ptr<NetworkAnalyticalCongestionUnaware::Topology> topology) noexcept;

    explicit HTSimNetworkApi(int rank) noexcept;

    int sim_send(void* buffer,
                 uint64_t count,
                 int type,
                 int dst,
                 int tag,
                 AstraSim::sim_request* request,
                 void (*msg_handler)(void* fun_arg),
                 void* fun_arg) override;

    int sim_recv(void* buffer,
                 uint64_t count,
                 int type,
                 int src,
                 int tag,
                 AstraSim::sim_request* request,
                 void (*msg_handler)(void* fun_arg),
                 void* fun_arg) override;

    AstraSim::timespec_t sim_get_time() override;

    void sim_schedule(AstraSim::timespec_t delta,
                      void (*fun_ptr)(void* fun_arg),
                      void* fun_arg) override;
};

}  // namespace HTSim
