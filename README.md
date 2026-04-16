# Multi-Container Runtime

## 1. Objective
This project implements a lightweight container runtime to demonstrate key Operating System concepts such as process isolation, scheduling, inter-process communication (IPC), and memory management.

---

## 2. Platform
- Ubuntu 22.04 / 24.04 (VM recommended)
- Secure Boot OFF

---

## 3. Build and Setup

### Install Dependencies
sudo apt update  
sudo apt install -y build-essential linux-headers-$(uname -r)

---

### Build Project
cd boilerplate  
make clean  
make  

This builds:
- engine (runtime)
- cpu_hog, memory_hog (workloads)
- monitor.ko (kernel module)

---

### Prepare Root Filesystem
rm -rf rootfs-base rootfs-alpha rootfs-beta  

mkdir rootfs-base  

tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base  

cp -a rootfs-base rootfs-alpha  
cp -a rootfs-base rootfs-beta  

---

### Copy Workloads
cp cpu_hog rootfs-alpha/  
cp memory_hog rootfs-alpha/  

cp cpu_hog rootfs-beta/  
cp memory_hog rootfs-beta/  

---

### Load Kernel Module
sudo rmmod monitor 2>/dev/null  
sudo insmod monitor.ko  
lsmod | grep monitor  

---

## 4. Running the System

### Terminal 1
sudo ./engine supervisor ./rootfs-base  

---

### Terminal 2
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog 60"  
sudo ./engine start beta ./rootfs-beta "/cpu_hog 60"  

sudo ./engine ps  

---

### Logs
sudo ./engine logs alpha  

---

### Stop Container
sudo ./engine stop alpha  
sudo ./engine ps  

---

## 5. Memory Limit Test
sudo ./engine start memtest ./rootfs-alpha "/memory_hog 2 500" --soft-mib 40 --hard-mib 64  

sleep 20  

sudo dmesg | grep container_monitor | tail -20  

Expected:
- Soft limit → warning  
- Hard limit → process killed  

---

## 6. Scheduling Experiment
time sudo ./engine run hogA ./rootfs-alpha "/cpu_hog 30"  

time sudo ./engine run hogB ./rootfs-beta "/cpu_hog 30" --nice 19  

Observation:
- hogA (nice 0) executes faster  
- hogB (nice 19) executes slower  

---

## 7. Features
- Multi-container execution  
- Process isolation using namespaces  
- Logging using pipes  
- IPC using UNIX domain sockets  
- Memory monitoring using kernel module  
- Scheduling behavior using nice values  

---

## 8. Concepts Explained

### Process Isolation
Implemented using Linux namespaces:
- CLONE_NEWPID → separate process IDs  
- CLONE_NEWUTS → separate hostname  
- CLONE_NEWNS → separate filesystem  

chroot() restricts container access to its root filesystem.

---

### Supervisor
A persistent supervisor process manages all containers and prevents zombie processes using waitpid().

---

### IPC and Logging
- UNIX domain sockets → CLI communication  
- Pipes → capture container output  

---

### Memory Management
Kernel module monitors RSS (Resident Set Size):
- Soft limit → warning  
- Hard limit → process termination  

---

### Scheduling
Linux uses Completely Fair Scheduler (CFS):
- nice 0 → high priority  
- nice 19 → low priority  

CPU time is distributed accordingly.

---

## 9. Conclusion
This project successfully demonstrates core OS concepts through a working container runtime including process isolation, logging, memory enforcement, and scheduling behavior.
