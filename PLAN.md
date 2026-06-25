# CPU Thermal Guard — 开发计划

一个 C 程序：周期性读取 CPU 温度，超过阈值时通过限制 CPU 最大频率（或 RAPL 功率上限）来降温，
温度回落后逐步恢复。面向 Linux（已在 Fedora 43 / Intel CPU 验证可用的 sysfs 接口）。

## 1. 目标与范围
- 读取 CPU 封装温度。
- 温度 > 高阈值时：降低限制（限频或限功率）。
- 温度 < 低阈值时：逐步放宽，直至恢复默认。
- 命令行可配置阈值、轮询间隔、限制方式（freq / power）、是否 dry-run。
- 需要 root 才能写 sysfs；非 root 时降级为只读监控模式。

## 2. 已确认的内核接口（本机实测）
### 测温
- `/sys/class/thermal/thermal_zone*/type` + `/temp`
  - 选 type == `x86_pkg_temp` 的 zone（本机为 thermal_zone8），单位为毫摄氏度。
  - 若无该 type，回退到 `acpitz` 或所有 zone 取最大值。

### 限频（首选，权限简单）
- 每个核心：`/sys/devices/system/cpu/cpuN/cpufreq/scaling_max_freq`（kHz，root 可写）
- 边界：`cpuinfo_min_freq` ~ `cpuinfo_max_freq`（本机 400000~2800000）
- 退出/恢复时写回 `cpuinfo_max_freq`。

### 限功率（可选）
- `/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw`（微瓦，root 可写）
- 上限参考 `constraint_0_max_power_uw`。
- 退出恢复原值。

## 3. 控制策略
- 滞回控制（hysteresis）避免抖动：
  - `temp >= high`：把 max_freq 降一档（step，例如 -200MHz），不低于地板（floor，例如 min*1.5）。
  - `temp <= low`：把 max_freq 升一档，不超过硬件 max。
  - `low < temp < high`：保持。
- 每次只调一档 + 轮询间隔（默认 1s），形成平滑闭环。
- power 模式同理：按步长加减 power_limit_uw。

## 4. 程序结构
```
cpu_thermal_guard/
├── PLAN.md
├── Makefile
├── README.md
└── src/
    ├── main.c        # 参数解析、主循环、信号处理(SIGINT/SIGTERM 恢复)
    ├── config.h      # 默认阈值/路径/步长等常量
    ├── thermal.c/.h  # 温度读取（选 zone、读 temp）
    └── limiter.c/.h  # 限频 + 限功率：探测核心、读写、保存/恢复原值
```

## 5. 关键实现点
- 启动时枚举 `cpu*/cpufreq` 目录，保存每核原始 scaling_max_freq。
- 信号处理：SIGINT/SIGTERM/正常退出 时一律恢复原值（避免把机器永久降频）。
- 写 sysfs 失败（EACCES）时打印明确提示（需 root）并转监控模式。
- 全部用整型毫度/kHz/uW，避免浮点。
- `--dry-run` 只打印将要执行的动作，不写文件，便于非 root 测试。

## 6. CLI（计划）
```
cpu_thermal_guard [选项]
  -H, --high <℃>      触发限制的高阈值（默认 85）
  -L, --low  <℃>      解除限制的低阈值（默认 75）
  -i, --interval <ms> 轮询间隔（默认 1000）
  -m, --mode freq|power  限制方式（默认 freq）
  -s, --step <MHz|W>  每次调整步长
  -n, --dry-run       不实际写入，仅打印
  -v, --verbose       打印每次轮询的温度与动作
  -h, --help
```

## 7. 验证步骤
1. `make` 编译无警告（-Wall -Wextra）。
2. 非 root `--dry-run -v`：确认能读温度、打印决策。
3. root 实跑：人为把 high 设到当前温度以下，观察 scaling_max_freq 被调低；
   再升高 high，观察恢复；Ctrl-C 后确认 scaling_max_freq 恢复原值。
4. （可选）power 模式同样验证 power_limit_uw 变化与恢复。

## 8. 风险与注意
- 写错频率边界可能导致系统卡顿——所有写入都做 clamp 到 [min, max]。
- 必须保证退出时恢复，否则机器会一直被限制。
- intel_pstate 在 performance governor 下仍尊重 scaling_max_freq，可用。
