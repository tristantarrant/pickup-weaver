# Pickup Weaver — guitar pickup, hollow-body and tone-pot simulation
#
# The plugin is built with CMake (DPF); this Makefile is the house entry
# point so that the Stom-Pi builder can use `make` / `make install`.
#
# Targets:
#   make               initialise the DPF submodule and build everything
#                      (LV2 + modgui, VST2, VST3, CLAP) into plugin/build/bin
#   make install       install the LV2 bundle into $(INSTALL_DIR)
#   make install-all   also VST2, VST3 and CLAP into $(VST2_DIR) / $(VST3_DIR) / $(CLAP_DIR)
#   make test          end-to-end CLAP/LV2 vs standalone-core test (needs numpy, lilv)
#   make clean         remove the build directory

PLUG       := pickup_weaver
BUILD_DIR  ?= plugin/build

PREFIX      ?= /usr
INSTALL_DIR ?= $(PREFIX)/lib/lv2
VST2_DIR    ?= $(PREFIX)/lib/vst
VST3_DIR    ?= $(PREFIX)/lib/vst3
CLAP_DIR    ?= $(PREFIX)/lib/clap

.PHONY: all build dpf install install-all test clean distclean

all: build

# DPF is a submodule (plus its pugl submodule); the build also applies
# plugin/dpf-patches/ idempotently at CMake configure time.
dpf:
	git submodule update --init --recursive

build: dpf
	cmake -S plugin -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) -j$$(nproc)

install: build
	mkdir -p "$(DESTDIR)$(INSTALL_DIR)"
	rm -rf "$(DESTDIR)$(INSTALL_DIR)/$(PLUG).lv2"
	cp -r "$(BUILD_DIR)/bin/$(PLUG).lv2" "$(DESTDIR)$(INSTALL_DIR)/"

install-all: install
	mkdir -p "$(DESTDIR)$(VST2_DIR)" "$(DESTDIR)$(VST3_DIR)" "$(DESTDIR)$(CLAP_DIR)"
	rm -rf "$(DESTDIR)$(VST3_DIR)/$(PLUG).vst3"
	cp -r "$(BUILD_DIR)/bin/$(PLUG).vst3" "$(DESTDIR)$(VST3_DIR)/"
	cp "$(BUILD_DIR)/bin/$(PLUG).clap" "$(DESTDIR)$(CLAP_DIR)/"
	cp "$(BUILD_DIR)/bin/$(PLUG)-vst2.so" "$(DESTDIR)$(VST2_DIR)/$(PLUG).so"

test: build
	python3 plugin/tests/test_plugin.py

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	git submodule deinit -f --all
