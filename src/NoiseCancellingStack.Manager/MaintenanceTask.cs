// SPDX-License-Identifier: MIT
namespace NoiseCancellingStack;

public static class MaintenanceTask
{
    public const string TaskName = @"Noise Cancelling Stack\Register new microphones";
    public const string WatcherTaskName = @"Noise Cancelling Stack\Keep microphones filtered";

    public static void Create(string managerPath, OperationLogger logger)
    {
        if (!File.Exists(managerPath))
        {
            throw new FileNotFoundException("The installed manager executable is missing.", managerPath);
        }

        // A SYSTEM task must never point at a user-writable executable. Setup intentionally fixes
        // the installation directory so this exact protected path can be enforced here.
        var expectedPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "NoiseCancellingStack",
            "NoiseCancellingStack.Manager.exe");
        if (!string.Equals(Path.GetFullPath(managerPath), Path.GetFullPath(expectedPath), StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("Refusing to create a SYSTEM task for a manager outside the fixed Program Files location.");
        }

        Register(TaskName, BuildTaskXml(managerPath), logger);
        logger.Write("Registered the startup repair task for newly discovered microphones.");

        // Restart the watcher so it always runs the manager that was just installed.
        Register(WatcherTaskName, BuildWatcherTaskXml(managerPath), logger);
        StopWatcher(logger);
        var runExit = ProcessRunner.Run("schtasks.exe", ["/Run", "/TN", WatcherTaskName], hidden: true, logger);
        if (runExit != 0)
        {
            throw new InvalidOperationException($"Task Scheduler returned exit code {runExit} while starting the watcher.");
        }
        logger.Write("Started the watcher that keeps raw capture disabled on filtered microphones.");
    }

    public static void Delete(OperationLogger logger)
    {
        StopWatcher(logger);
        foreach (var name in new[] { WatcherTaskName, TaskName })
        {
            var exitCode = ProcessRunner.Run(
                "schtasks.exe",
                ["/Delete", "/F", "/TN", name],
                hidden: true,
                logger);
            if (exitCode is not (0 or 1))
            {
                throw new InvalidOperationException($"Task Scheduler returned exit code {exitCode} while deleting {name}.");
            }
        }

        logger.Write("Removed the startup repair and watcher tasks (or they were already absent).");
    }

    /// <summary>Ends a running watcher; harmless when the task is absent or idle.</summary>
    public static void StopWatcher(OperationLogger logger)
    {
        ProcessRunner.Run("schtasks.exe", ["/End", "/TN", WatcherTaskName], hidden: true, logger);
    }

    private static void Register(string name, string xml, OperationLogger logger)
    {
        var taskXmlPath = Path.Combine(
            ProductConstants.ProgramDataDirectory,
            $"maintenance-task.{Guid.NewGuid():N}.xml");
        try
        {
            File.WriteAllText(taskXmlPath, xml, System.Text.Encoding.Unicode);
            var exitCode = ProcessRunner.Run(
                "schtasks.exe",
                ["/Create", "/F", "/TN", name, "/XML", taskXmlPath],
                hidden: true,
                logger);
            if (exitCode != 0)
            {
                throw new InvalidOperationException($"Task Scheduler returned exit code {exitCode} for {name}.");
            }
        }
        finally
        {
            File.Delete(taskXmlPath);
        }
    }

    internal static string BuildWatcherTaskXml(string managerPath)
    {
        var command = System.Security.SecurityElement.Escape(managerPath)
            ?? throw new ArgumentException("The manager path could not be encoded.", nameof(managerPath));
        // Runs for the whole session from boot (no execution limit) and is restarted if it exits
        // unexpectedly, so a microphone rebuilt by Windows is never left advertising raw capture.
        return $$"""
            <?xml version="1.0" encoding="UTF-16"?>
            <Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
              <RegistrationInfo>
                <Author>Noise Cancelling Stack</Author>
                <Description>Keeps raw capture disabled on filtered microphones so applications receive filtered audio.</Description>
              </RegistrationInfo>
              <Triggers>
                <BootTrigger>
                  <Enabled>true</Enabled>
                </BootTrigger>
              </Triggers>
              <Principals>
                <Principal id="System">
                  <UserId>S-1-5-18</UserId>
                  <RunLevel>HighestAvailable</RunLevel>
                </Principal>
              </Principals>
              <Settings>
                <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
                <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
                <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
                <AllowHardTerminate>true</AllowHardTerminate>
                <StartWhenAvailable>true</StartWhenAvailable>
                <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
                <IdleSettings>
                  <StopOnIdleEnd>false</StopOnIdleEnd>
                  <RestartOnIdle>false</RestartOnIdle>
                </IdleSettings>
                <WakeToRun>false</WakeToRun>
                <Enabled>true</Enabled>
                <Hidden>true</Hidden>
                <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
                <Priority>7</Priority>
                <RestartOnFailure>
                  <Interval>PT1M</Interval>
                  <Count>999</Count>
                </RestartOnFailure>
              </Settings>
              <Actions Context="System">
                <Exec>
                  <Command>{{command}}</Command>
                  <Arguments>watch --quiet</Arguments>
                </Exec>
              </Actions>
            </Task>
            """;
    }

    internal static string BuildTaskXml(string managerPath)
    {
        var command = System.Security.SecurityElement.Escape(managerPath)
            ?? throw new ArgumentException("The manager path could not be encoded.", nameof(managerPath));
        // Explicit battery settings avoid Task Scheduler's default of skipping startup repair on
        // unplugged laptops. A short execution limit bounds a stuck service or registry operation.
        return $$"""
            <?xml version="1.0" encoding="UTF-16"?>
            <Task version="1.4" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
              <RegistrationInfo>
                <Author>Noise Cancelling Stack</Author>
                <Description>Maintains noise suppression for newly discovered capture endpoints.</Description>
              </RegistrationInfo>
              <Triggers>
                <BootTrigger>
                  <Enabled>true</Enabled>
                  <Delay>PT1M</Delay>
                </BootTrigger>
              </Triggers>
              <Principals>
                <Principal id="System">
                  <UserId>S-1-5-18</UserId>
                  <RunLevel>HighestAvailable</RunLevel>
                </Principal>
              </Principals>
              <Settings>
                <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
                <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
                <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
                <AllowHardTerminate>true</AllowHardTerminate>
                <StartWhenAvailable>true</StartWhenAvailable>
                <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
                <WakeToRun>false</WakeToRun>
                <Enabled>true</Enabled>
                <Hidden>true</Hidden>
                <ExecutionTimeLimit>PT15M</ExecutionTimeLimit>
                <Priority>7</Priority>
              </Settings>
              <Actions Context="System">
                <Exec>
                  <Command>{{command}}</Command>
                  <Arguments>repair --quiet --scheduled</Arguments>
                </Exec>
              </Actions>
            </Task>
            """;
    }
}
