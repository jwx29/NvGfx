# NvGfx: Darwin overlay for NVIDIA's src/nvidia and src/nvidia-modeset Makefiles.
# Used as: make -f Makefile -f <this file> nvgfx-archive NVGFX_LIB=libnvkernel.a
# Upstream links nv-kernel.o / nv-modeset-kernel.o with GNU ld -r --gc-sections and objcopy, which do not exist
# for Mach-O. This overlay keeps NVIDIA's object rules and CFLAGS untouched and only adds a static archive target
# (what NullMoth's NVRM.kext links: libnvkernel.a and libnvkms.a). The modeset shader blobs (GNU ld --format=binary)
# are left out: kexts/NVRM/nvkms_shaders.cpp provides the _binary_*_shaders_xz_* symbols instead.
# <wtl> fork: nvgfx | author: watchthelight </wtl>

# 615 builds NVKMS with -mfunction-return=thunk-extern (a Linux return-thunk mitigation); XNU exports no
# __x86_return_thunk, and the shipped 2.5.x NVRM has none, so it is dropped here.
CFLAGS := $(filter-out -mfunction-return=thunk-extern,$(CFLAGS))

NVGFX_LIB  ?= libnvkernel.a
NVGFX_OBJS  = $(filter-out $(SHADER_OBJS),$(OBJS))

.PHONY: nvgfx-objs nvgfx-archive nvgfx-print-objs
nvgfx-objs: $(NVGFX_OBJS)
nvgfx-archive: $(OUTPUTDIR)/$(NVGFX_LIB)
nvgfx-print-objs:
	@printf '%s\n' $(NVGFX_OBJS)

$(OUTPUTDIR)/$(NVGFX_LIB): $(NVGFX_OBJS)
	@$(RM) $@
	@printf '%s\n' $(NVGFX_OBJS) > $@.filelist
	$(at_if_quiet)xcrun libtool -static -no_warning_for_no_symbols -o $@ -filelist $@.filelist
