#!/bin/bash
# Build helper on the build host: environment, lunch, then m with the given
# targets (e.g. "nothing" for the Soong/Kati analysis only, or "bacon").
# Usage (on the Ryzen): /mnt/datengrab/lineage/build.sh nothing
set -e
export GIT_CONFIG_GLOBAL=/mnt/datengrab/lineage/.gitconfig.lineage
ulimit -n 65536
cd /mnt/datengrab/lineage
source build/envsetup.sh
lunch lineage_gtelwifi-cp2a-userdebug
m "$@"
