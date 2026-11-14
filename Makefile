# Makefile for AMD RDNA 3 (Radeon 780M) macOS Driver Project

CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2
INCLUDES = -Isrc -Isrc/amd -Isrc/uapi -Isrc/winsys

TEST_BIN_DIR = build/tests
TESTS = $(TEST_BIN_DIR)/test_rdna3_discovery_uma \
        $(TEST_BIN_DIR)/test_rdna3_ip_dcn \
        $(TEST_BIN_DIR)/test_rdna3_ucode \
        $(TEST_BIN_DIR)/test_r3n_abi \
        $(TEST_BIN_DIR)/test_phx_topology \
        $(TEST_BIN_DIR)/test_wait_seq \
        $(TEST_BIN_DIR)/test_phx_gpu_queue \
        $(TEST_BIN_DIR)/test_phx_pending_fences

.PHONY: all test clean kext fixtures

all: test

$(TEST_BIN_DIR):
	mkdir -p $(TEST_BIN_DIR)

$(TEST_BIN_DIR)/test_phx_pending_fences: tests/test_phx_pending_fences.cpp src/phxbringup/PhxPendingFences.h | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(TEST_BIN_DIR)/test_phx_gpu_queue: tests/test_phx_gpu_queue.cpp src/phxbringup/PhxGpuQueue.h | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(TEST_BIN_DIR)/test_wait_seq: tests/test_wait_seq.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ -lpthread

$(TEST_BIN_DIR)/test_phx_topology: tests/test_phx_topology.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_discovery_uma: src/amd/rdna3_ip_discovery.cpp src/amd/rdna3_rebar_uma.cpp tests/test_rdna3_discovery_uma.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_ip_dcn: src/amd/rdna3_ip_blocks.cpp src/amd/rdna3_dcn314_scanout.cpp src/amd/rdna3_rebar_uma.cpp tests/test_rdna3_ip_dcn.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_rdna3_ucode: src/amd/rdna3_ucode_extract.cpp tests/test_rdna3_ucode.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

$(TEST_BIN_DIR)/test_r3n_abi: src/uapi/r3n_user_client.cpp src/winsys/radv_r3n_winsys.cpp tests/test_r3n_abi.cpp | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

test: $(TESTS)
	python3 tests/test_native_bo_lifetime.py
	python3 tests/test_native_stop.py
	python3 tests/test_native_vm.py
	python3 tests/test_native_sysmem.py
	python3 tests/test_native_submit.py
	python3 tests/test_install_safety.py
	python3 tests/test_release_pci_matches.py
	python3 tests/test_ue200watch.py
	python3 tests/test_phxboot_watch.py
	python3 tests/test_render_load_failure.py
	python3 tests/test_format_view_clear.py
	python3 tests/test_render_surface_clear.py
	python3 tests/test_scanout_dump.py
	python3 tests/test_phxctl_cli.py
	python3 tests/test_bringup_failures.py
	@echo "=== Running RDNA 3 Driver Unit Tests ==="
	@for t in $(TESTS); do \
		echo "Running $$t..."; \
		$$t || exit 1; \
	done
	@echo "All unit tests passed successfully!"

fixtures:
	bash tools/fetch_test_firmware.sh tests/fixtures/phoenix

kext:
	@echo "Building PhxBringup.kext..."
	python3 tools/build_kext.py

clean:
	rm -rf build/
