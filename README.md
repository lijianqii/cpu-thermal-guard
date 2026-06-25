# CPU Thermal Guard

一个 Linux C 程序：周期读取 CPU 温度，超过高阈值时**逐档收紧** CPU 性能上限
（限频或限功率），温度回落到低阈值以下时**逐档放宽**，直至恢复硬件默认。

## 工作原理

- **测温**：`/sys/class/thermal/thermal_zone*`，优先 `x86_pkg_temp`（CPU 封装温度）。
- **限频**（`-m freq`，默认）：写每个核心的
  `/sys/devices/system/cpu/cpuN/cpufreq/scaling_max_freq`（kHz）。
- **限功率**（`-m power`）：写 Intel RAPL
  `/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw`（µW）。
- **滞回控制**：`temp ≥ high` 收紧一档；`temp ≤ low` 放宽一档；中间保持。
  每档大小由 `-s` 步长决定，避免频率抖动。

## 安全设计（root 运行务必了解）

1. **边界 clamp**：任何写入都限制在 `[地板, 天花板]`，频率不低于 `cpuinfo_min_freq`，
   功率不低于 1W、不高于原始值。绝不会写出非法值。
2. **启动保存原值**：每个受控节点的原始值在启动时记录。
3. **退出必恢复**：正常退出、`SIGINT`、`SIGTERM` 都会在退出前写回原始值，
   不会让机器永久处于降频/降功率状态。
4. **权限降级**：非 root 或节点不可写时自动转为**只读监控**，绝不盲写。
5. **dry-run**：`-n` 只打印决策不写任何文件，便于无 root 验证。

## 编译

```sh
make            # 产出 ./cpu_thermal_guard
make run        # 等价于 ./cpu_thermal_guard -v --dry-run
```

## 用法

```
cpu_thermal_guard [选项]
  -H, --high <℃>       触发限制的高阈值 (默认 85)
  -L, --low  <℃>       解除限制的低阈值 (默认 75)
  -i, --interval <ms>  轮询间隔毫秒 (默认 1000)
  -m, --mode freq|power  限制方式 (默认 freq)
  -s, --step <值>      调整步长 (freq:MHz / power:W)
  -f, --floor <值>     最低上限地板 (freq:MHz / power:W, 默认自动)
  -n, --dry-run        不实际写入，仅打印决策
  -v, --verbose        打印每次轮询的温度与动作
  -h, --help           帮助
```

示例：

```sh
# 无 root 先观察决策
./cpu_thermal_guard -v -n -H 80 -L 70

# root 实跑（限频）
sudo ./cpu_thermal_guard -m freq -H 85 -L 75 -s 200

# root 实跑（限功率，步长 2W，地板 10W）
sudo ./cpu_thermal_guard -m power -H 85 -L 75 -s 2 -f 10
```

## 开机自启（systemd）

```sh
# 1. 安装二进制 + 服务单元（需 root）
sudo make install

# 2. 按需编辑参数
sudo systemctl edit --full cpu-thermal-guard   # 或直接改 /etc/systemd/system/cpu-thermal-guard.service

# 3. 启用并立即启动
sudo systemctl daemon-reload
sudo systemctl enable --now cpu-thermal-guard

# 查看状态/日志
systemctl status cpu-thermal-guard
journalctl -u cpu-thermal-guard -f

# 卸载
sudo make uninstall
```

服务单元 `cpu-thermal-guard.service` 已加固：`ProtectSystem=strict` + 仅
`ReadWritePaths=/sys/devices/system/cpu /sys/class/powercap`，禁网、禁新权限。
`systemctl stop` 发送 `SIGTERM`，程序会先恢复原始设置再退出。

## 测试过的环境

Fedora 43 / Intel CPU（`intel_pstate`，4 核）/ gcc 15。
其他发行版与 Intel CPU 通常通用；AMD 平台限频路径相同，RAPL 路径可能不同。
