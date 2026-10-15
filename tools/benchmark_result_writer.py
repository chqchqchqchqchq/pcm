"""串行、交替运行基线/候选输出器；校验在计时之外，只有相同字节的运行才进入汇总。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    """分块计算 SHA-256，不加载整个结果文件。"""
    value = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for block in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def save(path, value):
    """每轮完成后持久化测量记录，异常时保留此前数据。"""
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def export_reference(args):
    """只从本地 Git 复制输出器的两个文件，不切换或修改工作区。"""
    revision = subprocess.check_output(
        ["git", "rev-parse", "--verify", args.revision + "^{commit}"], cwd=ROOT, text=True).strip()
    target = args.output.resolve()
    target.mkdir(parents=True, exist_ok=False)
    hashes = {}
    for relative in ("include/result_writer.h", "src/result_writer.cpp"):
        data = subprocess.check_output(["git", "show", revision + ":" + relative], cwd=ROOT)
        path = target / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        hashes[relative] = hashlib.sha256(data).hexdigest()
    save(target / "reference.json", {"revision": revision, "files": hashes})
    print(target, flush=True)


def inspect_output(path):
    """以二进制读取统计实际行列和摘要，保留平台换行差异。"""
    sha = hashlib.sha256()
    lines = 0
    with path.open("rb") as handle:
        first = handle.readline()
        columns = first.count(b"\t") + 1
        sha.update(first)
        lines += first.count(b"\n")
        for block in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            sha.update(block)
            lines += block.count(b"\n")
    return {"sha256": sha.hexdigest(), "bytes": path.stat().st_size, "rows": lines - 1, "columns": columns}


def run(args):
    """同盘输出，不清空系统缓存；预热一次，正式顺序 AB / BA 交替。"""
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "platform": platform.platform(), "processor": platform.processor(), "logical_cpus": os.cpu_count(),
        "cache": "不清空系统缓存；每个场景各预热一次，正式顺序交替；每次仅运行一个子进程。",
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_sha256": {p: digest(ROOT / p) for p in ("src/result_writer.cpp", "include/result_writer.h")},
        "executables": {}, "inputs": {}, "runs": [], "summary": {}, "passed": False
    }
    cases = []
    if args.baseline_writer and args.candidate_writer:
        for profile in args.profiles:
            cases.append(("writer_" + profile, "writer", profile))
        report["executables"].update({"writer_baseline": str(args.baseline_writer.resolve()),
                                      "writer_candidate": str(args.candidate_writer.resolve())})
    if args.baseline_decoder and args.candidate_decoder:
        if not all((args.config, args.raw_file, args.parameters_file)):
            raise ValueError("解算基准需要 --config、--raw-file 和 --parameters-file")
        cases.append(("decode", "decode", None))
        report["executables"].update({"decode_baseline": str(args.baseline_decoder.resolve()),
                                      "decode_candidate": str(args.candidate_decoder.resolve())})
        for key in ("config", "raw_file", "parameters_file"):
            path = getattr(args, key).resolve()
            report["inputs"][key] = {"path": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}
    if not cases:
        raise ValueError("至少提供一对基线/候选可执行文件")
    report["executable_sha256"] = {key: digest(path) for key, path in report["executables"].items()}
    for name, kind, profile in cases:
        expected = None
        for round_index in range(args.repeats + 1):
            order = ("baseline", "candidate") if round_index % 2 == 0 else ("candidate", "baseline")
            for label in order:
                target = output / f"{name}_{round_index}_{label}.txt"
                executable = report["executables"][kind + "_" + label]
                command = [executable, "--output", str(target)]
                if kind == "writer":
                    command += ["--rows", str(args.rows), "--parameters", str(args.columns - 1),
                                "--profile", profile]
                    if args.include_raw:
                        command += ["--raw"]
                else:
                    command += ["--config", str(args.config.resolve()), "--raw", str(args.raw_file.resolve()),
                                "--parameters", str(args.parameters_file.resolve()), "--sample-rate", "1",
                                "--timeout-seconds", str(args.timeout)]
                    if not args.include_raw:
                        command += ["--engineering-only"]
                print(f"{name} round={round_index} {label}", flush=True)
                start = time.perf_counter()
                process = subprocess.run(command, capture_output=True, timeout=args.timeout + 30)
                wall_ms = (time.perf_counter() - start) * 1000
                log = process.stderr.decode("utf-8", errors="replace")
                record = {"case": name, "round": round_index, "warmup": round_index == 0,
                          "label": label, "command": command, "wall_ms": wall_ms, "log": log,
                          "returncode": process.returncode}
                report["runs"].append(record)
                save(output / "measurements.json", report)
                if process.returncode:
                    raise RuntimeError(log)
                if kind == "writer":
                    record["metrics"] = json.loads(process.stdout)
                else:
                    match = re.search(r"处理耗时 (\d+) ms：读取等待 (\d+) ms，参数解算 (\d+) ms，结果写入 (\d+) ms", log)
                    if not match:
                        raise ValueError("未找到生产路径的读/解算/写入计时")
                    record["metrics"] = dict(zip(("worker_ms", "read_wait_ms", "decode_ms", "writer_ms"),
                                                  map(int, match.groups())))
                record["validation"] = inspect_output(target)
                expected_columns = 1 + (args.columns - 1) * (2 if args.include_raw else 1)
                if record["validation"]["rows"] != args.rows or record["validation"]["columns"] != expected_columns:
                    raise ValueError("实际输出行/列数与预期不符")
                if expected is None:
                    expected = record["validation"]
                record["byte_identical"] = record["validation"] == expected
                save(output / "measurements.json", report)
                if not record["byte_identical"]:
                    raise ValueError("输出与本场景基线不一致，不进入性能排名；保留失败输出")
                # 只删除本次新目录中、当前命令生成且校验成功的一个文件。
                if not args.keep_outputs:
                    target.unlink()
                print(f"writer={record['metrics']['writer_ms']:.3f} ms; sha256={expected['sha256']}", flush=True)
        summary = {}
        for label in ("baseline", "candidate"):
            measured = [r for r in report["runs"] if r["case"] == name and r["label"] == label and not r["warmup"]]
            summary[label] = {}
            for metric in ("writer_ms", "wall_ms"):
                values = [r[metric] if metric == "wall_ms" else r["metrics"][metric] for r in measured]
                summary[label][metric] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        left, right = summary["baseline"]["writer_ms"], summary["candidate"]["writer_ms"]
        summary["writer_reduction_percent"] = 100 * (1 - right["median"] / left["median"])
        summary["clear_writer_advantage"] = right["max"] < left["min"] and summary["writer_reduction_percent"] >= 5
        report["summary"][name] = summary
    report["passed"] = True
    save(output / "measurements.json", report)
    print(json.dumps(report["summary"], ensure_ascii=False, indent=2), flush=True)


def verify(args):
    """只读首中尾夹具，对照独立刺激模型；另测输出器取消清理，不覆盖原有验证文件。"""
    from generate_all_methods_dataset import compare

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    dataset = args.dataset.resolve()
    decoder = args.decoder.resolve()
    report = {"passed": False, "decoder_sha256": digest(decoder), "samples": [], "abort_cleanup": []}
    for name in ("begin", "middle", "end"):
        source = dataset / "samples" / name
        target = output / (name + ".txt")
        command = [str(decoder), "--config", str(dataset / "config.hea"), "--raw", str(source / "raw.pcm"),
                   "--parameters", str(dataset / "parameters.txt"), "--sample-rate", "128",
                   "--output", str(target), "--timeout-seconds", "120"]
        process = subprocess.run(command, capture_output=True, timeout=150)
        if process.returncode:
            raise RuntimeError(process.stderr.decode("utf-8", errors="replace"))
        rows, cells = compare(source / "expected.txt", target)
        record = {"sample": name, "rows": rows, "cells": cells, "command": command,
                  "output_sha256": digest(target), "raw_sha256": digest(source / "raw.pcm"),
                  "expected_sha256": digest(source / "expected.txt"), "passed": True}
        report["samples"].append(record)
        save(output / "verification.json", report)
        print(f"{name}: {rows} rows, {cells} cells passed", flush=True)
    if args.writer:
        report["writer_sha256"] = digest(args.writer.resolve())
        for index in range(3):
            target = output / f"abort_{index}.txt"
            command = [str(args.writer.resolve()), "--output", str(target), "--rows", "512", "--abort"]
            process = subprocess.run(command, capture_output=True, timeout=120)
            if process.returncode or target.exists():
                raise RuntimeError("输出器取消后残留目标文件或退出失败")
            result = json.loads(process.stdout)
            if not result["aborted"]:
                raise ValueError("基准没有执行取消")
            report["abort_cleanup"].append(result)
            save(output / "verification.json", report)
    report["passed"] = True
    save(output / "verification.json", report)
    print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)


def main():
    """解析导出参考源码或串行性能测试命令。"""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    reference = commands.add_parser("export-reference")
    reference.add_argument("--revision", required=True)
    reference.add_argument("--output", type=Path, required=True)
    reference.set_defaults(action=export_reference)
    validation = commands.add_parser("verify")
    validation.add_argument("--dataset", type=Path, required=True)
    validation.add_argument("--decoder", type=Path, required=True)
    validation.add_argument("--writer", type=Path)
    validation.add_argument("--output", type=Path, required=True)
    validation.set_defaults(action=verify)
    measure = commands.add_parser("run")
    for name in ("baseline-writer", "candidate-writer", "baseline-decoder", "candidate-decoder",
                 "config", "raw-file", "parameters-file"):
        measure.add_argument("--" + name, type=Path)
    measure.add_argument("--output", type=Path, required=True)
    measure.add_argument("--rows", type=int, default=81920)
    measure.add_argument("--columns", type=int, default=1005)
    measure.add_argument("--profiles", nargs="+", choices=("mixed", "integer", "double"), default=["mixed"])
    measure.add_argument("--include-raw", action="store_true")
    measure.add_argument("--repeats", type=int, default=3)
    measure.add_argument("--timeout", type=int, default=600)
    measure.add_argument("--keep-outputs", action="store_true")
    measure.set_defaults(action=run)
    args = parser.parse_args()
    if args.command == "run" and (args.repeats < 1 or args.rows < 1 or args.columns < 2 or args.timeout < 1):
        parser.error("计数与超时时间必须为正数，列数至少为 2")
    args.action(args)


if __name__ == "__main__":
    main()
