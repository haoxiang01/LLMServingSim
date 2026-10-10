# ASTRA-sim 2.0
[ASTRA-sim](https://astra-sim.github.io/) is a distributed machine learning system simulator developed by Intel, Meta, and Georgia Tech. It enables the systematic study of challenges in modern deep learning systems, allowing for the exploration of bottlenecks and the development of efficient methodologies for large DNN models across diverse future platforms.

The previous version, ASTRA-sim 1.0, is available in the `ASTRA-sim-1.0` [branch](https://github.com/astra-sim/astra-sim/tree/ASTRA-sim-1.0).

Here is a concise visual summary of our simulator:
![alt text](https://github.com/astra-sim/astra-sim/blob/master/docs/images/astrasim_overview_codesign.png)

For a comprehensive understanding of the tool, and to gain insights into its capabilities, please visit our [website](https://astra-sim.github.io/).

For information on how to use ASTRA-sim, please visit our [Wiki](https://astra-sim.github.io/astra-sim-docs/index.html).

ASTRA-sim accepts Chakra Execution Traces as workload-layer inputs. For details, please visit [Chakra Github](https://github.com/mlcommons/chakra).

We appreciate your interest and support in ASTRA-sim!

## LLMServingSim collective scopes

In this fork, collectives described by Chakra's `involved_dim` attribute use
independent stream-tag counters for each dimension scope. A TP-only operation
does not advance an EP operation's sequence, so an idle DP member can omit its
logits gather without breaking the next shared EP wave. Counters persist across
batch graphs, overlapping scopes have distinct tags, and namespace exhaustion
fails explicitly rather than wrapping. Source/destination ranks distinguish
disjoint groups. Explicit communicator handling and timing models are unchanged.

Rebuild this backend when updating LLMServingSim's idle-head execution contract;
an older binary's global counter is not compatible with that omission.

## Logical collective links

The congestion-unaware analytical frontend accepts an optional
`collective_networks` mapping in its network YAML. Supported keys are
`all_reduce`, `all_gather` and `reduce_scatter`; each value names another network
YAML, resolved relative to the containing file unless absolute. Override files
must retain the base topology and rank dimensions, and use finite positive
bandwidths and finite nonnegative latencies. The selected operation must use
`ring` or `oneRing` in the system configuration.

Each stream retains its original collective type so AllReduce's internal
scatter/gather phases use the AllReduce link. Only the network delay function
is selected: message sizes, rank groups and local reduction costs are unchanged.
Missing operations and point-to-point traffic use the common network. This is
an effective analytical link model, not NCCL channel or grouped-launch emulation.

## Expert-region trace dependencies

The bundled Chakra converter preserves the dependency chain across every
collective on an expert marker. LLMServingSim's native MoE components rely on
that ordering between routing, dispatch, experts, combine and finalization.
Update and reinstall the converter together with the frontend; the logical
collectives remain analytical tasks, not a model of NCCL grouped kernel launches.

## HTSim frontend for LLMServingSim

`astra-sim/network_frontend/htsim` speaks the same interactive stdin/stdout
protocol as the congestion-unaware analytical frontend, so LLMServingSim can
run it with `--network-backend htsim`. Build it with
`build/astra_htsim/build.sh`. It checks that `csg-htsim` carries
`build/astra_htsim/htsim_astrasim.patch` (applying it to a pristine upstream
tree) and refuses a tree with other local changes.

- Links mirror the analytical network YAML: FullyConnected dimensions, one
  dedicated link per NPU pair of the lowest differing dimension, each an output
  queue at the dimension's bandwidth and a pipe of its latency.
  `collective_networks` is rejected.
- `--htsim-transport` is `tcp` (one TCP flow per message) or `raw` (packets
  paced at full link bandwidth, no acknowledgements, unbounded buffers only).
  The remaining `--htsim-*` flags set buffers, MTU and TCP parameters.
- Serialization is computed per packet to the nearest picosecond; upstream's
  integer picoseconds-per-byte is several percent fast above ~100 GiB/s and
  zero above ~931 GiB/s.
- The loop advances one HTSim timestamp at a time. Nothing periodic stays
  scheduled while no message is in flight, so an empty event list means an idle
  cluster and the clock moves in the analytical frontend's 1 ms quanta.
- HTSim's `std::cout`/`std::cerr` output goes to `--htsim-log` or is discarded:
  stdout is the protocol channel and the frontend does not read stderr.
- Finished flows are freed once every packet that entered their links before
  completion has left them; a patched `TcpSrc` sends nothing after its last
  acknowledgement.

## Committed in LLMServingSim

This directory and its dependencies are committed in the LLMServingSim
repository as plain directories rather than git submodules. Their upstream
sources, at the revisions they were taken from:

| Path | Upstream | Revision |
| --- | --- | --- |
| `.` | https://github.com/casys-kaist/astra-sim | `d3d068f4050a2fb9a7538cc671a982152c2ba9b7`, plus the HTSim frontend integration above |
| `extern/graph_frontend/chakra` | https://github.com/casys-kaist/chakra | `5e6e9b28438bc1899105dbdd68eed4cf61bee6fd` |
| `extern/helper/fmt` | https://github.com/fmtlib/fmt | `7e73566ce7bd3617cd4b14a2e3a19a2a30b524c7` |
| `extern/helper/spdlog` | https://github.com/gabime/spdlog | `63f0875000bf90867aa7c76a831bd834cb7d4b5e` |
| `extern/memory_backend/analytical` | https://github.com/casys-kaist/astra-memory-analytical | `62313ef09abaf6a9764d13e2684ca61247def14b` |
| `extern/network_backend/analytical` | https://github.com/casys-kaist/astra-network-analytical | `5dc02322f04ac42fcfbb5304d5f7f2fa20d25e3e` |
| `extern/network_backend/analytical/extern/googletest` | https://github.com/google/googletest | `1204d634444b0ba6da53201a8b6caf2a502d883c` |
| `extern/network_backend/analytical/extern/yaml-cpp` | https://github.com/jbeder/yaml-cpp | `3d2888cc8a45da2f420454ad728cdfad01a3d54f` |
| `extern/network_backend/csg-htsim` | https://github.com/Broadcom/csg-htsim | `67cbbbb1cccdbddc400803414127d044ecd55290`, with `build/astra_htsim/htsim_astrasim.patch` applied |
| `extern/network_backend/ns-3` | https://github.com/casys-kaist/astra-network-ns3 | `3964b1d41cb5f7b51004c316b0eae1a3713c2498` |

Only files tracked upstream are included; their `.gitmodules` files are not.
Update this table when replacing a dependency.

## Contact Us

For any questions about using ASTRA-sim, you can email the ASTRA-sim User Mailing List: astrasim-users@googlegroups.com

To join the mailing list, please fill out the following form: https://forms.gle/18KVS99SG3k9CGXm6
