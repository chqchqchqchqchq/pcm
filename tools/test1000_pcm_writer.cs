using System;
using System.Diagnostics;
using System.IO;

/// <summary>Writes deterministic PCM frames in bounded sequential blocks.</summary>
public static class Test1000PcmWriter
{
    /// <summary>Generates complete frames and publishes the file only after all writes succeed.</summary>
    public static void Generate(string output_path, int frame_count, int frame_words, int subframe_depth,
                                int frame_rate, int parameter_start, int parameter_count,
                                ushort sync_first, ushort sync_second)
    {
        if (frame_count <= 0 || frame_words < 7 || subframe_depth <= 0 || frame_rate <= 0
            || parameter_start < 7 || parameter_count <= 0
            || parameter_start > frame_words - parameter_count)
        {
            throw new ArgumentException("Invalid PCM frame geometry.");
        }

        int frame_bytes = checked(frame_words * 2);
        int block_frames = Math.Max(1, (8 * 1024 * 1024) / frame_bytes);
        byte[] buffer = new byte[checked(block_frames * frame_bytes)];
        string partial_path = output_path + ".partial";
        long total_bytes = (long)frame_count * frame_bytes;
        Stopwatch stopwatch = Stopwatch.StartNew();
        double next_report_seconds = 5;

        using (FileStream stream = new FileStream(partial_path, FileMode.CreateNew, FileAccess.Write,
                   FileShare.Read, buffer.Length, FileOptions.SequentialScan))
        {
            long first_frame = 0;
            while (first_frame < frame_count)
            {
                int count = (int)Math.Min(block_frames, frame_count - first_frame);
                for (int index = 0; index < count; ++index)
                {
                    long frame_index = first_frame + index;
                    int offset = index * frame_bytes;
                    WriteWord(buffer, offset, sync_first);
                    WriteWord(buffer, offset + 2, sync_second);
                    WriteWord(buffer, offset + 4, (int)(frame_index % subframe_depth));

                    long milliseconds = frame_index * 1000 / frame_rate;
                    int seconds_of_day = (int)((milliseconds / 1000) % 86400);
                    WriteWord(buffer, offset + 6, seconds_of_day / 3600);
                    WriteWord(buffer, offset + 8, seconds_of_day / 60 % 60);
                    WriteWord(buffer, offset + 10, seconds_of_day % 60);
                    WriteWord(buffer, offset + 12, (int)(milliseconds % 1000));

                    int parameter_offset = offset + parameter_start * 2;
                    for (int parameter_index = 1; parameter_index <= parameter_count; ++parameter_index)
                    {
                        int value = (int)((frame_index + parameter_index) & 0xFFFF);
                        WriteWord(buffer, parameter_offset, value);
                        parameter_offset += 2;
                    }
                }
                stream.Write(buffer, 0, count * frame_bytes);
                first_frame += count;

                if (stopwatch.Elapsed.TotalSeconds >= next_report_seconds || first_frame == frame_count)
                {
                    long written_bytes = first_frame * frame_bytes;
                    Console.WriteLine("Written {0:N0}/{1:N0} bytes ({2:F1}%), {3:F1} MiB/s",
                        written_bytes, total_bytes, written_bytes * 100.0 / total_bytes,
                        written_bytes / 1048576.0 / Math.Max(0.001, stopwatch.Elapsed.TotalSeconds));
                    next_report_seconds = stopwatch.Elapsed.TotalSeconds + 5;
                }
            }
            stream.Flush(true);
        }

        if (File.Exists(output_path))
        {
            File.Replace(partial_path, output_path, null);
        }
        else
        {
            File.Move(partial_path, output_path);
        }
        Console.WriteLine("Completed {0:N0} frames, {1:N0} bytes in {2:F1} seconds.",
            frame_count, total_bytes, stopwatch.Elapsed.TotalSeconds);
    }

    /// <summary>Stores a 16-bit PCM word independently of host byte order.</summary>
    private static void WriteWord(byte[] buffer, int offset, int value)
    {
        buffer[offset] = (byte)(value & 0xFF);
        buffer[offset + 1] = (byte)((value >> 8) & 0xFF);
    }
}
