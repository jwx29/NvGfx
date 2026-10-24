#!/bin/bash
# xeintelfb package: removes XeGfxIntelFB.kext and rebuilds the auxiliary kernel collection (panel back on IONDRVFramebuffer).
# <wtl> fork: nvgfx | author: watchthelight </wtl>
XEINTELFB_MODE=uninstall exec bash "$(cd "$(dirname "$0")" && pwd)/install.sh"
