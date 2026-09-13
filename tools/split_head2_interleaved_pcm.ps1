param(
    [string]$OriginalPath = "D:\kaifa\newPcmProcess\generated\head2_15gb.pcm",
    [string]$InterleavedPath = "D:\kaifa\newPcmProcess\generated\head2_15gb_interleaved.pcm",
    [string]$Stream2Path = "D:\kaifa\newPcmProcess\generated\head2_15gb_2.pcm",
    [string]$ProgressPath = "D:\kaifa\newPcmProcess\generated\head2_split.progress.json",
    [string]$DonePath = "D:\kaifa\newPcmProcess\generated\head2_split.done",
    [string]$ErrorPath = "D:\kaifa\newPcmProcess\generated\head2_split.error.txt"
)

$ErrorActionPreference = "Stop"

$source = @"
using System;
using System.Diagnostics;
using System.IO;
using System.Text;

public static class Head2InterleavedSplitter
{
    private const int Stream1FrameBytes = 1024;
    private const int Stream2FrameBytes = 512;
    private const int CycleBytes = Stream1FrameBytes + Stream2FrameBytes;
    private const int BufferCycles = 8192;

    public static int Split(string interleavedPath,
                            string stream1Path,
                            string stream2Path,
                            string progressPath,
                            string donePath,
                            string errorPath)
    {
        try
        {
            if (File.Exists(donePath))
            {
                File.Delete(donePath);
            }

            if (File.Exists(errorPath))
            {
                File.Delete(errorPath);
            }

            Directory.CreateDirectory(Path.GetDirectoryName(stream1Path));
            Directory.CreateDirectory(Path.GetDirectoryName(stream2Path));
            Directory.CreateDirectory(Path.GetDirectoryName(progressPath));

            long sourceBytes = new FileInfo(interleavedPath).Length;
            if (sourceBytes <= 0 || sourceBytes % CycleBytes != 0)
            {
                throw new ArgumentException("Interleaved source size must be a positive multiple of 1536 bytes.");
            }

            long cycleCount = sourceBytes / CycleBytes;
            long stream1Bytes = cycleCount * Stream1FrameBytes;
            long stream2Bytes = cycleCount * Stream2FrameBytes;

            byte[] inputBuffer = new byte[BufferCycles * CycleBytes];
            byte[] stream1Buffer = new byte[BufferCycles * Stream1FrameBytes];
            byte[] stream2Buffer = new byte[BufferCycles * Stream2FrameBytes];

            Stopwatch stopwatch = Stopwatch.StartNew();
            long cyclesDone = 0;
            long bytesRead = 0;
            long nextProgressAt = 0;

            using (FileStream input = new FileStream(interleavedPath, FileMode.Open, FileAccess.Read, FileShare.Read,
                       inputBuffer.Length, FileOptions.SequentialScan))
            using (FileStream stream1 = new FileStream(stream1Path, FileMode.Create, FileAccess.Write, FileShare.Read,
                       stream1Buffer.Length, FileOptions.SequentialScan))
            using (FileStream stream2 = new FileStream(stream2Path, FileMode.Create, FileAccess.Write, FileShare.Read,
                       stream2Buffer.Length, FileOptions.SequentialScan))
            {
                while (cyclesDone < cycleCount)
                {
                    int cyclesThisBlock = (int)Math.Min(BufferCycles, cycleCount - cyclesDone);
                    int inputBytes = cyclesThisBlock * CycleBytes;
                    ReadExactly(input, inputBuffer, inputBytes);

                    for (int index = 0; index < cyclesThisBlock; ++index)
                    {
                        Buffer.BlockCopy(inputBuffer,
                                         index * CycleBytes,
                                         stream1Buffer,
                                         index * Stream1FrameBytes,
                                         Stream1FrameBytes);
                        Buffer.BlockCopy(inputBuffer,
                                         (index * CycleBytes) + Stream1FrameBytes,
                                         stream2Buffer,
                                         index * Stream2FrameBytes,
                                         Stream2FrameBytes);
                    }

                    stream1.Write(stream1Buffer, 0, cyclesThisBlock * Stream1FrameBytes);
                    stream2.Write(stream2Buffer, 0, cyclesThisBlock * Stream2FrameBytes);

                    cyclesDone += cyclesThisBlock;
                    bytesRead += inputBytes;

                    if (bytesRead >= nextProgressAt || cyclesDone >= cycleCount)
                    {
                        WriteProgress(progressPath,
                                      bytesRead,
                                      sourceBytes,
                                      cyclesDone,
                                      cycleCount,
                                      stream1Bytes,
                                      stream2Bytes,
                                      stopwatch.Elapsed);
                        nextProgressAt = bytesRead + 256L * 1024L * 1024L;
                    }
                }

                stream1.Flush(true);
                stream2.Flush(true);
            }

            File.WriteAllText(donePath,
                string.Format("source={0}\nstream1={1}\nstream2={2}\nsource_bytes={3}\nstream1_bytes={4}\nstream2_bytes={5}\ncycles={6}\nelapsed_seconds={7:F3}\n",
                    interleavedPath, stream1Path, stream2Path, sourceBytes, stream1Bytes, stream2Bytes,
                    cycleCount, stopwatch.Elapsed.TotalSeconds),
                Encoding.UTF8);
            return 0;
        }
        catch (Exception ex)
        {
            File.WriteAllText(errorPath, ex.ToString(), Encoding.UTF8);
            return 1;
        }
    }

    private static void ReadExactly(FileStream input, byte[] buffer, int count)
    {
        int offset = 0;
        while (offset < count)
        {
            int read = input.Read(buffer, offset, count - offset);
            if (read <= 0)
            {
                throw new EndOfStreamException("Unexpected EOF while reading interleaved PCM data.");
            }

            offset += read;
        }
    }

    private static void WriteProgress(string path,
                                      long bytesRead,
                                      long sourceBytes,
                                      long cyclesDone,
                                      long cycleCount,
                                      long stream1Bytes,
                                      long stream2Bytes,
                                      TimeSpan elapsed)
    {
        double percent = bytesRead * 100.0 / sourceBytes;
        double mib = bytesRead / 1024.0 / 1024.0;
        double mibPerSecond = elapsed.TotalSeconds > 0.0 ? mib / elapsed.TotalSeconds : 0.0;
        string json = string.Format(System.Globalization.CultureInfo.InvariantCulture,
            "{{\"bytes_read\":{0},\"source_bytes\":{1},\"cycles_done\":{2},\"cycle_count\":{3},\"stream1_bytes\":{4},\"stream2_bytes\":{5},\"percent\":{6:F4},\"elapsed_seconds\":{7:F3},\"mib_per_second\":{8:F3},\"updated_at\":\"{9:O}\"}}",
            bytesRead, sourceBytes, cyclesDone, cycleCount, stream1Bytes, stream2Bytes, percent,
            elapsed.TotalSeconds, mibPerSecond, DateTimeOffset.Now);
        File.WriteAllText(path, json, Encoding.UTF8);
    }
}
"@

try {
    if (-not (Test-Path -LiteralPath $InterleavedPath)) {
        if (-not (Test-Path -LiteralPath $OriginalPath)) {
            throw "Neither original source '$OriginalPath' nor interleaved backup '$InterleavedPath' exists."
        }

        Move-Item -LiteralPath $OriginalPath -Destination $InterleavedPath
    }

    Add-Type -TypeDefinition $source -Language CSharp
    exit [Head2InterleavedSplitter]::Split($InterleavedPath,
                                           $OriginalPath,
                                           $Stream2Path,
                                           $ProgressPath,
                                           $DonePath,
                                           $ErrorPath)
}
catch {
    Set-Content -LiteralPath $ErrorPath -Value $_.Exception.ToString()
    exit 1
}
