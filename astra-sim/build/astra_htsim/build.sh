#!/bin/bash
set -e
set -x

# set paths
SCRIPT_DIR=$(dirname "$(realpath "$0")")
PROJECT_DIR="${SCRIPT_DIR:?}/../.."
BUILD_DIR="${SCRIPT_DIR:?}"/build
CHAKRA_ET_DIR="${SCRIPT_DIR:?}"/../../extern/graph_frontend/chakra/schema/protobuf

# set functions
function compile_chakra_et() {
  # compile et_def.proto if one doesn't exist
  if [[ ! -f "${CHAKRA_ET_DIR:?}"/et_def.pb.h || ! -f "${CHAKRA_ET_DIR:?}"/et_def.pb.cc ]]; then
    protoc et_def.proto \
      --proto_path="${CHAKRA_ET_DIR:?}" \
      --cpp_out="${CHAKRA_ET_DIR:?}"
  fi

  if [[ ! -f "${CHAKRA_ET_DIR:?}"/et_def_pb2.py ]]; then
    protoc et_def.proto \
      --proto_path="${CHAKRA_ET_DIR:?}" \
      --python_out="${CHAKRA_ET_DIR:?}"
  fi
}

function setup() {
  # make build directory if one doesn't exist
  if [[ ! -d "${BUILD_DIR:?}" ]]; then
    mkdir -p "${BUILD_DIR:?}"
  fi

  # set concurrent build threads, capped at 16
  NUM_THREADS=$(nproc)
  if [[ ${NUM_THREADS} -ge 16 ]]; then
    NUM_THREADS=16
  fi
}

function patch_htsim() {
  local htsim_dir="${SCRIPT_DIR:?}"/../../extern/network_backend/csg-htsim
  local patch_file="${SCRIPT_DIR:?}"/htsim_astrasim.patch
  # The csg-htsim committed in LLMServingSim already carries this patch, so
  # normally there is nothing to do. A pristine upstream tree gets it applied;
  # anything else (e.g. an older version of the patch) must not be built.
  if patch -p1 -d "${htsim_dir}" -R --dry-run -s -f -i "${patch_file}" >/dev/null 2>&1; then
    echo "HTSim patch already applied"
  elif patch -p1 -d "${htsim_dir}" --dry-run -s -f -i "${patch_file}" >/dev/null 2>&1; then
    patch -p1 -d "${htsim_dir}" -s -f -i "${patch_file}"
    # Objects built before the patch would not see its header changes.
    make -C "${htsim_dir}"/sim clean >/dev/null
    echo "HTSim patch applied successfully"
  else
    echo "HTSim patch failed: csg-htsim has other local changes. From the"
    echo "LLMServingSim root, restore it with"
    echo "  git checkout -- astra-sim/extern/network_backend/csg-htsim"
    echo "and rebuild."
    exit 1
  fi
}

function compile_astrasim_htsim() {
  # compile AstraSim
  cd "${BUILD_DIR:?}" || exit
  cmake ..
  cmake --build . -j "${NUM_THREADS:?}"
}

function compile_astrasim_htsim_as_debug() {
  # compile AstraSim
  cd "${BUILD_DIR:?}" || exit
  cmake .. -DCMAKE_BUILD_TYPE=Debug
  cmake --build . --config=Debug -j "${NUM_THREADS:?}"
}

function cleanup() {
  rm -rf "${BUILD_DIR:?}"
  rm -f "${CHAKRA_ET_DIR}/et_def.pb.cc"
  rm -f "${CHAKRA_ET_DIR}/et_def.pb.h"
  rm -f "${CHAKRA_ET_DIR}/et_def_pb2.py"
}

# set default option values
build_as_debug=false
should_clean=false

# Process command-line options
while getopts "ldr" OPT; do
  case "${OPT:?}" in
  l)
    should_clean=true
    ;;
  d)
    build_as_debug=true
    ;;
  *)
    exit 1
    ;;
  esac
done

# run operations as required
if [[ ${should_clean:?} == true ]]; then
  cleanup
else
  # setup ASTRA-sim build
  setup
  compile_chakra_et
  patch_htsim

  # compile ASTRA-sim
  if [[ ${build_as_debug:?} == true ]]; then
    compile_astrasim_htsim_as_debug
  else
    compile_astrasim_htsim
  fi
fi
