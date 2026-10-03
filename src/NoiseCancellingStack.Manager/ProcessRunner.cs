// SPDX-License-Identifier: MIT
using System.Diagnostics;

namespace NoiseCancellingStack;

public static class ProcessRunner
{
    // ArgumentList avoids command-shell parsing for paths, task names, and registry destinations.
    public static int Run(string fileName, IEnumerable<string> arguments, bool hidden, OperationLogger? logger = null)
    {
        var start = new ProcessStartInfo
        {
            FileName = fileName,
            UseShellExecute = false,
            CreateNoWindow = hidden,
            WindowStyle = hidden ? ProcessWindowStyle.Hidden : ProcessWindowStyle.Normal,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var argument in arguments)
        {
            start.ArgumentList.Add(argument);
        }

        using var process = Process.Start(start)
            ?? throw new InvalidOperationException($"Could not start {fileName}.");
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        process.WaitForExit();
        Task.WaitAll(stdout, stderr);

        if (!string.IsNullOrWhiteSpace(stdout.Result))
        {
            logger?.Write($"{Path.GetFileName(fileName)}: {stdout.Result.Trim()}");
        }
        if (!string.IsNullOrWhiteSpace(stderr.Result))
        {
            logger?.Write($"{Path.GetFileName(fileName)} error: {stderr.Result.Trim()}");
        }

        return process.ExitCode;
    }
}
