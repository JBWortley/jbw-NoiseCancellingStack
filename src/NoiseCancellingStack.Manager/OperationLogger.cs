// SPDX-License-Identifier: MIT
namespace NoiseCancellingStack;

public interface IOperationLogger
{
    void Write(string message);
}

public sealed class OperationLogger : IOperationLogger, IDisposable
{
    private readonly StreamWriter writer;
    private readonly bool quiet;

    public OperationLogger(string operation, bool quiet)
    {
        // Secure the parent before choosing or opening a log filename; the process runs elevated.
        this.quiet = quiet;
        SecureStorage.Ensure();
        Directory.CreateDirectory(ProductConstants.LogDirectory);
        var safeOperation = string.Concat(operation.Where(c => char.IsLetterOrDigit(c) || c == '-'));
        LogPath = Path.Combine(
            ProductConstants.LogDirectory,
            $"{DateTime.Now:yyyyMMdd-HHmmss-fff}-{safeOperation}.log");
        writer = new StreamWriter(LogPath, append: false) { AutoFlush = true };
        Write($"{ProductConstants.ProductName} {ProductConstants.ProductVersion}");
        Write($"Operation: {operation}");
        Write($"Started: {DateTimeOffset.Now:O}");
    }

    public string LogPath { get; }

    public void Write(string message)
    {
        var line = $"[{DateTimeOffset.Now:O}] {message}";
        writer.WriteLine(line);
        if (!quiet)
        {
            Console.WriteLine(message);
        }
    }

    public void Dispose() => writer.Dispose();
}
