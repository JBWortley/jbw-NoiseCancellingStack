using System.Security.Principal;

namespace NoiseCancellingStack;

public static class Program
{
    private const int ExitSuccess = 0;
    private const int ExitFailure = 1;
    private const int ExitWarnings = 2;

    public static int Main(string[] args)
    {
        var operation = args.FirstOrDefault()?.ToLowerInvariant() ?? "status";
        var quiet = args.Contains("--quiet", StringComparer.OrdinalIgnoreCase);
        using var logger = new OperationLogger(operation, quiet);

        try
        {
            EnsureAdministrator();
            using var mutex = new Mutex(initiallyOwned: true, @"Global\NoiseCancellingStack.Manager", out var ownsMutex);
            if (!ownsMutex)
            {
                throw new InvalidOperationException("Another Noise Cancelling Stack operation is already running.");
            }

            var engine = new InstallerEngine(new InstallStateStore(), logger);
            return operation switch
            {
                "install" => Report(engine.InstallOrRepair(createMaintenanceTask: true), logger),
                "repair" => Report(engine.InstallOrRepair(createMaintenanceTask: !args.Contains("--scheduled", StringComparer.OrdinalIgnoreCase)), logger),
                "uninstall" => Report(engine.Uninstall(), logger),
                "status" => PrintStatus(engine.Status(), logger),
                "help" or "--help" or "-h" => PrintHelp(logger),
                _ => throw new ArgumentException($"Unknown operation: {operation}")
            };
        }
        catch (Exception ex)
        {
            logger.Write($"FAILED: {ex}");
            if (!quiet)
            {
                Console.Error.WriteLine($"See log: {logger.LogPath}");
            }
            return ExitFailure;
        }
    }

    private static void EnsureAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        var principal = new WindowsPrincipal(identity);
        if (!principal.IsInRole(WindowsBuiltInRole.Administrator))
        {
            throw new UnauthorizedAccessException("Administrator rights are required.");
        }
    }

    private static int Report(OperationReport report, OperationLogger logger)
    {
        logger.Write(
            $"Result: attached={report.Attached}, already-attached={report.AlreadyAttached}, " +
            $"restored={report.Restored}, skipped={report.Skipped}, conflicts={report.Conflicts}.");
        foreach (var message in report.Messages)
        {
            logger.Write(message);
        }
        logger.Write($"Log: {logger.LogPath}");
        return report.HasWarnings ? ExitWarnings : ExitSuccess;
    }

    private static int PrintStatus(IReadOnlyList<EndpointSummary> endpoints, OperationLogger logger)
    {
        foreach (var endpoint in endpoints)
        {
            logger.Write(
                $"{(endpoint.EqualizerApoAttached ? "ATTACHED" : "NOT ATTACHED"),-12} " +
                $"{endpoint.SampleRate,6} Hz  {endpoint.DisplayName}  {endpoint.EndpointId}");
        }
        logger.Write($"Attached {endpoints.Count(item => item.EqualizerApoAttached)} of {endpoints.Count} registered capture endpoints.");
        return ExitSuccess;
    }

    private static int PrintHelp(OperationLogger logger)
    {
        logger.Write("Usage: NoiseCancellingStack.Manager.exe <install|repair|status|uninstall> [--quiet]");
        return ExitSuccess;
    }
}
