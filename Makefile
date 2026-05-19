.PHONY: all init update clean help

# 默认目标
all: init

# 初始化并拉取所有 submodules
init:
	@echo "Initializing and updating submodules..."
	git submodule update --init --recursive

# 更新所有 submodules 到最新版本
update:
	@echo "Updating all submodules to latest..."
	git submodule update --remote --recursive

# 更新特定的 submodule
update-jemalloc:
	@echo "Updating jemalloc..."
	git submodule update --remote thirdparty/jemalloc

update-libco:
	@echo "Updating libco..."
	git submodule update --remote thirdparty/libco

update-cpp_util:
	@echo "Updating cpp_util..."
	git submodule update --remote thirdparty/cpp_util

# 清理 submodules（取消初始化）
clean:
	@echo "Deinitializing submodules..."
	git submodule deinit --all -f

# 显示 submodules 状态
status:
	@echo "Submodule status:"
	git submodule status

# 帮助信息
help:
	@echo "Available targets:"
	@echo "  make init           - Initialize and clone all submodules"
	@echo "  make update         - Update all submodules to latest version"
	@echo "  make update-jemalloc - Update jemalloc only"
	@echo "  make update-libco   - Update libco only"
	@echo "  make update-cpp_util - Update cpp_util only"
	@echo "  make status         - Show submodule status"
	@echo "  make clean          - Deinitialize all submodules"
	@echo "  make help           - Show this help message"

.PHONY: configure build test bench

configure:
	cmake -S . -B build

build: configure
	cmake --build build -j

test: build
	cd build && ctest --output-on-failure

bench: build
	./bench/run_single_worker_qps.sh
