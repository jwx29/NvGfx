bash ~/Downloads/build_xegfx_stack.sh --check

bash ~/Downloads/build_xegfx_stack.sh \
  --repo "$HOME/src/NvGfx" \
  --out "$HOME/src/NvGfx-build" \
  --upstream-ref origin/xe \
  --ws-copy
