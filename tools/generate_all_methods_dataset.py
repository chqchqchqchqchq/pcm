#!/usr/bin/env python3
"""生成符合旧版 CFTE 规则的完整参数解算夹具，可用真实控制器逐格校验。"""

import argparse
import csv
import hashlib
import itertools
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from decimal import Decimal


@dataclass
class Parameter:
    """参数刺激值与物理布局；期望值来自刺激值，不通过 PCM 解码器计算。"""

    name: str
    values: list
    segments: list
    code: str = "uint"
    calibration: str = "NO"
    extra: list = field(default_factory=list)
    inverse: int = 0
    rate: int = 1
    note: str = ""
    engineering: object = None
    fragments: object = None

    def stimulus(self, frame, occurrence=0):
        """按参数实际采样序号循环产生边界值。"""
        return self.values[(frame * self.rate + occurrence) % len(self.values)]

    def expected(self, frame, occurrence=0):
        """独立返回原码解释值和工程值。"""
        value = self.stimulus(frame, occurrence)
        raw = value * 10 / 32768 if self.code == "fcs618" else value
        return raw, self.engineering(raw) if self.engineering else raw

    def encoded(self, value):
        """将指定刺激值编码为位模式，仅供写入数据使用。"""
        width = sum(segment[3] for segment in self.segments)
        if self.fragments:
            return self.fragments(value)
        if self.calibration in ("float", "double") or self.code in ("float", "fcsf"):
            if width == 32:
                bits = int.from_bytes(struct.pack(">f", value), "big")
            elif width == 64:
                bits = int.from_bytes(struct.pack(">d", value), "big")
            else:
                bits = int(value) & 0xffff
        elif self.code == "bcd":
            bits = int(str(value), 16)
        elif self.code == "xint":
            bits = abs(value) | ((1 << (width - 1)) if value < 0 else 0)
        else:
            bits = int(value) & ((1 << width) - 1)
        parts = []
        remaining = width
        for _, _, _, length, _ in self.segments:
            remaining -= length
            parts.append((bits >> remaining) & ((1 << length) - 1))
        return parts


def text_file(path, content, encoding="utf-8"):
    """先写临时文件，再替换本生成器的目标文件。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".partial")
    temporary.write_bytes(content.replace("\r\n", "\n").replace("\n", "\r\n").encode(encoding))
    temporary.replace(path)


def json_file(path, data):
    """保存可读的 UTF-8 清单。"""
    text_file(path, json.dumps(data, ensure_ascii=False, indent=2) + "\n")


def segment(word, length=16, source=0, target=0, frame=0):
    """返回零基索引的 frame/word/source/length/object 五元组。"""
    return (frame, word, source, length, target)


def main_parameters():
    """覆盖全部原码、校准及拼字分支，保持不同参数占用位置独立。"""
    parameters = []
    next_word = 8

    def add(name, values, width=16, code="uint", **kwargs):
        """为普通参数顺序分配最多四个 PCM 字。"""
        nonlocal next_word
        lengths = [16] * (width // 16) + ([width % 16] if width % 16 else [])
        segments = [segment(next_word + index, length) for index, length in enumerate(lengths)]
        next_word += len(lengths)
        result = Parameter(name, values, segments, code=code, **kwargs)
        parameters.append(result)
        return result

    add("UINT8", [0, 1, 127, 128, 255], 8, note="8 位无符号边界")
    add("UINT16", [0, 1, 32767, 32768, 65535], note="16 位无符号边界")
    add("UINT32", [0, 1, 0x12345678, 0x80000000, 0xffffffff], 32, note="两字拼接")
    add("UINT64", [0, 1, (1 << 53) + 1, 1 << 63, (1 << 64) - 1], 64, note="64 位整数精度")
    for width in (8, 12, 16, 32, 64):
        add(f"SINT{width}", [-(1 << (width - 1)), -1, 0, 1, (1 << (width - 1)) - 1],
            width, "sint", note=f"{width} 位补码；符号由有效位宽确定")
    add("BCD16", [0, 1, 1234, 9009, 9999], code="bcd", note="4 位十进制 BCD")
    add("BCD32", [0, 12345678, 90000001, 99999999], 32, "bcd", note="8 位十进制 BCD")
    add("BCD64", [0, 1234567890123456, 9999999999999999], 64, "bcd", note="16 位十进制 BCD")
    add("XINT16", [-32767, -1, 0, 1, 32767], code="xint", note="最高位符号加绝对值")
    add("XINT32", [-2147483647, -1234567, 0, 1234567, 2147483647], 32, "xint")
    add("FCS618", [-32768, -16384, -1, 0, 1, 16384, 32767], code="fcs618",
        note="有符号 16 位计数乘 10/32768；raw 列也是解释后的值")
    add("FCSF16", [-32768, -123, -1, 0, 123, 32767], code="fcsf", note="旧版飞控 16 位为补码整数")
    floating = [-65536.5, -1.25, -0.0, 0.0, 0.125, 3.5, 1024.25]
    add("FCSF32", floating, 32, "fcsf", note="IEEE754 单精度")
    add("FCSF64", floating + [math.pi], 64, "fcsf", note="IEEE754 双精度")
    add("FLOAT_CODE32", floating, 32, "float", note="code_type=float 别名")
    add("FLOAT_CAL32", floating, 32, calibration="float", note="cal_type=float 覆盖 code_type=uint")
    add("DOUBLE_CAL64", floating + [math.pi], 64, calibration="double",
        note="cal_type=double 覆盖 code_type=uint")
    add("LINE", [-100, -1, 0, 1, 100], code="sint", calibration="line",
        extra=[("coefficient", "1.5,2.25,0,0,0,0")], engineering=lambda x: 1.5 + 2.25 * x,
        note="一次线性校准；常数项在前")
    add("LINE_SIX", [-3, -2, -1, 0, 1, 2, 3], code="sint", calibration="line",
        extra=[("coefficient", "1,-2,3,-4,5,-6")],
        engineering=lambda x: 1 - 2*x + 3*x**2 - 4*x**3 + 5*x**4 - 6*x**5,
        note="旧版 line 同样接受六项多项式")
    add("POLY", [-3, -2, -1, 0, 1, 2, 3], code="sint", calibration="poly",
        extra=[("coefficient", "-2,3,-0.5,0.25,-0.125,0.0625")],
        engineering=lambda x: -2 + 3*x - .5*x**2 + .25*x**3 - .125*x**4 + .0625*x**5,
        note="五次多项式，覆盖六个非零系数")
    points = [("segment_number", "3"), ("code,phy", "-10,-100"),
              ("code,phy", "0,10"), ("code,phy", "10,50")]
    piecewise = lambda x: 11*x + 10 if x <= 0 else 4*x + 10
    add("SEGM", [-20, -10, -5, 0, 5, 10, 20], code="sint", calibration="segm",
        extra=points, engineering=piecewise, note="左右端点、插值、两端外推")
    add("FILLCODE", [-20, -10, -5, 0, 5, 10, 20], calibration="fillcode",
        extra=points, engineering=piecewise, note="fillcode 强制补码解释，再分段校准")
    add("HYPER", [-10, -1, 0, 4, 5, 6, 20], code="sint", calibration="hyper",
        extra=[("segment_point", "5"), ("first_segment", "120,2,1,3"),
               ("second_segment", "40,20,-1,-2")],
        engineering=lambda x: 120/(2+x)+3 if x >= 5 else 40/(20-x)-2,
        note="阈值以下、等于阈值、阈值以上")
    add("INVERSE16", [0x1234, 0xabcd, 0x0102, 0xff00], inverse=1, note="提取前交换每个字的两个字节")
    inverse_field = add("INVERSE_FIELD", [0, 1, 0x5a, 0xa5, 255], 8, inverse=1,
        note="先交换字节，再从 source_bit=4 取 8 位")
    inverse_field.segments[0] = segment(next_word-1, 8, 4)
    source_field = add("SOURCE_BIT", [0, 1, 21, 42, 63], 6, note="LSB 起算，非零源偏移")
    source_field.segments[0] = segment(next_word-1, 6, 5)
    add("CONCAT24", [0, 1, 0x123456, 0xffffff], 24, note="所有 object_bit=0，按段顺序高到低拼接")
    gap = add("OBJECT_GAP", [0x0a03, 0x050c, 0x0f0f], 8, note="目标位 8/0，中间留空",
              fragments=lambda x: [x >> 8, x & 15])
    gap.segments = [segment(next_word-1, 4, 0, 8), segment(next_word-1, 4, 8, 0)]
    overlap = add("OBJECT_OR", [0x2f], 8, note="目标位重叠按 OR：0xA<<2 | 0x7<<1 | 1",
                  fragments=lambda x: [10, 7, 1])
    overlap.segments = [segment(next_word-1, 4, 0, 2), segment(next_word-1, 4, 4, 1),
                        segment(next_word-1, 1, 8, 0)]
    reverse = add("OBJECT_REORDER32", [0x12345678, 0xaabbccdd, 0xffffffff], 32,
                  note="显式目标位重排；低字在前，高字在后",
                  fragments=lambda x: [x & 0xffff, x >> 16])
    reverse.segments = [segment(next_word-2, target=0), segment(next_word-1, target=16)]
    cross = add("CROSS_SUBFRAME", [0x12345678, 0xabcdef01, 0x80000001], 32,
                note="两段分布在子帧 0 与子帧 3")
    cross.segments[1] = segment(next_word-1, frame=3)
    six = add("SIX_SEGMENTS", [0x123456789abc, 0xffffffffffff, 0x010203040506], 48,
              note="全部六个有效拼字段，各取 8 位")
    six.segments = [segment(next_word-3+i//2, 8, (i % 2)*8) for i in range(6)]
    for rate, frame in ((1, 2), (2, 1), (4, 0), (8, 3)):
        item = add(f"RATE{rate}", list(range(100*rate, 100*rate+16)), rate=rate,
                   note=f"每长周出现 {rate} 次；低频补点、高频子帧内重复")
        item.segments[0] = segment(next_word-1, frame=frame)
    assert next_word < 128
    return parameters


def align_config_text(content):
    """按最长字段名补最少前导空格对齐冒号；流格式和参数描述段标题顶格书写。"""
    section_prefixes = ("The format description of stream", "the_parameter_description_of_stream")
    lines = [line.strip() if line.lstrip().startswith(section_prefixes) else line
             for line in content.splitlines()]
    fields = [(index, line.partition(":")) for index, line in enumerate(lines)
              if ":" in line and not line.lstrip().startswith(("#",)+section_prefixes)]
    # 旧版 fscanf("%s :%s") 要求 bits_order 与冒号间有一个语法空格。
    keys = {index: parts[0].strip()+(" " if parts[0].strip() == "bits_order" else "")
            for index, parts in fields}
    width = max(map(len, keys.values()), default=0)
    for index, (_, _, value) in fields:
        lines[index] = keys[index].rjust(width)+":"+value
    return "\n".join(lines)+"\n"


def parameter_name_for_stream(name, stream_index=1):
    """将参数名规范为参数名_流号，已有当前流号后缀时不重复追加。"""
    if stream_index < 1:
        raise ValueError("流号必须为正整数")
    suffix = f"_{stream_index}"
    return name if name.endswith(suffix) else name+suffix


def config_text(parameters, equipment="DM6", fps=4, words=256, depth=4, word_length=16, stream_index=1):
    """输出旧版准确段名、Description 及重复 code,phy 行。"""
    text = ["#CFTE/filemain/rec01.00", "Header_informations:", "Create_date:2026.09.14 00:00:00",
            "test_id: ALL_METHODS", "stream_number: 1", "derpar_number: 0",
            f"The format description of stream {stream_index}:"]
    fields = {
        "aircraft_name": "TEST_BENCH", "test_name": "ALL_METHODS", "parameter_number": len(parameters)+4,
        "bit_rate": fps*words*depth*16, "pcm_code": "NRZ-L", "loop_width": "0.1",
        "word_length": word_length, "parity": "NONE", "Onboard_acq._equipments_type": equipment,
        "frame_length": words, "subframe_deepth": depth, "major_frame_rate": fps,
        "sync_word_length": 32, "sync_pattern": "0xFE6B2840", "id_position": 2, "id_lsb": 0,
        "dating_type": "counter", "time_interpolation": 0, "output_polarity": "normal",
        "input_source": "internal_bs", "input_polarity": "normal", "bits_order": "normal",
        "save_last_frame": 0, "sync_strategy": "normal", "sync_location": 0, "subframe_mode": "sfid",
        "direction": "increasing counter", "first_value": 0, "pattern_loop": 0,
    }
    text.extend(f"{key}: {value}" for key, value in fields.items())
    text.append(f"the_parameter_description_of_stream {stream_index}:")
    time_parameters = [
        Parameter(f"{name}_{stream_index}", [0], [segment(index+3)], note="设备时间原始存储字")
        for index, name in enumerate(("HOUR", "MINUTE", "SECOND", "MSSECOND"))
    ]
    for serial, parameter in enumerate(time_parameters + parameters, 1):
        fields = {
            "serial_no.": serial, "id_no.": f"{serial:04d}",
            "name": parameter_name_for_stream(parameter.name, stream_index), "unit": "raw",
            "cal_type": parameter.calibration, "code_type": parameter.code, "cal_date": "2026-09-14",
            "cal_number": "SYNTHETIC", "rate": parameter.rate, "word_interval": 0,
            "sign_bit": 0, "inverse": parameter.inverse, "pcm_words": len(parameter.segments),
        }
        for column, key in enumerate(("frame_no", "word_no", "source_bit", "bit_length", "object_bit")):
            values = [str(part[column]) for part in parameter.segments]
            fields[key] = ",".join(values + ["0"]*(6-len(values)))
        text.extend(f"{key}: {value}" for key, value in fields.items())
        text.extend(f"{key}: {value}" for key, value in parameter.extra)
        text.append(f"Description: {parameter.note or parameter.name}")
    return align_config_text("\n".join(text) + "\n")


def time_words(equipment, milliseconds):
    """由目标时刻直接编码 10 种设备时间字，包括时分进位。"""
    seconds, ms = divmod(milliseconds, 1000)
    h, remainder = divmod(seconds, 3600)
    m, s = divmod(remainder, 60)
    if equipment == "秦川":
        return [(h//10 << 7) | (h % 10 << 3) | m//10,
                (m % 10 << 7) | (s//10 << 4) | s % 10, 0, 0]
    if equipment == "KAM500":
        return [(h//10 << 11) | (h % 10 << 7) | (m//10 << 4) | m % 10,
                (s//10 << 12) | (s % 10 << 8) | (ms//100 << 4) | (ms//10 % 10),
                ms % 10 << 12, 0]
    if equipment == "770":
        return [(h//10 << 4) | h % 10,
                (m//10 << 13) | (m % 10 << 9) | (s//10 << 6) | (s % 10 << 2) | (ms//100)//4,
                ((ms//100) % 4 << 14) | (ms//10 % 10 << 10) | (ms % 10 << 6), 0]
    if equipment == "JQD":
        return [(h//10 << 12) | (h % 10 << 8) | (m//10 << 4) | m % 10,
                (s//10 << 13) | (s % 10 << 9), ms << 6, 0]
    if equipment == "UMA2000":
        ticks, tail = divmod(milliseconds, 10)
        return [ticks >> 16, ticks & 0xffff, tail*1000, 0]
    if equipment in ("XFC100", "XMA"):
        microseconds = milliseconds*1000
        return [microseconds >> 32, (microseconds >> 16) & 0xffff, microseconds & 0xffff, 0]
    if equipment == "XYD":
        return [0, (h << 7) | m, (s << 8) | ms, 0]
    if equipment == "TTC":
        ticks = milliseconds//10
        return [ticks >> 16, ticks & 0xffff, 0, 0]
    return [h, m, s, ms]


def write_raw(path, parameters, frames, base_ms, fps=4, words=256, depth=4, equipment="DM6"):
    """流式生成完整长周，检查同步字、时间字与各参数之间没有物理位冲突。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".pcm.partial")
    with temporary.open("wb") as output:
        for index in range(frames):
            data = [0]*(words*depth)
            masks = [0]*(words*depth)
            for subframe in range(depth):
                for word, value in enumerate((0xfe6b, 0x2840, subframe)):
                    address = subframe*words+word
                    data[address], masks[address] = value, 0xffff
            for offset, value in enumerate(time_words(equipment, base_ms + index*1000//fps), 3):
                data[offset], masks[offset] = value, 0xffff
            for parameter in parameters:
                for occurrence in range(parameter.rate):
                    fragments = parameter.encoded(parameter.stimulus(index, occurrence))
                    for part, fragment in zip(parameter.segments, fragments):
                        frame, word, source, length, _ = part
                        # 写入刺激的实际位置；与期望值计算没有解码依赖。
                        if parameter.rate <= depth:
                            address = frame*words + word + occurrence*(depth*words//parameter.rate)
                        else:
                            repeats = parameter.rate//depth
                            address = (occurrence//repeats)*words + word + occurrence % repeats*(words//repeats)
                        mask = ((1 << length)-1) << source
                        value = fragment << source
                        if parameter.inverse:
                            mask = ((mask & 255) << 8) | (mask >> 8)
                            value = ((value & 255) << 8) | (value >> 8)
                        if masks[address] & mask:
                            raise ValueError(f"{parameter.name}: 第 {address} 字存在重叠写入")
                        # 未参与解算的位填 1，防止遗漏掩码仍偶然算对。
                        free_mask = (~(masks[address] | mask)) & 0xffff
                        data[address] = (data[address] & ~mask) | value | free_mask
                        masks[address] |= mask
            output.write(struct.pack("<" + "H"*len(data), *data))
    temporary.replace(path)


def number(value):
    """整数保持完整精度，浮点数使用足够往返的有效数字。"""
    return str(value) if isinstance(value, int) else format(value, ".17g")


def expected_file(path, parameters, frames, base_ms, fps, maximum, slots, stride, start=0, end=-1):
    """从刺激表和明确的输出采样位置产生期望，包含原码列与工程列。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, delimiter="\t", lineterminator="\n")
        writer.writerow(["timestamp_seconds"] + list(itertools.chain.from_iterable(
            (p.name+"_raw", p.name) for p in parameters)))
        first_frame = max(0, math.ceil((start-base_ms/1000)*fps - 1e-9))
        for frame in range(first_frame, frames, stride):
            base = (base_ms + frame*1000//fps)/1000
            for slot in slots:
                timestamp = base + slot/(fps*maximum)
                if timestamp < start or (end >= 0 and timestamp >= end):
                    continue
                row = [f"{timestamp:.9f}"]
                for parameter in parameters:
                    occurrence = slot//(maximum//parameter.rate)
                    row.extend(number(v) for v in parameter.expected(frame, occurrence))
                writer.writerow(row)
                count += 1
    return count


def file_hash(path):
    """有界缓冲计算 SHA-256。"""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024*1024), b""):
            digest.update(block)
    return digest.hexdigest()


def generate(output, frames):
    """生成主配置、时间码配置、12 位配置、组文件、预期值及清单。"""
    parameters = main_parameters()
    runs = []
    cases = []
    output.mkdir(parents=True, exist_ok=True)

    def dataset(folder, items, count, base_ms, fps=4, words=256, depth=4, equipment="DM6", word_length=16):
        """保存一组实际可以从 GUI 加载的配置和 PCM。"""
        directory = output/folder
        for parameter in items:
            parameter.name = parameter_name_for_stream(parameter.name)
        config = directory/"config.hea"
        text = config_text(items, equipment, fps, words, depth, word_length)
        text_file(config, text, "gb18030")
        text_file(directory/"config_utf8.hea", text)
        text_file(directory/"parameters.txt", "\n".join(["TIME"]+[p.name for p in items])+"\n")
        write_raw(directory/"raw.pcm", items, count, base_ms, fps, words, depth, equipment)
        cases.append({"directory": folder, "equipment": equipment, "parameters": len(items)+4,
                      "frames": count, "major_frame_rate": fps, "subframe_depth": depth,
                      "frame_words": words, "physical_word_bits": 16, "effective_word_bits": word_length,
                      "base_time_seconds": base_ms/1000, "duration_seconds": count/fps,
                      "raw_bytes": count*words*depth*2})
        return directory

    def run_case(name, folder, items, count, base_ms, fps, maximum, rate, slots, stride=1,
                 start=0, end=-1, utf8=False):
        """记录一项验证任务；采样位置由用例明确指定。"""
        expected = output/"expected"/f"{name}.txt"
        rows = expected_file(expected, items, count, base_ms, fps, maximum, slots, stride, start, end)
        runs.append({"name": name, "config": f"{folder}/config{'_utf8' if utf8 else ''}.hea",
                     "raw": f"{folder}/raw.pcm", "parameters": f"{folder}/parameters.txt",
                     "sample_rate": rate, "start": start, "end": end,
                     "expected": f"expected/{name}.txt", "expected_rows": rows})

    base_ms = 3723000
    dataset("main", parameters, frames, base_ms)
    for rate, slots, stride in ((32, list(range(8)), 1), (16, [0, 2, 4, 6], 1),
                                (12, [0, 3, 6], 1), (20, [0, 2, 4, 6], 1),
                                (4, [0], 1), (3, [0], 1), (2, [0], 2)):
        run_case(f"main_rate{rate}", "main", parameters, frames, base_ms, 4, 8, rate, slots, stride)
    run_case("main_utf8", "main", parameters, frames, base_ms, 4, 8, 32, list(range(8)), utf8=True)
    run_case("main_range_phase", "main", parameters, frames, base_ms, 4, 8, 2, [0], 2,
             start=3723.125, end=3724.875)
    run_case("main_range_end", "main", parameters, frames, base_ms, 4, 8, 32, list(range(8)),
             start=3723.125, end=3723.6)
    equipment_list = ("DM6", "秦川", "KAM500", "770", "JQD", "UMA2000", "XFC100", "XMA", "XYD", "TTC")
    for equipment in equipment_list:
        slug = "qinchuan" if equipment == "秦川" else equipment.lower()
        folder = f"timecodes/{slug}"
        fraction_ms = {"秦川": 0, "TTC": 780, "XYD": 123}.get(equipment, 789)
        time_ms = (12*3600+59*60+56)*1000 + fraction_ms
        items = [Parameter("TIME_CHECK", [0, 1, 32768, 65535], [segment(8)],
                           note=f"{equipment} 时间码配套计数，跨 13:00:00 验证进位")]
        dataset(folder, items, 16, time_ms, 1, 16, 1, equipment)
        run_case("time_"+slug, folder, items, 16, time_ms, 1, 1, 1, [0])
    items = [Parameter("UINT12_STORAGE16", [0, 1, 2047, 2048, 4095], [segment(8, 12)],
                       note="12 位有效数据使用 16 位小端存储，未用高位填 1"),
             Parameter("SINT12_STORAGE16", [-2048, -1, 0, 1, 2047], [segment(9, 12)], code="sint")]
    dataset("word12", items, 16, base_ms, 1, 16, 1, word_length=12)
    run_case("word12", "word12", items, 16, base_ms, 1, 1, 1, [0])
    coverage = [{"name": p.name, "code_type": p.code, "cal_type": p.calibration, "rate": p.rate,
                 "inverse": p.inverse, "segments": p.segments, "stimulus_values": p.values,
                 "calibration_fields": p.extra, "description": p.note} for p in parameters]
    files = {}
    for path in sorted(output.rglob("*")):
        if path.is_file() and (path.suffix in (".hea", ".pcm") or
                               path.name == "parameters.txt" or path.parent == output/"expected"):
            files[path.relative_to(output).as_posix()] = {"bytes": path.stat().st_size, "sha256": file_hash(path)}
    manifest = {"format_version": 1, "description": "旧版 CFTE 参数解算完整覆盖数据",
                "main_parameter_count": len(parameters)+4, "main_output_parameter_count": len(parameters),
                "cases": cases, "parameters": coverage, "runs": runs, "files": files}
    manifest["generator_sha256"] = file_hash(Path(__file__))
    json_file(output/"manifest.json", manifest)
    json_file(output/"verification.json", {"passed": None, "description": "已重新生成，尚未校验"})
    rows = ["# 全参数解算测试数据", "", "以下数据为确定性合成夹具，使用旧版 CFTE 规则。", "",
            "## 直接使用", "",
            "- 配置：main/config.hea（GB18030，旧版段名）；config_utf8.hea 为同内容 UTF-8 版本。",
            "- 按本文件最长普通字段名补最少前导空格，冒号紧跟字段名并对齐。",
        "- The format description of stream N: 和 the_parameter_description_of_stream N: 段标题顶格书写。",
            "- 原始文件：main/raw.pcm；参数组：main/parameters.txt。",
            f"- 主配置共 {len(parameters)+4} 个参数：4 个时间参数与 {len(parameters)} 个业务参数。",
            f"- {frames} 个长周；每子帧 256 字、每长周 4 个子帧、4 长周/秒；PCM 大小 {frames*2048} 字节。",
            "- GUI 选择“全程”，处理采样率填 32，并勾选原码输出，即对应 expected/main_rate32.txt。",
            "- 时间从 01:02:03.000 开始。rate 是长周内出现次数，不是 Hz。",
            "- parameters.txt 是 GUI 可导入的参数名列表，第一项 TIME。",
            "- 所有 PCM 文件均按 16 位小端存储；同步字落盘为 6B FE 40 28；帧号和字号均从 0 开始。",
            "", "## 校验与采样", "",
            "- expected/ 是独立刺激值模型产生的预期；actual/ 是真实控制器输出；verification.json 是逐格校验报告。",
            "- raw 列表示原码解释后的数值，未施加工程校准；并非无符号拼接位模式。",
            "- 整数列严格比较（含超过 2^53 的 64 位整数）；浮点相对误差 1e-14、绝对误差 1e-12。",
            "- expected 保留纳秒精度秒数；actual 使用旧版 TIME / HH:mm:ss:zzz，毫秒以下截断。",
            "- 时间列按旧版格式精确比较；表头按列名匹配，行数、列数和每格数值都必须一致。",
            "- 主数据处理采样率 32/16/12/20/4/3/2 分别验证超采步长和抽取；",
            "  请求 20 实际输出 16 行/秒，请求 3 实际输出 4 行/秒，沿用旧版整数取整规则。",
            "- 窗口 [3723.125,3724.875) 以采样率 2 验证抽取相位；",
            "  窗口 [3723.125,3723.6) 以采样率 32 验证长周定位和右开区间裁剪。",
            "- timecodes/ 覆盖 10 种设备时间码，均跨 13:00:00；秦川没有毫秒，TTC 分辨率 10 ms。",
            "- word12/ 验证有效字长 12 位、实际存储 16 位；未选中的高位填 1 以检查掩码。",
            "- 合成数据不代表真实采集设备样本，也未用旧版可执行程序做逐行对照。",
            "", "## 参数覆盖", "", "| 参数 | 原码 | 校准 | 次数/长周 | 覆盖点 |",
            "|---|---|---|---:|---|"]
    rows.extend(f"| {p.name} | {p.code} | {p.calibration} | {p.rate} | {p.note} |" for p in parameters)
    rows.extend(["", "## 重新生成", "", "在仓库根目录运行：", "",
                 "    python tools/generate_all_methods_dataset.py --frames 1024",
                 "    python tools/generate_all_methods_dataset.py --verify-only "
                 "--decoder build-legacy-verify/pcm_dataset_decode.exe",
                 "", "解算验证前，将 Qt 与 MinGW 的 bin 目录加入 PATH。解算入口由 CMake 随项目构建。",
                 "生成器只替换本目录内指定的同名文件，不清理其他数据集。", ""])
    text_file(output/"README.md", "\n".join(rows))
    return manifest


def legacy_time_text(seconds):
    """用十进制定点运算产生旧版 TIME 文本，毫秒以下截断。"""
    milliseconds = int(Decimal(str(seconds))*1000) % 86400000
    return (f"{milliseconds//3600000:02d}:{milliseconds//60000 % 60:02d}:"
            f"{milliseconds//1000 % 60:02d}:{milliseconds % 1000:03d}")


def compare(expected, actual):
    """流式逐格比较，整数用 Decimal 精确比较，浮点允许输出格式的舍入误差。"""
    with expected.open(encoding="utf-8-sig", newline="") as left, \
            actual.open(encoding="utf-8-sig", newline="") as right:
        wanted, got = csv.reader(left, delimiter="\t"), csv.reader(right, delimiter="\t")
        header, actual_header = next(wanted, None), next(got, None)
        if not header or not actual_header:
            raise ValueError("预期或实际文件缺少表头")
        if actual_header[0] != "TIME":
            raise ValueError("实际时间列必须为旧版 TIME")
        actual_header[0] = "timestamp_seconds"
        if len(set(actual_header)) != len(actual_header) or set(header) != set(actual_header):
            raise ValueError("表头列不匹配")
        positions = [actual_header.index(name) for name in header]
        cells = rows = 0
        for row_index, pair in enumerate(itertools.zip_longest(wanted, got), 2):
            expected_row, actual_row = pair
            if expected_row is None or actual_row is None:
                raise ValueError(f"第 {row_index} 行起行数不匹配")
            if len(expected_row) != len(header) or len(actual_row) != len(header):
                raise ValueError(f"第 {row_index} 行列数错误")
            for column, actual_column in enumerate(positions):
                if column == 0:
                    wanted_time = legacy_time_text(expected_row[column])
                    if actual_row[actual_column] != wanted_time:
                        raise ValueError(f"第 {row_index} 行 TIME：期望 {wanted_time}，"
                                         f"实际 {actual_row[actual_column]}")
                    cells += 1
                    continue
                a, b = Decimal(expected_row[column]), Decimal(actual_row[actual_column])
                tolerance = Decimal("0")
                if any(c in expected_row[column].lower() for c in ".e"):
                    tolerance = max(Decimal("1e-12"), abs(a)*Decimal("1e-14"))
                if not a.is_finite() or not b.is_finite() or abs(a-b) > tolerance:
                    raise ValueError(f"第 {row_index} 行 {header[column]}：期望 {a}，实际 {b}")
                cells += 1
            rows += 1
        return rows, cells


def verify(output, manifest, decoder):
    """调用和 GUI 相同的控制器入口，核对全部实际输出并记录可审阅报告。"""
    reports = []
    for relative, metadata in manifest["files"].items():
        if file_hash(output/relative) != metadata["sha256"]:
            raise ValueError(f"夹具文件校验失败：{relative}")
    actual_dir = output/"actual"
    actual_dir.mkdir(exist_ok=True)
    for run in manifest["runs"]:
        destination = actual_dir/(run["name"]+".txt")
        command = [str(decoder), "--config", str(output/run["config"]), "--raw", str(output/run["raw"]),
                   "--parameters", str(output/run["parameters"]), "--output", str(destination),
                   "--sample-rate", str(run["sample_rate"]), "--start", str(run["start"]), "--end", str(run["end"])]
        report = {"name": run["name"], "passed": False}
        try:
            process = subprocess.run(command, capture_output=True, timeout=120)
            (actual_dir/(run["name"]+".log")).write_bytes(process.stdout + process.stderr)
            if process.returncode != 0:
                raise ValueError(f"解算进程退出码 {process.returncode}，详见 actual/{run['name']}.log")
            rows, cells = compare(output/run["expected"], destination)
            if rows != run["expected_rows"]:
                raise ValueError("行数与清单不符")
            report.update(passed=True, rows=rows, cells=cells)
        except (OSError, ValueError, subprocess.TimeoutExpired, ArithmeticError) as error:
            report["error"] = str(error)
        reports.append(report)
        print(json.dumps(report, ensure_ascii=True), flush=True)
    result = {"passed": all(report["passed"] for report in reports), "decoder": str(decoder),
              "manifest_sha256": file_hash(output/"manifest.json"),
              "decoder_sha256": file_hash(decoder), "runs": reports,
              "checked_cells": sum(report.get("cells", 0) for report in reports)}
    json_file(output/"verification.json", result)
    return result["passed"]


def main():
    """解析生成/校验选项并将失败转换为非零退出码。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1]/"generated/all_methods")
    parser.add_argument("--frames", type=int, default=1024, help="主数据长周数，至少 16")
    parser.add_argument("--decoder", type=Path, help="CMake 构建的 pcm_dataset_decode 可执行文件")
    parser.add_argument("--verify-only", action="store_true")
    options = parser.parse_args()
    if options.frames < 16:
        parser.error("--frames 至少为 16，以覆盖全部刺激值")
    if options.verify_only and not options.decoder:
        parser.error("--verify-only 需要 --decoder")
    output = options.output.resolve()
    try:
        manifest = (json.loads((output/"manifest.json").read_text(encoding="utf-8")) if options.verify_only
                    else generate(output, options.frames))
        print(json.dumps({"directory": str(output), "cases": len(manifest["cases"]),
                          "runs": len(manifest["runs"]), "parameters": manifest["main_parameter_count"]}))
        if options.decoder and not verify(output, manifest, options.decoder.resolve()):
            return 1
        return 0
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
