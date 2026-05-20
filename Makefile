.PHONY: all init update update-jemalloc update-libco update-cpp_util clean status help configure build test bench

all: build

init:
	@echo "Initializing and updating submodules..."
	git submodule update --init --recursive

update:
	@echo "Updating all submodules to latest..."
	git submodule update --remote --recursive

update-jemalloc:
	@echo "Updating jemalloc..."
	git submodule update --remote thirdparty/jemalloc

update-libco:
	@echo "Updating libco..."
	git submodule update --remote thirdparty/libco

update-cpp_util:
	@echo "Updating cpp_util..."
	git submodule update --remote thirdparty/cpp_util

configure:
	cmake -S . -B build

build: configure
	cmake --build build -j

test: build
	cd build && ctest --output-on-failure

bench: build
	./bench/run_single_worker_qps.sh

clean:
	@echo "Removing build directory..."
	rm -rf build

status:
	@echo "Submodule status:"
	git submodule status

help:
	@echo "Available targets:"
	@echo "  make init"
	@echo "  make update"
	@echo "  make build"
	@echo "  make test"
	@echo "  make bench"
	@echo "  make clean"
