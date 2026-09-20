#!/usr/bin/env python3
"""生成至少 1000 参数、20 GiB 的旧版 CFTE 数据，并验证文件完整性和三处解算样本。"""

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

from generate_all_methods_dataset import (
    compare, config_text, expected_file, file_hash, json_file, main_parameters,
    parameter_name_for_stream, text_file, write_raw,
)

BASE_MS = 3723000
# 8 Hz 为 2 的幂，长周间隔恰好 125 ms，20 GiB 全程不会跨日。
FPS = 8
WORDS = 2048
DEPTH = 8
FRAME_BYTES = WORDS * DEPTH * 2
MAXIMUM_RATE = 16
SAMPLE_RATE = FPS * MAXIMUM_RATE


def progress(output, phase, **fields):
    """打印并持久化当前阶段，供长时间生成期间查看。"""
    record = {"phase": phase, **fields}
    print(json.dumps(record, ensure_ascii=True), flush=True)
    json_file(output/"progress.json", record)


def expand_parameters(count):
    """高频参数优先分配物理字，保留全部解算布局并满足旧版 2048 字子帧限制。"""
    original = main_parameters()
    high_rate = copy.deepcopy(original[-1])
    high_rate.name = "RATE16"
    high_rate.rate = MAXIMUM_RATE
    high_rate.values = list(range(1600, 1616))
    high_rate.note = "每长周 16 次；8 个子帧内各重复两次"
    original.append(high_rate)
    parameters = []
    for index in range(count):
        group, position = divmod(index, len(original))
        parameter = copy.deepcopy(original[position])
        if group:
            parameter.name += f"_G{group+1:02d}"
        parameter.name = parameter_name_for_stream(parameter.name)
        phase = group % len(parameter.values)
        parameter.values = parameter.values[phase:] + parameter.values[:phase]
        parameter.note = f"第 {group+1} 组；" + (parameter.note or original[position].name)
        parameters.append(parameter)
    occupied = {frame*WORDS+word for frame in range(DEPTH) for word in range(8)}
    for parameter in sorted(parameters, key=lambda item: -item.rate):
        minimum_word = min(part[1] for part in parameter.segments)
        span = max(part[1] for part in parameter.segments)-minimum_word+1
        repeats = max(1, parameter.rate//DEPTH)
        period = max(1, DEPTH//parameter.rate)
        relative = [(frame % period, word-minimum_word, source, length, target)
                    for frame, word, source, length, target in parameter.segments]
        for first_word in range(8, WORDS//repeats-span+1):
            addresses = set()
            for occurrence in range(parameter.rate):
                for frame, word, _, _, _ in relative:
                    if parameter.rate <= DEPTH:
                        address = frame*WORDS+first_word+word+occurrence*(DEPTH*WORDS//parameter.rate)
                    else:
                        address = (occurrence//repeats)*WORDS+first_word+word
                        address += occurrence % repeats*(WORDS//repeats)
                    addresses.add(address)
            if not addresses.intersection(occupied):
                if min(addresses) < 0 or max(addresses) >= WORDS*DEPTH:
                    raise ValueError(f"{parameter.name}: 采样地址超出长周")
                occupied.update(addresses)
                parameter.segments = [(frame, first_word+word, source, length, target)
                                      for frame, word, source, length, target in relative]
                break
        else:
            raise ValueError(f"{parameter.name}: 参数数量超过旧版安全帧布局容量")
    return parameters


def generate_raw(output, parameters, frames):
    """缓存一个刺激周期，用 8 MiB 块写满文件，逐帧更新独立递增的 DM6 时间码。"""
    period = math.lcm(*(len(parameter.values)//math.gcd(len(parameter.values), parameter.rate)
                        for parameter in parameters))
    template_path = output/"samples"/"period.pcm"
    template_path.parent.mkdir(parents=True, exist_ok=True)
    progress(output, "building_template", period_frames=period)
    write_raw(template_path, parameters, period, BASE_MS, FPS, WORDS, DEPTH)
    template = template_path.read_bytes()
    if len(template) != period*FRAME_BYTES:
        raise ValueError("刺激周期文件长度错误")
    view = memoryview(template)
    target = output/"raw.pcm"
    temporary = output/"raw.pcm.partial"
    digest = hashlib.sha256()
    start = time.monotonic()
    next_report = start
    first = 0
    with temporary.open("wb") as destination:
        while first < frames:
            phase = first % period
            count = min(256, period-phase, frames-first)
            block = bytearray(view[phase*FRAME_BYTES:(phase+count)*FRAME_BYTES])
            for offset in range(count):
                milliseconds = BASE_MS + (first+offset)*1000//FPS
                seconds, ms = divmod(milliseconds, 1000)
                hour, remainder = divmod(seconds, 3600)
                minute, second = divmod(remainder, 60)
                struct.pack_into("<4H", block, offset*FRAME_BYTES+6, hour, minute, second, ms)
            if destination.write(block) != len(block):
                raise OSError("原始数据未完整写入")
            digest.update(block)
            first += count
            now = time.monotonic()
            if now >= next_report or first == frames:
                progress(output, "writing_raw", bytes=first*FRAME_BYTES, total_bytes=frames*FRAME_BYTES,
                         percent=round(first*100/frames, 2),
                         mib_per_second=round(first*FRAME_BYTES/(1024**2)/max(now-start, .001), 1))
                next_report = now+5
        destination.flush()
        os.fsync(destination.fileno())
    if temporary.stat().st_size != frames*FRAME_BYTES:
        raise ValueError("原始数据大小不等于完整长周数乘以长周字节数")
    temporary.replace(target)
    return digest.hexdigest(), period


def verify_raw(output, expected_hash):
    """重新读取整个实际文件并计算哈希，确认 20 GiB 均正确落盘。"""
    target = output/"raw.pcm"
    total = target.stat().st_size
    digest = hashlib.sha256()
    read_bytes = 0
    next_report = 0
    with target.open("rb") as source:
        while True:
            block = source.read(8*1024*1024)
            if not block:
                break
            digest.update(block)
            read_bytes += len(block)
            now = time.monotonic()
            if now >= next_report or read_bytes == total:
                progress(output, "verifying_raw", bytes=read_bytes, total_bytes=total,
                         percent=round(read_bytes*100/total, 2))
                next_report = now+5
    actual_hash = digest.hexdigest()
    if actual_hash != expected_hash:
        raise ValueError("写入哈希与磁盘全文件读取哈希不一致")
    return actual_hash


def clock_text(milliseconds):
    """将确定性的毫秒时刻显示为时分秒。"""
    seconds, ms = divmod(milliseconds, 1000)
    hour, remainder = divmod(seconds, 3600)
    minute, second = divmod(remainder, 60)
    return f"{hour:02d}:{minute:02d}:{second:02d}.{ms:03d}"


def verify_samples(output, parameters, frames, decoder):
    """从首中尾分别抽取 64 个真实长周，调用生产控制器逐格比较全部 1000 参数。"""
    reports = []
    count = min(frames, 64)
    positions = (("begin", 0), ("middle", frames//2), ("end", frames-count))
    with (output/"raw.pcm").open("rb") as raw:
        for name, first in positions:
            progress(output, "decoding_sample", sample=name, first_frame=first, frames=count)
            raw.seek(first*FRAME_BYTES)
            sample_bytes = raw.read(count*FRAME_BYTES)
            if len(sample_bytes) != count*FRAME_BYTES:
                raise ValueError(f"{name} 样本未读完整")
            # 独立检查样本中每个长周的同步字、子帧 ID、时间连续性。
            for index in range(count):
                frame_offset = index*FRAME_BYTES
                h, m, s, ms = struct.unpack_from("<4H", sample_bytes, frame_offset+6)
                decoded_ms = ((h*60+m)*60+s)*1000+ms
                if decoded_ms != BASE_MS+(first+index)*1000//FPS:
                    raise ValueError(f"{name} 第 {index} 个长周时间码错误")
                for subframe in range(DEPTH):
                    sync1, sync2, frame_id = struct.unpack_from(
                        "<3H", sample_bytes, frame_offset+subframe*WORDS*2)
                    if (sync1, sync2, frame_id) != (0xfe6b, 0x2840, subframe):
                        raise ValueError(f"{name} 第 {index} 个长周同步或 ID 错误")
            directory = output/"samples"/name
            directory.mkdir(parents=True, exist_ok=True)
            sample_path = directory/"raw.pcm"
            sample_path.write_bytes(sample_bytes)
            start = (BASE_MS+first*1000//FPS)/1000
            end = (BASE_MS+(first+count)*1000//FPS)/1000
            wanted_rows = expected_file(directory/"expected.txt", parameters, first+count,
                                        BASE_MS, FPS, MAXIMUM_RATE, list(range(MAXIMUM_RATE)), 1, start=start, end=end)
            command = [str(decoder), "--config", str(output/"config.hea"), "--raw", str(sample_path),
                       "--parameters", str(output/"parameters.txt"), "--output", str(directory/"actual.txt"),
                       "--sample-rate", str(SAMPLE_RATE)]
            process = subprocess.run(command, capture_output=True, timeout=120)
            (directory/"decode.log").write_bytes(process.stdout+process.stderr)
            if process.returncode:
                raise ValueError(f"{name} 解算失败，退出码 {process.returncode}，详见样本 decode.log")
            rows, cells = compare(directory/"expected.txt", directory/"actual.txt")
            if rows != wanted_rows or rows != count*MAXIMUM_RATE:
                raise ValueError(f"{name} 解算输出行数错误")
            report = {"name": name, "first_frame": first, "frames": count,
                      "start_seconds": start, "end_seconds": end, "raw_sha256": file_hash(sample_path),
                      "rows": rows, "cells": cells, "passed": True}
            reports.append(report)
            progress(output, "sample_passed", **report)
    return reports


def generate(output, count, raw_bytes, decoder):
    """完成配置、20 GiB 原始数据、全文件完整性及首中尾解算验证。"""
    if WORDS > 2048 or WORDS*DEPTH > 32767:
        raise ValueError("超出旧版帧缓冲或有符号时间地址的安全范围")
    if raw_bytes % FRAME_BYTES or raw_bytes < 128*FRAME_BYTES:
        raise ValueError("原始文件大小必须为完整长周字节数的整数倍，且至少 128 个长周")
    frames = raw_bytes//FRAME_BYTES
    if BASE_MS+frames*1000//FPS >= 86400000:
        raise ValueError("此规模会导致时间跨日，需要先调整长周率")
    parameters = expand_parameters(count)
    frequencies = [FPS, SAMPLE_RATE] + [parameter.rate for parameter in parameters]
    if any(rate <= 0 or (rate & (rate-1)) != 0 for rate in frequencies):
        raise ValueError("长周率、处理采样率和参数出现次数必须为 2 的幂")
    if 1000 % FPS:
        raise ValueError("DM6 毫秒时间码需要能精确表示长周间隔")
    output.mkdir(parents=True, exist_ok=True)
    required_space = raw_bytes + 1024**3
    if shutil.disk_usage(output).free < required_space:
        raise OSError(f"需要至少 {required_space} 字节可用空间，供新文件和验证样本使用")
    config = config_text(parameters, fps=FPS, words=WORDS, depth=DEPTH)
    text_file(output/"config.hea", config, "gb18030")
    text_file(output/"config_utf8.hea", config)
    text_file(output/"parameters.txt", "\n".join(["TIME"]+[parameter.name for parameter in parameters])+"\n")
    # 短参数组可用于全文件读取/时间连续性测试，不生成 1000 参数的巨型 TXT。
    text_file(output/"parameters_quick.txt", "TIME\nUINT16_1\n")
    json_file(output/"verification.json", {"passed": None, "description": "正在重新生成和验证"})
    written_hash, period = generate_raw(output, parameters, frames)
    verified_hash = verify_raw(output, written_hash)
    reports = verify_samples(output, parameters, frames, decoder)
    first_ms = BASE_MS
    last_ms = BASE_MS+(frames-1)*1000//FPS
    end_ms = BASE_MS+frames*1000//FPS
    coverage = [
        {"name": parameter.name, "code_type": parameter.code, "cal_type": parameter.calibration,
         "rate": parameter.rate, "inverse": parameter.inverse, "segments": parameter.segments,
         "stimulus_values": parameter.values, "calibration_fields": parameter.extra,
         "description": parameter.note}
        for parameter in parameters
    ]
    manifest = {
        "format_version": 1, "description": "1000 业务参数、旧版全解算方式、20 GiB PCM 合成数据",
        "business_parameters": count, "time_parameters": 4, "total_parameters": count+4,
        "parameter_name_format": "parameter_name_stream_index", "stream_index": 1,
        "raw_bytes": raw_bytes, "raw_gib": raw_bytes/1024**3, "raw_sha256": verified_hash,
        "frame_count": frames, "frame_bytes": FRAME_BYTES, "subframe_words": WORDS,
        "subframe_depth": DEPTH, "major_frame_rate": FPS, "full_output_sample_rate": SAMPLE_RATE,
        "parameter_sample_rates_hz": sorted({FPS*parameter.rate for parameter in parameters}),
        "sample_rates_power_of_two": True, "maximum_parameter_rate": MAXIMUM_RATE,
        "legacy_subframe_limit_words": 2048, "legacy_buffer_limits_verified": True,
        "config_alignment_width_rule": "longest_non_section_field_name",
        "physical_word_bits": 16, "sync_bytes_hex": "6B FE 40 28", "stimulus_period_frames": period,
        "time_start": clock_text(first_ms), "last_major_frame_time": clock_text(last_ms),
        "time_end_exclusive": clock_text(end_ms), "duration_seconds": frames/FPS,
        "config_gb18030_sha256": file_hash(output/"config.hea"),
        "config_utf8_sha256": file_hash(output/"config_utf8.hea"),
        "generator_sha256": file_hash(Path(__file__)),
        "base_generator_sha256": file_hash(Path(__file__).with_name("generate_all_methods_dataset.py")),
        "parameters": coverage, "samples": reports,
    }
    json_file(output/"manifest.json", manifest)
    verification = {
        "passed": True, "raw_size_verified": (output/"raw.pcm").stat().st_size == raw_bytes,
        "whole_file_sha256_verified": True, "raw_sha256": verified_hash,
        "checked_raw_bytes": raw_bytes, "sample_reports": reports,
        "decoded_rows": sum(report["rows"] for report in reports),
        "decoded_cells": sum(report["cells"] for report in reports),
        "decoder_sha256": file_hash(decoder), "manifest_sha256": file_hash(output/"manifest.json"),
        "scope": "全文件大小及 SHA-256；首中尾各 64 个长周全部参数解算，不是全量 TXT 比对",
    }
    json_file(output/"verification.json", verification)
    readme = [
        "# 1000 参数、20 GiB 全解算方式数据", "",
        f"业务参数 {count} 个，加上 HOUR_1/MINUTE_1/SECOND_1/MSSECOND_1，共 {count+4} 个参数。",
        "保留原有 40 种解算布局并新增 RATE16；地址独立，组间刺激序列错开相位。", "",
        "## 加载文件", "",
        "- 配置：config.hea，GB18030 编码，使用旧版 CFTE 段名及校准字段。",
        "- config_utf8.hea：同内容 UTF-8 配置。",
        "- 按本文件最长普通字段名补最少前导空格，冒号统一对齐。",
        "- bits_order 按旧版固定语法保留冒号前的 1 个空格，其余字段冒号紧跟名称。",
        "- The format description of stream N: 和 the_parameter_description_of_stream N: 段标题顶格书写。",
        f"- 原始数据：raw.pcm，{raw_bytes:,} 字节，即 {raw_bytes/1024**3:g} GiB。",
        "- 参数名统一为参数名_流号，例如 UINT8_1、UINT8_G02_1、HOUR_1。",
        "- 参数组：parameters.txt，包含 TIME 和全部业务参数。",
        "- parameters_quick.txt：TIME 与 UINT16_1，可用于小输出量的全文件处理。",
        f"- 完整频次解算：用户处理采样率填 {SAMPLE_RATE}，可勾选原码输出。",
        "- 首次验证可选择 samples/begin/raw.pcm 并使用同一配置和参数组，输出对应其 expected.txt。", "",
        "## 帧与时间", "",
        f"- {frames:,} 个完整长周；每子帧 {WORDS} 字，每长周 {DEPTH} 个子帧。",
        f"- 每长周 {FRAME_BYTES} 字节；每秒 {FPS} 个长周。",
        f"- 同步字落盘为 6B FE 40 28，子帧 ID 位于零基第 2 字，从 0 到 {DEPTH-1}。",
        "- 所有 PCM 字均为 16 位小端；rate 表示每长周出现次数。",
        "- 长周率为 8 Hz；rate 为 1/2/4/8/16，参数实际采样率为 8/16/32/64/128 Hz。",
        "- 完整频次处理采样率为 128 Hz，所有采样相关频率均为 2 的幂。",
        f"- 开始时间 {clock_text(first_ms)}，最后长周 {clock_text(last_ms)}。",
        f"- 时间范围右端 {clock_text(end_ms)}，总时长 {frames/FPS:g} 秒，无跨日回退。",
        f"- 业务刺激每 {period} 个长周循环；时间码持续递增。", "",
        "## 覆盖与验证", "",
        "- 保留全部原码、校准、字节交换、目标位重排/重叠/留空、跨子帧、六段拼字与 1/2/4/8/16 次采样。",
        "- 主配置为 DM6 时间码；其他设备时间码的专项夹具位于原 generated/all_methods/timecodes。",
        "- 全文件写入后重新读取校验 SHA-256，同时确认精确字节数。",
        "- 首部、中部、尾部各抽取 64 个真实长周，经生产控制器解算，逐格对照独立刺激模型。",
        "- 每个样本包含 1024 行，原码和工程值均比较；64 位整数精确比较。",
        "- 时间输出采用旧版 TIME 列和 HH:mm:ss:zzz 格式，毫秒以下截断。",
        "- expected 保留精确秒数；校验时将时间转换为旧版格式，与 actual 逐行比对。",
        "- samples 下保存抽样 PCM、expected.txt、actual.txt、decode.log；verification.json 保存报告。",
        "- 未运行全量 1000 参数 TXT 输出；该输出的行数为长周数乘 16，大小显著高于抽样结果。",
        "- 本数据为合成验证夹具，未与真实设备采集文件或旧版可执行程序输出逐行对照。", "",
        "## 重新生成", "",
        "在仓库根目录执行，运行前将构建所用 Qt 和 MinGW 的 bin 目录加入 PATH：", "",
        "    python tools/generate_large_all_methods_dataset.py "
        "--decoder build-legacy-verify/pcm_dataset_decode.exe",
        "",
        "默认参数为 --parameters 1000 --gib 20；--output 可指定输出目录。",
        "生成过程写入 raw.pcm.partial，完成后发布 raw.pcm；进度保存在 progress.json。",
        "",
    ]
    text_file(output/"README.md", "\n".join(readme))
    progress(output, "completed", raw_bytes=raw_bytes, total_parameters=count+4, **verification)


def verify_existing(output, decoder):
    """复用已生成的 20 GiB 文件，按新解算器重做完整性与首中尾验证。"""
    manifest = json.loads((output/"manifest.json").read_text(encoding="utf-8"))
    if (output/"raw.pcm").stat().st_size != manifest["raw_bytes"]:
        raise ValueError("原始文件大小已改变")
    if file_hash(output/"config.hea") != manifest["config_gb18030_sha256"]:
        raise ValueError("配置文件已改变，请重新生成相符的夹具")
    verification = json.loads((output/"verification.json").read_text(encoding="utf-8"))
    json_file(output/"verification.json", {"passed": None, "description": "正在重新校验"})
    verify_raw(output, manifest["raw_sha256"])
    if (manifest["subframe_words"], manifest["subframe_depth"], manifest["major_frame_rate"]) != (WORDS, DEPTH, FPS):
        raise ValueError("现有数据布局与本生成器不一致，请先重新生成")
    parameters = expand_parameters(manifest["business_parameters"])
    reports = verify_samples(output, parameters, manifest["frame_count"], decoder)
    manifest["samples"] = reports
    manifest["time_output_format"] = "TIME / HH:mm:ss:zzz"
    json_file(output/"manifest.json", manifest)
    verification.update(
        passed=True, sample_reports=reports, decoded_rows=sum(report["rows"] for report in reports),
        decoded_cells=sum(report["cells"] for report in reports), decoder_sha256=file_hash(decoder),
        whole_file_sha256_verified=True, manifest_sha256=file_hash(output/"manifest.json"))
    json_file(output/"verification.json", verification)
    readme = output/"README.md"
    content = readme.read_text(encoding="utf-8")
    explanation = ("\n时间输出采用旧版 TIME 列和 HH:mm:ss:zzz 格式，毫秒以下截断。\n"
                   "expected.txt 保留高精度秒数作为独立预期；校验时转换为旧版格式逐行比较。\n")
    if "时间输出采用旧版 TIME 列" not in content:
        text_file(readme, content+explanation)
    progress(output, "completed", raw_bytes=manifest["raw_bytes"], **verification)


def main():
    """读取规模选项；出现 I/O、布局或比对错误时保存失败状态并返回非零退出码。"""
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--output", type=Path, default=root/"generated/all_methods_1000_20g")
    parser.add_argument("--parameters", type=int, default=1000)
    parser.add_argument("--gib", type=int, default=20)
    parser.add_argument("--decoder", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    options = parser.parse_args()
    if options.parameters < 1000:
        parser.error("--parameters 至少为 1000")
    if options.gib <= 0:
        parser.error("--gib 必须为正整数")
    output = options.output.resolve()
    try:
        decoder = options.decoder.resolve()
        if not decoder.is_file():
            raise ValueError("找不到解算验证程序")
        if options.verify_only:
            verify_existing(output, decoder)
        else:
            generate(output, options.parameters, options.gib*1024**3, decoder)
        return 0
    except (OSError, ValueError, subprocess.TimeoutExpired, ArithmeticError) as error:
        if output.is_dir():
            json_file(output/"verification.json", {"passed": False, "error": str(error)})
            progress(output, "failed", error=str(error))
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
