# Makefile — OS-Jackfruit Multi-Container Runtime
#
# Targets:
#   make          — build everything (user-space + kernel module)
#   make engine   — build user-space runtime only
#   make module   — build kernel module only
#   make workloads — build test workloads only
#   make clean    — remove all build artifacts
#   make load     — load kernel module (requires root)
#   make unload   — unload kernel module (requires root)

CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -pthread
KDIR    := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

.PHONY: all engine module workloads clean load unload

all: engine module workloads

# ---- User-space runtime ---------------------------------------------------
engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c -lpthread

# ---- Kernel module --------------------------------------------------------
module: monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

obj-m := monitor.o

# ---- Workload binaries ----------------------------------------------------
workloads: cpu_hog io_pulse memory_hog

cpu_hog: cpu_hog.c
	$(CC) $(CFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	$(CC) $(CFLAGS) -o io_pulse io_pulse.c

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o memory_hog memory_hog.c

# ---- Load / unload helpers ------------------------------------------------
load: module
	sudo insmod monitor.ko
	@echo "Module loaded. Device:"
	@ls -l /dev/container_monitor

unload:
	sudo rmmod monitor || true

# ---- Clean ----------------------------------------------------------------
clean:
	rm -f engine cpu_hog io_pulse memory_hog
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f modules.order Module.symvers
