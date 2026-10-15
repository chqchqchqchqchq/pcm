# ResultWriter 文本输出优化与复现

本轮在 `e45bf44` 的基础上优化流式输出器，不改变 GUI、解算调度、采样规则、结果列顺序和 TXT 协议。
原始背景见 [20 GiB 性能报告](性能对比与优化.md)。

## 实测结论

主基准于 2026-09-20 完成，2026-09-23 补齐并发回归、独立样本及原码补测。
以下均为三次正式运行的中位数，括号内为最小值至最大值，单位秒；预热不计入统计。

| 场景 | 优化前格式化与写入 | 优化后格式化与写入 | 耗时减少 |
| --- | ---: | ---: | ---: |
| 20 GiB 实际解算，1005 列 × 81920 行 | 20.371（19.860–20.703） | 10.195（10.174–10.202） | 50.0% |
| 独立宽表：整数，1005 列 × 81920 行 | 10.076（10.042–10.110） | 2.741（2.726–2.779） | 72.8% |
| 独立宽表：混合值，同上 | 11.584（11.522–11.619） | 4.590（4.480–4.614） | 60.4% |
| 独立宽表：非整浮点，同上 | 21.285（21.155–21.366） | 17.203（17.165–17.441） | 19.2% |
| 原码补测：混合值，2009 列 × 8192 行 | 2.023（1.961–2.029） | 0.773（0.749–0.774） | 61.8% |

真实 20 GiB 端到端耗时由 **32.264 秒（31.562–32.704）降至 22.371 秒（22.330–22.420）**，
减少约 **30.7%**。端到端收益小于输出器收益，因为仍包含配置解析、读取、解算等工作。
以上各场景收益均超过 5%，且测量范围不重叠。非整浮点收益较小：精确浮点转十进制仍交给 Qt，
本轮没有为追求更短耗时替换精度、舍入或科学计数规则。

主基准及原码补测合计 40 次运行（含预热）的返回码、实际行列数、字节数与 SHA-256 校验全部通过。
真实数据的八份输出均为 500687714 字节，摘要与旧报告相同：

```text
8d0337d5d84745e446c353ba2663da45e8a2b6df4b67ab2bf3c127d5d32741fd
```

最终 CTest **13/13 通过**，包含新增的三线程交错写入及取消隔离回归。
首、中、尾 128 Hz 原码/工程值样本合计 **3072 行、6147072 个单元格**通过独立预期校验。
输出器取消清理三次耗时为 **0.616–0.798 ms**，均未生成目标文件；这不是整个后台的停止延迟保证。

保留的原始记录（含各轮命令、耗时、摘要、源码与可执行文件身份）：

- [主基准 32 次运行](benchmarks/result_writer_main_20260920.json)
- [原始码值补测 8 次运行](benchmarks/result_writer_raw_20260923.json)
- [独立样本与取消清理](benchmarks/result_writer_verification_20260923.json)
- [历史输出器源码身份](benchmarks/result_writer_reference.json)

## 实现与兼容边界

- 每任务保存行单元格和约 256 KiB 的 UTF-8 写入缓冲。行大小已知后一次扩容、直接填充，
  不再对每个整数分配 `QString`，也不再经 `QStringList::join` 和整行 UTF-16 转换。
- 64 位有符号/无符号整数使用 32 字节行内缓冲。最小负整数在无符号域求绝对值，无溢出或精度损失。
- 浮点仍遵循 Qt `g17` 规则。仅非零、绝对值小于 `1e17` 且精确等于整数的浮点值走整数快捷转换；
  非整数、正负零、科学计数边界、NaN、Inf 均由 `QByteArray::number(..., 'g', 17)` 处理。
- 不添加全局数字缓存，不假定参数列类型固定，也不依赖测试数据的重复周期。
- 保留原来的时间分行键、毫秒显示规则、缺值和空值区别、重复值最后覆盖、跨流查找回退、
  `_raw` 列顺序、UTF-8 无 BOM 和平台换行。`QSaveFile` 仍以 Text 模式打开；Windows 输出仍为 CRLF。
- 写入缓冲、行状态和选项均属于任务。`setOptions()` 只影响之后启动的任务，
  解决同一输出器中改变选项可能影响已启动任务的问题；批量路径也保存选项快照。
- 临时文件只在 `finishTask()` 成功时提交。取消/析构不刷出未完成缓冲、不提交，保留既有目标及其他任务结果。
  检查短写并保留错误，禁止错误后提交。仍采用调用者协作取消和批次边界，不引入线程或新的不可中断工作队列。

内存随列数、单元格长度和最大行长度变化，不随数据总行数累积。
256 KiB 是刷新阈值而非最大行长；超宽单行可以超过阈值，完整行拼接后立即刷新。
原来的批量排序写出路径保留为独立回归对照，不把全部结果积存在内存中用于性能测试。

## 测量口径

测试机：Windows，Intel Core i9-14900HX，24 核 / 32 逻辑处理器；Qt 5.14.2、MinGW 7.3、
CMake Release（`-O3 -DNDEBUG`）。只运行一个基准进程，不清空系统缓存。
每个场景每个版本预热一次，正式各三次，顺序交替；摘要及行列数校验在计时之外进行。
结果均写入 D 盘新建目录，成功校验后逐个删除本轮大型输出，保留原始输入、既有结果和 JSON 记录。

两类计时不可混用：

- 独立宽表基准：1004 参数 + TIME、81920 行，64 行一批。
  `writer_ms` 包含启动、每次 `appendDecodedBlock()`、末行刷新和最终提交；不含生成数据及 SHA-256。
  `wall_ms` 还含数据生成、文件摘要等驱动开销，不能当作纯写入时间。
  integer 为整数，mixed 每 8 列有 1 列非整浮点，double 全部为持续变化的非整浮点。
- 真实 20 GiB：相同配置及参数组，1 Hz、原码关闭；1005 列、81920 行、500687714 字节。
  `writer_ms` 沿用生产日志的“结果写入”，包含格式化和分批写入，但不含最后 `finishTask()`；
  `wall_ms` 是完整解算进程的端到端时间，包含配置解析和提交，不含外部输出校验。
  读取等待是预读重叠之后的等待时间，不等于物理读盘耗时。

差异不足 5% 或运行范围重叠时不认定有明确优势。
合成场景用于区分数字类型成本，不替代真实数据结论；Qt6、其他操作系统和其他硬盘尚未实测。

## 可复现步骤

下述目录须不存在，重复实验时更换名称。Windows 将所用 Qt/MinGW 的 `bin` 目录加入 PATH。
Python 仅使用标准库。Linux/macOS 调整编译器、Qt 前缀和可执行文件扩展名即可。

### 1. 构建两个输出器的同一基准驱动

从本地 Git 导出历史输出器，不切换分支、不覆盖当前源码：

```powershell
python tools/benchmark_result_writer.py export-reference --revision e45bf44 --output generated/writer-reference
cmake -S . -B build-writer-bench -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/qt/5.14.2/mingw73_64 -DCMAKE_CXX_COMPILER=D:/qt/Tools/mingw730_64/bin/g++.exe -DPCM_RESULT_WRITER_REFERENCE_ROOT=D:/kaifa/newPcmProcess/generated/writer-reference
cmake --build build-writer-bench --parallel 4
python tools/benchmark_result_writer.py run --baseline-writer build-writer-bench/result_writer_reference_benchmark.exe --candidate-writer build-writer-bench/result_writer_benchmark.exe --profiles integer mixed double --output generated/writer-measurement
```

`result_writer_reference_benchmark` 只替换历史 `result_writer.h/.cpp`，与当前版本共享同一驱动、
Qt 和构建选项。导出目录有版本及源码摘要清单。
快速冒烟可追加 `--rows 128 --repeats 1`；原码补测追加 `--include-raw`（此时 2009 列）。
输出字节不一致或行列数错误会返回非零，失败结果保留，不进入性能排名。

### 2. 完整 20 GiB 对照

优化前解算器必须来自相同基线，而不是另一个历史性能版本。
本轮保留的基线为 `generated/result_writer_benchmark/baseline_decode.exe`。
在新机器可导出整个基线到新目录，使用同一 Qt/MinGW Release 构建；不在当前工作区执行回退：

```powershell
git archive --format=zip --output=generated/writer-base-e45bf44.zip e45bf44
Expand-Archive -LiteralPath generated/writer-base-e45bf44.zip -DestinationPath generated/writer-base-e45bf44
cmake -S generated/writer-base-e45bf44 -B build-writer-baseline -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/qt/5.14.2/mingw73_64 -DCMAKE_CXX_COMPILER=D:/qt/Tools/mingw730_64/bin/g++.exe
cmake --build build-writer-baseline --target pcm_dataset_decode --parallel 4
```

```powershell
python tools/benchmark_result_writer.py run --baseline-decoder build-writer-baseline/pcm_dataset_decode.exe --candidate-decoder build-writer-bench/pcm_dataset_decode.exe --config generated/all_methods_1000_20g/config.hea --raw-file generated/all_methods_1000_20g/raw.pcm --parameters-file generated/performance/parameters.txt --output generated/writer-full-measurement
```

没有数据时，可用 `tools/generate_large_all_methods_dataset.py --parameters 1000 --gib 20 --output 新目录 --decoder 解算器路径`
生成夹具，并使用其 `config.hea`、`raw.pcm`、`parameters.txt`。不要覆盖已有数据目录。
旧报告的参数组来自 `generated/performance/parameters.txt`，完整文件 SHA-256 以本次 JSON 为准。

### 3. 回归与独立样本校验

```powershell
ctest --test-dir build-writer-bench --output-on-failure
python tools/benchmark_result_writer.py verify --dataset generated/all_methods_1000_20g --decoder build-writer-bench/pcm_dataset_decode.exe --writer build-writer-bench/result_writer_benchmark.exe --output generated/writer-verification
```

`verify` 只读首、中、尾各 64 个长周，128 Hz、原码开启，使用已有独立刺激模型的 `expected.txt` 逐格比较。
另外三次调用宽表驱动的 `--abort`，确认已写入多个缓冲块后仍无目标文件。
`finish_or_abort_ms` 在此仅代表输出器的取消清理时间，不代表 GUI 请求至整个后台停止的延迟。

新增 C++ 回归覆盖：

- 流式输出和未优化批量路径的完整二进制对照；4 种时间/原码组合；跨 256 KiB 刷新与 37 值切批。
- 64 位边界及十进制位数边界；4500 个随机 double 位模式和 4500 个随机 float 位模式；
  `1e17` 科学计数临界值、非有限值、正负零、次正规数。
- 中文、emoji、分隔符清理、孤立代理项、字节数组、布尔、日期时间、缺失值、重复覆盖和跨流同 ID。
- 两种输出模式的任务选项隔离；多任务交错写入；错误任务 ID 拒绝；
  多缓冲块后取消不覆盖原文件、不影响另一任务提交；析构清理；空任务；打开/提交失败不发送完成信号。
- 三个工作线程并发追加，其中一个取消，其余两份结果与独立批量路径逐字节一致。

既有控制器多时间段中断保留已完成文件、旧版解算与原码校准等测试继续保留。
