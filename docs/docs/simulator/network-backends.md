---
title: Network backends
sidebar_position: 7
---

# Network backends

`--network-backend` selects the ASTRA-Sim binary that times communication.
Every backend runs the same Python frontend, the same execution traces and the
same compute and memory model; only the time a message spends on the network
differs.

| Backend | Model | Build |
| --- | --- | --- |
| `analytical` (default) | Congestion-unaware: `latency + size / bandwidth` per message | `./scripts/compile.sh` |
| `htsim` | Packet-level simulation of the same links, with queueing; TCP or full-bandwidth transport | `./scripts/compile.sh --htsim` |
| `ns3` | Work in progress; not usable with the current frontend | — |

## HTSim

The `htsim` backend runs [csg-htsim](https://github.com/Broadcom/csg-htsim)
under ASTRA-Sim. Each point-to-point message that ASTRA-Sim's collective
algorithms emit becomes a stream of packets that are serialized, queued and
propagated individually, either as a TCP flow or at the link's full
bandwidth. Messages that share a link compete for it, so concurrent
collectives, all-to-all traffic and pipeline transfers interfere with each
other, which the analytical backend cannot represent.

### Build

Inside the simulator container:

```bash
./scripts/compile.sh --htsim
```

This builds the analytical backend as usual, then
`astra-sim/build/astra_htsim/build.sh`, which applies
`build/astra_htsim/htsim_astrasim.patch` to the `csg-htsim` submodule's working
tree and produces `astra-sim/build/astra_htsim/build/bin/AstraSim_HTSim`.
The patched submodule therefore shows as modified in `git status`; that is
expected. If the build reports that `csg-htsim` has other local changes,
restore it with `git -C astra-sim/extern/network_backend/csg-htsim checkout -- sim`
and rebuild.

### Run

```bash
python -m serving \
  --cluster-config 'configs/cluster/single_node_tp_pp_instance.json' \
  --dataset 'workloads/example_trace.jsonl' --num-reqs 10 \
  --network-backend htsim
```

Any cluster config works unchanged. `collective_links` is analytical-only and
is rejected.

### What the links are

HTSim simulates the topology the cluster config already describes, so
`link_bw` and `link_latency` keep their meaning:

- The generated `network.yml` has one FullyConnected dimension per parallelism
  axis. A message between two NPUs crosses one dedicated link of the lowest
  dimension in which their addresses differ — the link the analytical backend
  charges.
- A link is an output queue that serializes packets at that dimension's
  `link_bw` (GiB/s) followed by a pipe of `link_latency` ns. The two directions
  are separate links; data in one direction shares its queue with the
  acknowledgements of the other.
- There are no switches, so traffic interferes only where messages share a
  link: between the same pair of NPUs, including the reverse direction.

### Transport

`htsim.transport` chooses how a message crosses its link.

**`raw`: full bandwidth.** A message is sent as back-to-back packets at the
link's full bandwidth, with no congestion control and no acknowledgements. A
message alone on its link takes the analytical cost, rounded up to whole
packets; messages sharing a link interleave in its queue and split the
bandwidth. Both sides complete when the last packet arrives, as in the
analytical backend. Nothing retransmits, so `raw` requires unbounded buffers.

**`tcp` (default): one TCP flow per message.**

- Connections are treated as already open (no SYN round trip) unless
  `tcp_handshake` is set, as collective libraries keep their connections
  across operations.
- Each message starts in slow start from `initial_cwnd_packets`, so short
  messages on high-bandwidth links do not reach line rate.
- Acknowledgements travel on the reverse link and share its queue.
- With the default unbounded buffers the network is lossless. A finite
  `queue_bytes` drops packets when a queue fills, and TCP recovers by fast
  retransmit or by timeout. Timeouts do not back off, and a loss before a
  flow's first RTT sample waits `initial_rto_us`.

With either transport, packets are fixed-size: a message occupies
`ceil(size / mtu)` full packets. Serialization time is computed per packet to
the nearest picosecond, so link bandwidths of any magnitude are honoured.

RAW_RESULTS

### Settings

The optional top-level `htsim` object in the cluster config sets transport
parameters; every field is optional. See the
[cluster config reference](../reference/cluster-config#htsim-settings).

```json
{
  "htsim": {"transport": "raw"}
}
```

```json
{
  "htsim": {
    "transport": "tcp",
    "queue_bytes": 1048576,
    "mtu": 4096,
    "initial_cwnd_packets": 64,
    "min_rto_us": 1000,
    "initial_rto_us": 1000
  }
}
```

The object is ignored, with a warning, by other backends.

### Output

HTSim's own console output is discarded. With `--keep-inputs` (or
`--save-trace-text`) it is written to `htsim.log` in the run's inputs
directory, ending with a summary line: messages simulated and, for `tcp`,
connections released and packets dropped.

### Limitations

- Two transports: TCP, and full bandwidth without congestion control. RDMA
  congestion control (DCQCN, HPCC), priority flow control and NCCL's protocol
  choices are not modeled.
- FullyConnected dimensions only; no switch tiers, oversubscription or
  multipath.
- Simulation time grows with the number of packets, i.e. with bytes moved
  divided by `mtu`. The bundled scenarios run in about the same wall time as
  analytical; long, communication-heavy runs take longer.
- The [hardware profiler's](../profiler/adding-hardware#calibration-contract)
  link fits are calibrated against the analytical Ring model. Under HTSim
  they describe the same physical links; with `tcp` they are not a
  calibration of TCP behaviour.
