#!/usr/bin/env python3
"""使用旧版 fscanf 格式和原始 read_subf 探针检查配置及原始文件打开路径。"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess


def legacy_header(path):
    """逐项执行旧版主对话框的 CFTE 头部 fscanf，所有转换必须成功且字段顺序正确。"""
    crt = ctypes.CDLL("msvcrt") if os.name == "nt" else ctypes.CDLL(None)
    crt.fclose.argtypes = [ctypes.c_void_p]
    crt.fscanf.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    crt.fscanf.restype = ctypes.c_int
    if os.name == "nt":
        crt._wfopen.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
        crt._wfopen.restype = ctypes.c_void_p
        handle = crt._wfopen(str(path.resolve()), "r")
    else:
        crt.fopen.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        crt.fopen.restype = ctypes.c_void_p
        handle = crt.fopen(os.fsencode(path), b"r")
    if not handle:
        raise OSError(f"无法打开配置：{path}")
    scans = []

    def scan(key, fmt, kinds):
        """为旧版格式串分配安全目标缓冲，并核对实际转换数和首词元。"""
        values = [ctypes.create_string_buffer(4096) if kind == "s" else
                  ctypes.c_int() if kind == "i" else ctypes.c_double() for kind in kinds]
        converted = crt.fscanf(handle, fmt.encode("ascii"),
                               *(value if kind == "s" else ctypes.byref(value)
                                 for kind, value in zip(kinds, values)))
        decoded = [value.value.decode("gb18030", errors="replace") if kind == "s" else value.value
                   for kind, value in zip(kinds, values)]
        if converted != len(kinds):
            raise ValueError(f"{key}: 旧版 fscanf 仅转换 {converted}/{len(kinds)} 项，读到 {decoded}")
        if key and key != decoded[0]:
            raise ValueError(f"旧版期待 {key}，实际读到 {decoded[0]}")
        scans.append({"key": key or decoded[0], "values": decoded[1:]})
        return decoded

    try:
        scan("#CFTE/filemain/rec01.00", "%s \n", "s")
        scan("Header_informations:", "%s \n", "s")
        date = scan(None, "%s %s\n", "ss")
        if not date[0].startswith("Create_date:"):
            raise ValueError("旧版创建日期字段错位")
        scan("test_id:", "%s %s\n", "ss")
        streams = scan("stream_number:", "%s %d\n", "si")[1]
        if streams != 1:
            raise ValueError("此验证入口要求单流数据")
        scan("derpar_number:", "%s %d\n", "si")
        title = scan("The", "%s %s %s %s %s %s\n", "ssssss")
        if title != ["The", "format", "description", "of", "stream", "1:"]:
            raise ValueError("流格式段标题不符")
        settings = {}
        fields = [("aircraft_name","s"),("test_name","s"),("parameter_number","i"),
                  ("bit_rate","d"),("pcm_code","s"),("loop_width","d"),("word_length","i"),
                  ("parity","s"),("Onboard_acq._equipments_type","s"),("frame_length","i"),
                  ("subframe_deepth","i"),("major_frame_rate","i"),("sync_word_length","d"),
                  ("sync_pattern","s"),("id_position","i"),("id_lsb","i"),("dating_type","s"),
                  ("time_interpolation","d"),("output_polarity","s"),("input_source","s"),
                  ("input_polarity","s"),("bits_order","s"),("save_last_frame","d"),
                  ("sync_strategy","s"),("sync_location","d"),("subframe_mode","s"),
                  ("direction","s"),("first_value","d"),("pattern_loop","d")]
        for key, kind in fields:
            if key == "bits_order":
                result = scan(key, "%s :%s\n", "ss")
            elif key == "direction":
                result = scan(key+":", "%s %s %s\n", "sss")
            else:
                format_type = {"s":"s","i":"d","d":"lf"}[kind]
                result = scan(key+":", "%s %"+format_type+"\n", "s"+kind)
            settings[key] = result[1]
        scan("the_parameter_description_of_stream", "%s %s\n", "ss")
        return settings, scans
    finally:
        crt.fclose(handle)


def verify(directory, probe):
    """核对配置边界、最少前导空格、参数个数，并让旧版函数实际读取首尾。"""
    config = directory/"config.hea"
    settings, scans = legacy_header(config)
    words, depth = settings["frame_length"], settings["subframe_deepth"]
    if not (0 < words <= 2048 and words*depth <= 32767):
        raise ValueError("超出旧版帧缓冲或有符号时间地址容量")
    content = config.read_bytes().decode("gb18030")
    if content != (directory/"config_utf8.hea").read_bytes().decode("utf-8"):
        raise ValueError("两种编码的配置内容不同")
    titles = ("The format description of stream", "the_parameter_description_of_stream")
    fields = [line for line in content.splitlines() if ":" in line and not line.startswith(titles)]
    width = max(len(line.partition(":")[0].strip()) for line in fields)
    for line in fields:
        key, _, value = line.partition(":")
        syntax_key = key.strip()+(" " if key.strip() == "bits_order" else "")
        if line.index(":") != width or key != syntax_key.rjust(width):
            raise ValueError(f"字段未按最少空格对齐：{line}")
    names = [line.partition(":")[2].strip() for line in fields
             if line.partition(":")[0].strip() == "name"]
    if any(not name.endswith("_1") for name in names) or len(set(names)) != len(names):
        raise ValueError("参数名必须唯一且带当前流号后缀 _1")
    parameters = len(names)
    if parameters != settings["parameter_number"] or parameters < 1004:
        raise ValueError("参数数量与配置声明不一致")
    result = subprocess.run([str(probe.resolve()), str((directory/"raw.pcm").resolve()),
                             str(words), str(depth), str(settings["id_position"])],
                            capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise ValueError(f"旧版读帧探针退出码 {result.returncode}: {result.stdout} {result.stderr}")
    report = {"passed": True, "scope": "旧版头部 fscanf 格式及原始 read_subf 首尾打开路径；非旧版 GUI 全参数解算",
              "frame_words": words, "subframes": depth, "parameters": parameters,
              "parameter_name_suffix_verified": True,
              "raw_bytes": (directory/"raw.pcm").stat().st_size,
              "config_sha256": hashlib.sha256(config.read_bytes()).hexdigest(),
              "probe_sha256": hashlib.sha256(probe.read_bytes()).hexdigest(),
              "delimiter_column": width+1, "checked_fields": len(fields),
              "minimal_padding_verified": True, "header_scans": scans,
              "old_read_subf_output": result.stdout}
    (directory/"legacy_open_verification.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    return report


def main():
    """运行旧版打开兼容检查，失败时返回非零退出码。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    options = parser.parse_args()
    report = verify(options.directory, options.probe)
    print(json.dumps({key:value for key,value in report.items() if key != "header_scans"},
                     ensure_ascii=False))


if __name__ == "__main__":
    main()
