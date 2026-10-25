echo "=== Kernel extensions ==="
kmutil showloaded | grep -Ei 'XeGfx|NMIntelFB|XeGfxAccel|XeGfxAGDC'

echo
echo "=== Metal plugin ==="
PLUGIN="/Library/GPUBundles/NVMTLDriver.bundle"
ls -lh "$PLUGIN/Contents/MacOS/NVMTLDriver"
shasum -a 256 "$PLUGIN/Contents/MacOS/NVMTLDriver"

echo
echo "=== Metal plugin signature ==="
codesign --verify --verbose=4 "$PLUGIN"
codesign -dv --verbose=4 "$PLUGIN" 2>&1 | grep -E 'Identifier|Authority|TeamIdentifier|Signature|Runtime'

echo
echo "=== Mesa ANV and libdrm ==="
ls -lh \
  /Library/GPUBundles/nvmtl/libvulkan_intel.dylib \
  /Library/GPUBundles/nvmtl/libdrm.2.dylib \
  /Library/GPUBundles/nvmtl/intel_icd.json

echo
echo "=== Vulkan ICD configuration ==="
cat /Library/GPUBundles/nvmtl/intel_icd.json

echo
echo "=== Metal plugin dependencies ==="
otool -L "$PLUGIN/Contents/MacOS/NVMTLDriver"
