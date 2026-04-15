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

## Conclusion

- Scheduling works based on nice values
- Memory limits enforced
- Clean shutdown with no zombies
