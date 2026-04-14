# Multi-Container Runtime (OS-Jackfruit Extension)

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

This project extends the base OS-Jackfruit framework with:
- Multi-container supervisor (user space)
- Kernel-level memory monitoring using RSS
- Soft and hard memory limits per container
- Lifecycle tracking (RUNNING / STOPPED / HARD_KILLED / EXITED)

---

# Project Structure

OS-Jackfruit/
│
├── boilerplate/
│   ├── engine.c              # user-space supervisor
│   ├── monitor.c            # kernel memory monitor
│   ├── monitor_ioctl.h      # ioctl definitions
│   ├── Makefile
│
├── rootfs-alpha/            # optional test filesystem
├── rootfs-beta/             # optional test filesystem
├── rootfs-base/             # base template filesystem
│
├── logs/                    # runtime logs
└── README.md

---

# Getting Started

## 1. VM Requirements

- Ubuntu 22.04 / 24.04
- Secure Boot OFF
- Linux kernel headers installed

Install dependencies:

sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

---

## 2. Build Kernel Module

cd boilerplate
make -C /lib/modules/$(uname -r)/build M=$PWD modules

---

## 3. Insert Kernel Module

sudo insmod monitor.ko

Check:

dmesg | tail

---

## 4. Create Device Node

dmesg | grep container_monitor

sudo mknod /dev/container_monitor c <MAJOR> 0
sudo chmod 666 /dev/container_monitor

---

## 5. Build User Space Engine

gcc engine.c -o engine -pthread

---

# Running the System

## Start Supervisor

./engine supervisor

---

## Start Containers

./engine start c1
./engine start c2

---

## List Containers

./engine ps

---

## Stop Container

./engine stop c1

---

# Kernel Memory Monitoring

The kernel module monitors container memory usage using RSS.

---

## Soft Limit Behavior

- Triggered when RSS exceeds soft limit
- Logs warning in kernel logs (dmesg)
- Triggered only once per container

Example:

[container_monitor] SOFT LIMIT: c1 pid=1234 rss=...

---

## Hard Limit Behavior

- Triggered when RSS exceeds hard limit
- Sends SIGKILL to process
- Removes container from kernel tracking list

Example:

[container_monitor] HARD LIMIT: c1 pid=1234 rss=...

---

# Container Lifecycle States

RUNNING      -> container active  
STOPPED      -> manually stopped by supervisor  
HARD_KILLED  -> killed by kernel monitor  
EXITED       -> normal termination  

---

# Key Components

## engine.c (User Space)
- fork-based container creation
- ioctl registration with kernel
- container lifecycle tracking
- stop / ps commands

---

## monitor.c (Kernel Module)
- linked list container tracking
- spinlock protected shared state
- periodic RSS monitoring using timer
- soft/hard memory enforcement
- automatic cleanup of dead processes

---

## Communication
- ioctl interface
- device: /dev/container_monitor

---

# Demo Flow

1. Start supervisor  
   ./engine supervisor

2. Start container  
   ./engine start c1

3. Check kernel registration  
   dmesg | grep container_monitor

4. Observe soft limit  
   dmesg | grep SOFT

5. Observe hard limit  
   dmesg | grep HARD

6. Check status  
   ./engine ps

---

# Notes

- rootfs is optional and not required for Task 4 correctness
- system focuses on kernel-level memory monitoring
- designed for Linux kernel 6.x

---

# Conclusion

This project demonstrates:
- kernel module programming
- ioctl-based communication
- process lifecycle tracking
- memory monitoring using RSS
- basic container runtime design
