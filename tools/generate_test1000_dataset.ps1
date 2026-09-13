param(
    [string]$OutputDirectory = "D:\kaifa\newPcmProcess\generated\test1000",
    [ValidateRange(1, 2147483647)]
    [int]$FrameCount = 1024,
    [ValidateScript({ $_ -ge 0 })]
    [long]$TargetBytes = 0
)

$ErrorActionPreference = "Stop"

$parameter_count = 1000
$stream_index = 1
$word_length_bits = 16
$word_bytes = 2
$frame_length_words = 2048
$frame_bytes = $frame_length_words * $word_bytes
$subframe_depth = 1
$major_frame_rate_hz = 32
$sync_first_word = 0xFE6B
$sync_second_word = 0x2840
# Configuration frame_no and word_no are zero-based, just like buffer offsets.
$parameter_start_word_zero_based = 16
$parameter_start_word_one_based = $parameter_start_word_zero_based + 1

if ($TargetBytes -gt 0) {
    if ($PSBoundParameters.ContainsKey("FrameCount")) {
        throw "Specify either TargetBytes or FrameCount, not both."
    }
    $required_frames = [long][Math]::Ceiling($TargetBytes / [double]$frame_bytes)
    if ($required_frames -gt [int]::MaxValue) {
        throw "TargetBytes exceeds the supported frame count."
    }
    $FrameCount = [int]$required_frames
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$config_path = Join-Path $OutputDirectory "test1000_config.txt"
$raw_path = Join-Path $OutputDirectory "test1000.pcm"
$group_path = Join-Path $OutputDirectory "test1000_parameter_groups.json"
$manifest_path = Join-Path $OutputDirectory "test1000_manifest.json"

function Get-ParameterId {
    param([int]$Index)
    return "P{0:D4}" -f $Index
}

function Get-ParameterName {
    param([int]$Index)
    return "PARAM_{0:D4}" -f $Index
}

function Get-SixColumnValue {
    param([int]$FirstValue)
    return "$FirstValue,0,0,0,0,0"
}

$utf8_no_bom = [System.Text.UTF8Encoding]::new($false)

$writer = [System.IO.StreamWriter]::new($config_path, $false, $utf8_no_bom)
try {
    $writer.WriteLine("#CFTE/filemain/rec01.00")
    $writer.WriteLine("Header_informations:")
    $writer.WriteLine("Create_date:2026.09.11 00:00:00")
    $writer.WriteLine("test_id: TEST_1000_PARAMS")
    $writer.WriteLine("stream_number: 1")
    $writer.WriteLine("derpar_number: 0")
    $writer.WriteLine("The format description of stream 1:")
    $writer.WriteLine("aircraft_name: TEST_BENCH")
    $writer.WriteLine("test_name: TEST_1000_PARAMS")
    $writer.WriteLine("parameter_number: $parameter_count")
    $writer.WriteLine("bit_rate: 0.000000")
    $writer.WriteLine("pcm_code: NRZ-L")
    $writer.WriteLine("loop_width: 0.100000")
    $writer.WriteLine("word_length: $word_length_bits")
    $writer.WriteLine("parity: NONE")
    $writer.WriteLine("Onboard_acq._equipments_type: generated")
    $writer.WriteLine("frame_length: $frame_length_words")
    $writer.WriteLine("subframe_deepth: $subframe_depth")
    $writer.WriteLine("major_frame_rate: $major_frame_rate_hz")
    $writer.WriteLine("sync_word_length: 32")
    $writer.WriteLine("sync_pattern: 0xFE6B2840")
    $writer.WriteLine("id_position: 3")
    $writer.WriteLine("id_lsb: 0")
    $writer.WriteLine("dating_type: counter")
    $writer.WriteLine("time_interpolation: 0")
    $writer.WriteLine("output_polarity: normal")
    $writer.WriteLine("input_source: internal_bs")
    $writer.WriteLine("input_polarity: normal")
    $writer.WriteLine("bits_order: normal")
    $writer.WriteLine("save_last_frame: 0")
    $writer.WriteLine("sync_strategy: normal")
    $writer.WriteLine("sync_location: 0")
    $writer.WriteLine("subframe_mode: sfid")
    $writer.WriteLine("direction: increasing counter")
    $writer.WriteLine("first_value: 0")
    $writer.WriteLine("pattern_loop: 0")
    $writer.WriteLine("the_parameter_description_of_stream_1:")

    for ($index = 1; $index -le $parameter_count; ++$index) {
        $word_no = $parameter_start_word_zero_based + $index - 1
        $parameter_id = Get-ParameterId -Index $index
        $parameter_name = Get-ParameterName -Index $index
        $writer.WriteLine("serial_no.: $index")
        $writer.WriteLine("id_no.: $parameter_id")
        $writer.WriteLine("name: $parameter_name")
        $writer.WriteLine("unit: raw")
        $writer.WriteLine("cal_type: NO")
        $writer.WriteLine("code_type: uint")
        $writer.WriteLine("cal_date: 2026-09-11")
        $writer.WriteLine("cal_number:")
        $writer.WriteLine("rate: 32")
        $writer.WriteLine("word_interval: $($frame_length_words * $subframe_depth)")
        $writer.WriteLine("sign_bit: 0")
        $writer.WriteLine("inverse: 0")
        $writer.WriteLine("pcm_words: 1")
        $writer.WriteLine("frame_no: $(Get-SixColumnValue -FirstValue 0)")
        $writer.WriteLine("word_no: $(Get-SixColumnValue -FirstValue $word_no)")
        $writer.WriteLine("source_bit: $(Get-SixColumnValue -FirstValue 0)")
        $writer.WriteLine("bit_length: $(Get-SixColumnValue -FirstValue 16)")
        $writer.WriteLine("object_bit: $(Get-SixColumnValue -FirstValue 0)")
        $writer.WriteLine("description: Generated test parameter $index; value = (frame_index + $index) mod 65536")
    }
}
finally {
    $writer.Dispose()
}

# Keep the per-word loop in compiled code and bound memory to an 8 MiB block.
if (-not ("Test1000PcmWriter" -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot "test1000_pcm_writer.cs")
}
[Test1000PcmWriter]::Generate(
    $raw_path, $FrameCount, $frame_length_words, $subframe_depth, $major_frame_rate_hz,
    $parameter_start_word_zero_based, $parameter_count, $sync_first_word, $sync_second_word)

$parameter_ids = New-Object System.Collections.Generic.List[string]
for ($index = 1; $index -le $parameter_count; ++$index) {
    $parameter_ids.Add((Get-ParameterId -Index $index))
}

$groups = [ordered]@{
    version = 1
    config_file = [System.IO.Path]::GetFileName($config_path)
    raw_file = [System.IO.Path]::GetFileName($raw_path)
    byte_order = "little-endian 16-bit words"
    groups = @(
        [ordered]@{
            group_id = "all_1000"
            display_name = "All 1000 parameters"
            stream_index = 1
            parameter_ids = $parameter_ids.ToArray()
        },
        [ordered]@{
            group_id = "first_100"
            display_name = "First 100 parameters"
            stream_index = 1
            parameter_ids = $parameter_ids.GetRange(0, 100).ToArray()
        },
        [ordered]@{
            group_id = "last_100"
            display_name = "Last 100 parameters"
            stream_index = 1
            parameter_ids = $parameter_ids.GetRange($parameter_count - 100, 100).ToArray()
        }
    )
}
$groups | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $group_path -Encoding UTF8

$manifest = [ordered]@{
    generated_at = [DateTimeOffset]::Now.ToString("o")
    description = "Single-stream PCM test dataset with 1000 uint parameters."
    files = [ordered]@{
        raw_file = $raw_path
        config_file = $config_path
        parameter_groups_file = $group_path
    }
    stream = [ordered]@{
        stream_index = 1
        sync_pattern_logical = "0xFE6B2840"
        sync_pattern_bytes_on_disk = "6B FE 40 28"
        byte_order = "little-endian 16-bit words"
        frame_length_words = $frame_length_words
        frame_length_bytes = $frame_bytes
        frame_count = $FrameCount
        subframe_depth = $subframe_depth
        major_frame_rate_hz = $major_frame_rate_hz
        raw_file_bytes = [long]$frame_bytes * $FrameCount
    }
    parameters = [ordered]@{
        count = $parameter_count
        id_range = @("P0001", "P1000")
        config_index_base = 0
        frame_no = 0
        first_parameter_word_zero_based = $parameter_start_word_zero_based
        last_parameter_word_zero_based = $parameter_start_word_zero_based + $parameter_count - 1
        first_parameter_word_one_based = $parameter_start_word_one_based
        last_parameter_word_one_based = $parameter_start_word_one_based + $parameter_count - 1
        value_formula = "value = (frame_index + parameter_index_1_based) mod 65536"
    }
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifest_path -Encoding UTF8

[pscustomobject]@{
    OutputDirectory = $OutputDirectory
    RawFile = $raw_path
    RawBytes = (Get-Item -LiteralPath $raw_path).Length
    ConfigFile = $config_path
    ConfigBytes = (Get-Item -LiteralPath $config_path).Length
    ParameterGroupsFile = $group_path
    ParameterCount = $parameter_count
    FrameCount = $FrameCount
}
