# sourced by the build scripts
X=$HOME/src/xe-userland
export PATH=$HOME/.venvs/mesa/bin:/opt/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
export CC=/usr/bin/clang CXX=/usr/bin/clang++ OBJC=/usr/bin/clang
