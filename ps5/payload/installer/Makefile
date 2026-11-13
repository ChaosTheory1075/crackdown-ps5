# Crackdown: ReActivated - the installer payload (GPL-3.0-or-later, from GTA IV for PS5's), packaged in
# the app as payloads/crackdown-ps5-installer.elf (ci/ps5_package_crackdown.sh builds it).
#   make PS5_PAYLOAD_SDK=<sdk>
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF ?= crackdown-ps5-installer.elf
# Crackdown 3's install and Xbox sign-in (cd3.cpp), with libcurl (OpenSSL): the toolchain's homebrew packages,
# as Max Payne 3's logs payload
CD3_ELF ?= crackdown-ps5-cd3.elf
HOMEBREW ?= $(PS5_PAYLOAD_SDK)/../../pacbrew/v0.40.2/sysroot/user/homebrew
ZLIB ?= $(PS5_PAYLOAD_SDK)/../zlib/root/usr
CURL_LIBS := $(HOMEBREW)/lib/libcurl.a $(HOMEBREW)/lib/libpsl.a $(HOMEBREW)/lib/libssl.a $(HOMEBREW)/lib/libcrypto.a $(HOMEBREW)/lib/libzstd.a $(ZLIB)/lib/libz.a

SRC := installer.cpp store_disc.cpp store_stfs.cpp
HDR := store_disc.hpp store_stfs.hpp store_zip.hpp include/rex/platform.h include/gta4_install_io.h \
       include/rex/filesystem/direct_io.h
CXXFLAGS := -std=c++20 -O2 -Wall -Werror -Iinclude

all: $(ELF) $(CD3_ELF)

$(CD3_ELF): cd3.cpp cd3_prompts.inc
	$(CXX) -std=c++20 -O2 -Wall -Werror -I$(HOMEBREW)/include -I$(ZLIB)/include -DCURL_STATICLIB -o $@ cd3.cpp $(CURL_LIBS) -lSceSystemService -lSceUserService -lpthread
	$(PS5_PAYLOAD_SDK)/bin/llvm-strip --strip-debug $@ 2>/dev/null || true

$(ELF): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) -lpthread
	$(PS5_PAYLOAD_SDK)/bin/llvm-strip --strip-debug $@ 2>/dev/null || true

clean:
	rm -f $(ELF) $(CD3_ELF)
