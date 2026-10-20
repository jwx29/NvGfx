#!/bin/bash
# intelfb package: removes NMIntelFB.kext and rebuilds the auxiliary kernel collection (the Hybrid-boot recovery).
# <wtl> fork: nvgfx | author: watchthelight </wtl>
INTELFB_MODE=uninstall exec bash "$(cd "$(dirname "$0")" && pwd)/install.sh"
