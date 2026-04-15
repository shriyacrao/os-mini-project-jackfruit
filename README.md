# Multi-Container Runtime for Linux Containers

## Team Information

| Name | SRN |
|------|-----|
| Shreya C Rao | PES2UG24CS481 |
| Shriya C Rao | PES2UG24CS496 |

## Project Overview

This project implements a lightweight multi-container runtime in Linux using namespaces, `chroot`, a user-space supervisor, and a kernel module for memory monitoring. The runtime can launch multiple isolated containers, track their metadata, capture logs, enforce soft and hard memory limits, and support command-based management through UNIX domain socket IPC.

## Features

- Process isolation using PID namespaces
- Hostname isolation using UTS namespaces
- Filesystem isolation using mount namespaces and `chroot`
- Long-running supervisor for container lifecycle management
- Metadata tracking for all containers
- Pipe-based log capture with bounded-buffer logging
- UNIX domain socket IPC for CLI-to-supervisor communication
- Kernel-space RSS monitoring with soft and hard memory limits
- Graceful shutdown and zombie-free cleanup
- Basic scheduling experiment using Linux `nice` values

## Prerequisites

- Ubuntu 22.04 or 24.04 running in a VM
- Secure Boot disabled
- Not tested on WSL
- `build-essential` and matching kernel headers installed

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

## Build Instructions

Clone the project and build all components:

```bash
cd ~/os_jackfruit
make
```

This builds:

- `engine` – user-space runtime and supervisor
- `monitor.ko` – kernel module for memory monitoring
- `cpu_hog` – CPU-intensive test workload
- `io_pulse` – I/O-intensive test workload
- `memory_hog` – memory-intensive test workload

## Load the Kernel Module

```bash
sudo make load
```

This inserts `monitor.ko` and creates `/dev/container_monitor`.

Verify:

```bash
ls /dev/container_monitor
```

## Prepare the Root Filesystem

Create a minimal root filesystem and copy the required shell binary:

```bash
mkdir rootfs
sudo cp /bin/bash rootfs/
sudo mkdir -p rootfs/{lib,lib64}
ldd /bin/bash | grep -o '/lib[^ ]*' | xargs -I{} sudo cp {} rootfs/lib/
```

Copy the workload binaries into `rootfs`:

```bash
cp cpu_hog ./rootfs/
cp io_pulse ./rootfs/
cp memory_hog ./rootfs/
```

## Start the Supervisor

Run the supervisor in one terminal:

```bash
sudo ./engine supervisor ./rootfs
```

The supervisor listens on `/tmp/engine.sock` for management commands.

## Start and Manage Containers

Open a new terminal and use the following commands:

```bash
sudo ./engine start alpha ./rootfs /cpu_hog 30
sudo ./engine start beta ./rootfs /cpu_hog 30
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine shutdown
```

## Demo Screenshots

### 1. Dependency Setup

![Dependency Setup](screenshots/01_setup_dependencies.jpg)

### 2. Build Output

![Build Output](screenshots/02_make_build.jpg)

### 3. Module Loaded

![Module Loaded](screenshots/03_module_loaded.jpg)

### 4. Supervisor Started

![Supervisor Started](screenshots/04_supervisor_started.jpg)

### 5. Container Started

![Container Started](screenshots/05_container_started.jpg)

### 6. Metadata Tracking

![PS Metadata](screenshots/06_ps_metadata.jpg)

The `ps` command shows each tracked container along with its state, exit code, memory limits, and log file path.

### 7. Log Output

![Log Output](screenshots/07_log_output.jpg)

### 8. Log File View

![Log File View](screenshots/08_log_file_view.jpg)

The supervisor captures container output through pipes and stores it in per-container log files.

### 9. Soft Limit Warning

![Soft Limit Warning](screenshots/09_soft_limit_warning.jpg)

When a container crosses the soft RSS limit, the kernel module logs a warning without killing the process.

### 10. Hard Limit Enforcement

![Hard Limit Kill](screenshots/10_hard_limit_kill.jpg)

### 11. Killed State in Metadata

![Hard Limit Killed State](screenshots/11_hard_limit_killed_state.jpg)

When a container exceeds the hard RSS limit, the kernel module sends `SIGKILL`, and the supervisor updates its metadata accordingly.

### 12. Multiple Containers Running

![Two Containers Running](screenshots/12_two_containers_running.jpg)

This demonstrates that the supervisor can manage more than one container at the same time.

### 13. CLI and IPC

![CLI IPC](screenshots/13_cli_ipc1.jpg)

The CLI sends commands to the supervisor over a UNIX domain socket located at `/tmp/engine.sock`.

### 14. Scheduling Experiment

![Scheduling Experiment](screenshots/14_scheduling_experiment.jpg)

The scheduling test compares CPU-bound workloads using different `nice` values.

### 15. No Zombie Processes

![No Zombies](screenshots/15_no_zombies.jpg)

After shutdown, exited child processes are reaped correctly and no zombie processes remain.

### 16. Clean Shutdown

![Shutdown Complete](screenshots/16_shutdown_complete.jpg)

This confirms orderly shutdown of containers, logging threads, and the kernel monitor.

## Engineering Analysis

### Isolation Mechanisms

Container isolation is achieved using Linux namespaces and `chroot`. The runtime uses `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS` so each container gets its own process ID space, hostname, and mount namespace. After entering the isolated environment, the process is restricted to `rootfs`, which provides basic filesystem isolation.

### Supervisor and Process Lifecycle

A long-running supervisor is necessary because container processes must have a parent that can monitor them, maintain metadata, and reap them on exit. When a child process terminates, the supervisor handles `SIGCHLD`, calls `waitpid()`, and updates the corresponding container state. This prevents zombie processes and keeps lifecycle tracking accurate.

### IPC and Logging

The runtime uses two separate IPC channels. Container logs flow from child processes to the supervisor through pipes, while management commands are sent through a UNIX domain socket. A bounded circular buffer is used for logging so that producers and consumers can work safely with mutex and condition-variable synchronization.

### Memory Monitoring

The kernel module periodically checks RSS usage for registered containers. A soft limit generates a warning message, while a hard limit causes immediate termination using `SIGKILL`. Kernel-space enforcement is more reliable than user-space monitoring because the target process cannot block or ignore the action.

### Scheduling Behavior

The scheduling experiment uses `nice` values to compare two CPU-bound containers. Since the runs in this demonstration are sequential rather than simultaneous, the result mainly shows that system load affects observed iteration counts. A more rigorous comparison would run both containers at the same time so that the Linux Completely Fair Scheduler distributes CPU time according to priority.

## Design Decisions and Tradeoffs

### Namespace-Based Isolation

Using Linux namespaces with `chroot` keeps the implementation simple while still demonstrating core isolation concepts. It is easier to implement than a full production container stack, although it is not as strong as `pivot_root` and cgroup-based isolation.

### Supervisor-Centered Architecture

All metadata is stored in the supervisor process, which makes the design straightforward and easy to debug. The tradeoff is that if the supervisor crashes, the in-memory metadata is lost.

### Separate IPC Paths

Using one path for logging and another for commands avoids interference between heavy log output and user control operations. The tradeoff is slightly more implementation complexity.

### Kernel-Space Enforcement

Memory enforcement in kernel space is more dependable because hard-limit termination cannot be bypassed by the target process. The tradeoff is that kernel code is harder to debug and must be written carefully.

## File Structure

```text
os_jackfruit/
├── engine.c
├── monitor.c
├── monitor_ioctl.h
├── cpu_hog.c
├── io_pulse.c
├── memory_hog.c
├── environment-check.sh
├── Makefile
├── README.md
└── screenshots/
```

## How to Clean Up

```bash
sudo ./engine shutdown
sudo make unload
make clean
```

## GitHub Repository

Add your repository link here:

[GitHub Repository](https://github.com/shriyacrao/os-mini-project-jackfruit)