# Reuses ReShade SDK headers fetched by _renodx_hello_stack/.
CXX         := i686-w64-mingw32-g++
RESHADE_SDK := ../../_renodx_hello_stack/artifacts/reshade-sdk/include
OUT         := ../bin/neocron-renodx-engine.addon32

CXXFLAGS := -std=c++17 -O2 -DNDEBUG \
            -static -static-libgcc -static-libstdc++ \
            -fms-extensions \
            -Wall -Wextra -Wno-cast-function-type \
            -Iinclude \
            -I$(RESHADE_SDK)
LDFLAGS  := -shared -Wl,--kill-at -Wl,--enable-stdcall-fixup

all: $(OUT)
$(OUT): engine_injector.cpp | ../bin
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)
	@echo "Built $@ ($$(stat -c%s $@) bytes)"
../bin:
	mkdir -p $@
clean:
	rm -f $(OUT)
.PHONY: all clean
