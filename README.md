# OS Jackfruit – Container Runtime

## 1. Team Information
- Sarang S Nair - PES2UG24CS447
- Shibi Shetty - PES2UG24CS467

---

## 2. Build, Load, and Run Instructions

### Prerequisites
Ubuntu 22.04 / 24.04 VM

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

### Build
```bash
cd OS-Jackfruit-main/boilerplate
make
```

### Load Kernel Module
```bash
sudo insmod container_monitor.ko
dmesg | tail
```

### Start Supervisor
```bash
sudo ./engine supervisor ./rootfs-base &
```

---

## 3. Running Containers

```bash
sudo ./engine start alpha $(pwd)/rootfs-alpha /cpu_hog --soft-mib 40 --hard-mib 64
sudo ./engine start beta $(pwd)/rootfs-beta /cpu_hog --soft-mib 40 --hard-mib 64 --nice 19
```

Check:
```bash
sudo ./engine ps
```

Logs:
```bash
sudo ./engine logs alpha
```

---

## 4. Workloads

- cpu_hog → CPU intensive workload
- yes > /dev/null → background CPU load
- memory monitoring via kernel module

---

## 5. Scheduling Experiment

```bash
yes > /dev/null &
yes > /dev/null &
yes > /dev/null &
yes > /dev/null &
```

Run:
```bash
sudo ./engine run alpha ./rootfs-alpha cpu_hog --nice 0
sudo ./engine run alpha ./rootfs-alpha cpu_hog --nice 19
```

Check:
```bash
sudo ./engine logs alpha | grep "done"
```

---

## 6. Memory Monitoring

```bash
sudo dmesg -w | grep "container_monitor"
```

Shows:
- container registration
- soft limit
- hard limit

---

## 7. Clean Teardown

Stop:
```bash
sudo pkill -f "engine supervisor"
```

Check processes:
```bash
ps aux | grep -v grep | grep "engine|cpu_hog|memory_hog"
```

Check zombies:
```bash
ps aux | grep Z
```

Check logs:
```bash
sudo dmesg | grep "container_monitor" | tail -10
```

---

## 8. Engineering Analysis

### 1. Isolation Mechanisms

Our runtime achieves process and filesystem isolation using Linux namespaces and filesystem pivoting. When a container is launched, a new process is created with isolated PID and mount namespaces. The PID namespace ensures that processes inside the container see their own process tree starting from PID 1, preventing visibility into host processes. The mount namespace allows the container to have a separate filesystem view.

We use `chroot`/`pivot_root`-style isolation to switch the root filesystem to the container’s rootfs directory. This prevents access to host files outside the container environment. However, all containers still share the same host kernel. This means kernel resources such as scheduling, memory management, and system calls are global and not isolated per container.

---

### 2. Supervisor and Process Lifecycle

The supervisor is a long-running parent process responsible for managing container lifecycles. It spawns container processes using `fork` and `exec`, maintaining a parent-child relationship that allows it to monitor execution.

The supervisor tracks metadata such as container ID, PID, and state. It also handles process termination using signals and ensures proper cleanup using `waitpid`, preventing zombie processes. This centralized control simplifies lifecycle management and ensures consistent handling of start, stop, and logging operations.

---

### 3. IPC, Threads, and Synchronization

The system uses multiple IPC mechanisms. Communication between the runtime and kernel module occurs through `ioctl` calls, which allow structured data exchange via `monitor_request`. Additionally, logging and process coordination involve shared data structures.

Race conditions can arise when multiple threads or processes access shared logs or container metadata simultaneously. To prevent this, synchronization primitives such as mutexes or condition variables are used in user-space, while kernel-space uses appropriate locking mechanisms to protect shared structures. This ensures consistency and prevents data corruption in concurrent scenarios.

---

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures the actual physical memory used by a process but does not include swapped-out memory or shared pages accurately. In our system, memory limits are enforced using both soft and hard thresholds.

Soft limits act as warnings, allowing processes to continue execution while being monitored. Hard limits enforce strict constraints, triggering actions such as termination when exceeded. Enforcement is implemented in kernel space because only the kernel has full visibility and control over memory allocation. User-space alone cannot reliably enforce limits due to lack of control over page allocation and scheduling decisions.

---

### 5. Scheduling Behavior

Our experiments with CPU-bound workloads demonstrate the impact of Linux scheduling policies. When processes are run with different nice values, the scheduler allocates CPU time proportionally, favoring lower nice (higher priority) processes.

This reflects key scheduling goals:

* **Fairness:** CPU time is distributed among processes based on priority.
* **Responsiveness:** Higher priority tasks receive faster execution.
* **Throughput:** The system maintains efficient CPU utilization even under load.

The observed differences in execution time and progress confirm how Linux’s Completely Fair Scheduler (CFS) balances competing workloads while respecting priority adjustments.

## Conclusion

- Scheduling works based on nice values
- Memory limits enforced
- Clean shutdown with no zombies
