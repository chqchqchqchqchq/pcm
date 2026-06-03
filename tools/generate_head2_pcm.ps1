param(
    [string]$OutputPath = "D:\kaifa\newPcmProcess\generated\head2_15gb.pcm",
    [string]$ProgressPath = "D:\kaifa\newPcmProcess\generated\head2_15gb.progress.json",
    [string]$DonePath = "D:\kaifa\newPcmProcess\generated\head2_15gb.done",
    [string]$ErrorPath = "D:\kaifa\newPcmProcess\generated\head2_15gb.error.txt",
    [long]$TargetBytes = 15000000000
)

$ErrorActionPreference = "Stop"

$source = @"
using System;
using System.Diagnostics;
using System.IO;
using System.Text;

public static class Head2PcmGenerator
{
    private const int Stream1FrameBytes = 1024;
    private const int Stream2FrameBytes = 512;
    private const int CycleBytes = Stream1FrameBytes + Stream2FrameBytes;
    private const int BufferCycles = 8192;
    private const int BufferBytes = BufferCycles * CycleBytes;

    public static int Generate(string outputPath, string progressPath, string donePath, string errorPath, long targetBytes)
    {
        try
        {
            if (targetBytes <= 0 || targetBytes % CycleBytes != 0)
            {
                throw new ArgumentException("TargetBytes must be a positive multiple of 1536.");
            }

            long cycleCount = targetBytes / CycleBytes;

            Directory.CreateDirectory(Path.GetDirectoryName(outputPath));
            Directory.CreateDirectory(Path.GetDirectoryName(progressPath));

            if (File.Exists(donePath))
            {
                File.Delete(donePath);
            }

            if (File.Exists(errorPath))
            {
                File.Delete(errorPath);
            }

            var buffer = new byte[BufferBytes];
            var stopwatch = Stopwatch.StartNew();
            long written = 0;
            long cycle = 0;
            long nextProgressAt = 0;

            using (var stream = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.Read,
                       BufferBytes, FileOptions.SequentialScan))
            {
                while (cycle < cycleCount)
                {
                    int cyclesThisBlock = (int)Math.Min(BufferCycles, cycleCount - cycle);
                    int blockBytes = cyclesThisBlock * CycleBytes;

                    for (int blockCycle = 0; blockCycle < cyclesThisBlock; ++blockCycle)
                    {
                        long absoluteCycle = cycle + blockCycle;
                        int offset = blockCycle * CycleBytes;
                        WriteFrame(buffer, offset, Stream1FrameBytes, absoluteCycle, 1);
                        WriteFrame(buffer, offset + Stream1FrameBytes, Stream2FrameBytes, absoluteCycle, 2);
                    }

                    stream.Write(buffer, 0, blockBytes);
                    written += blockBytes;
                    cycle += cyclesThisBlock;

                    if (written >= nextProgressAt || cycle >= cycleCount)
                    {
                        WriteProgress(progressPath, written, targetBytes, cycle, cycleCount, stopwatch.Elapsed);
                        nextProgressAt = written + 256L * 1024L * 1024L;
                    }
                }

                stream.Flush(true);
            }

            File.WriteAllText(donePath,
                string.Format("output={0}\nbytes={1}\ncycles={2}\nelapsed_seconds={3:F3}\n",
                    outputPath, targetBytes, cycleCount, stopwatch.Elapsed.TotalSeconds),
                Encoding.UTF8);
            return 0;
        }
        catch (Exception ex)
        {
            File.WriteAllText(errorPath, ex.ToString(), Encoding.UTF8);
            return 1;
        }
    }

    private static void WriteFrame(byte[] buffer, int offset, int frameBytes, long cycle, int streamIndex)
    {
        int words = frameBytes / 2;
        if (streamIndex == 2)
        {
            WriteWord(buffer, offset, 0, 0xFE68);
            WriteWord(buffer, offset, 1, 0x2841);
        }
        else
        {
            WriteWord(buffer, offset, 0, 0xFE6B);
            WriteWord(buffer, offset, 1, 0x2840);
        }

        WriteWord(buffer, offset, 2, (ushort)(cycle & 0x0003));

        long totalMilliseconds = (cycle * 1000L) / 32L;
        long secondsOfDay = (totalMilliseconds / 1000L) % 86400L;
        ushort hour = (ushort)(secondsOfDay / 3600L);
        ushort minute = (ushort)((secondsOfDay / 60L) % 60L);
        ushort second = (ushort)(secondsOfDay % 60L);
        ushort millisecond = (ushort)(totalMilliseconds % 1000L);

        WriteWord(buffer, offset, 3, hour);
        WriteWord(buffer, offset, 4, minute);
        WriteWord(buffer, offset, 5, second);
        WriteWord(buffer, offset, 6, millisecond);

        ushort previous = millisecond;
        for (int word = 7; word < words; ++word)
        {
            ushort value = (ushort)((cycle + (long)word * 17L + streamIndex * 257L) & 0xFFFF);
            if ((previous == 0xFE6B && value == 0x2840)
                || (previous == 0xFE68 && value == 0x2841))
            {
                value = (ushort)(value + 1);
            }

            WriteWord(buffer, offset, word, value);
            previous = value;
        }
    }

    private static void WriteWord(byte[] buffer, int frameOffset, int wordIndex, ushort value)
    {
        int byteIndex = frameOffset + (wordIndex * 2);
        buffer[byteIndex] = (byte)(value & 0xFF);
        buffer[byteIndex + 1] = (byte)(value >> 8);
    }

    private static void WriteProgress(string path, long written, long targetBytes, long cycles, long cycleCount, TimeSpan elapsed)
    {
        double percent = written * 100.0 / targetBytes;
        double mib = written / 1024.0 / 1024.0;
        double mibPerSecond = elapsed.TotalSeconds > 0.0 ? mib / elapsed.TotalSeconds : 0.0;
        string json = string.Format(System.Globalization.CultureInfo.InvariantCulture,
            "{{\"bytes_written\":{0},\"target_bytes\":{1},\"cycles_written\":{2},\"cycle_count\":{3},\"percent\":{4:F4},\"elapsed_seconds\":{5:F3},\"mib_per_second\":{6:F3},\"updated_at\":\"{7:O}\"}}",
            written, targetBytes, cycles, cycleCount, percent, elapsed.TotalSeconds, mibPerSecond, DateTimeOffset.Now);
        File.WriteAllText(path, json, Encoding.UTF8);
    }
}
"@

Add-Type -TypeDefinition $source -Language CSharp
exit [Head2PcmGenerator]::Generate($OutputPath, $ProgressPath, $DonePath, $ErrorPath, $TargetBytes)
