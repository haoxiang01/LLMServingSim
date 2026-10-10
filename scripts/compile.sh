#!/bin/bash
set -e

# Usage: ./scripts/compile.sh [--htsim]
#   --htsim  also build the packet-level HTSim backend (--network-backend htsim)

# Path
SCRIPT_DIR=$(dirname "$(realpath $0)")
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

BUILD_HTSIM=false
for arg in "$@"; do
  case "$arg" in
    --htsim) BUILD_HTSIM=true ;;
    *) echo "Unknown option: $arg (usage: $0 [--htsim])" >&2; exit 1 ;;
  esac
done

# Intall Chakra (Use Chakra fork in ASTRA-Sim repo).
(
cd ${REPO_ROOT}/astra-sim/extern/graph_frontend/chakra
pip3 install .
)

# Compile ASTRA-sim with analytical backend model
(
cd ${REPO_ROOT}/astra-sim
bash ./build/astra_analytical/build.sh
)

# Compile ASTRA-sim with HTSim backend model. The build patches the csg-htsim
# submodule's working tree (build/astra_htsim/htsim_astrasim.patch).
if [[ ${BUILD_HTSIM} == true ]]; then
(
cd ${REPO_ROOT}/astra-sim
bash ./build/astra_htsim/build.sh
)
fi

# Compile ASTRA-sim with ns3 backend model
# (
# cd ${REPO_ROOT}/astra-sim
# bash ./build/astra_ns3/build.sh
# )
