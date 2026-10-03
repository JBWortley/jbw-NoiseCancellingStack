// SPDX-License-Identifier: MIT
namespace NoiseCancellingStack;

public static class MaintenanceTask
{
    public const string TaskName = @"Noise Cancelling Stack\Register new microphones";

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

        var taskXmlPath = Path.Combine(
            ProductConstants.ProgramDataDirectory,
            $"maintenance-task.{Guid.NewGuid():N}.xml");
        try
        {
            File.WriteAllText(taskXmlPath, BuildTaskXml(managerPath), System.Text.Encoding.Unicode);
            var exitCode = ProcessRunner.Run(
                "schtasks.exe",
                ["/Create", "/F", "/TN", TaskName, "/XML", taskXmlPath],
                hidden: true,
                logger);
            if (exitCode != 0)
            {
                throw new InvalidOperationException($"Task Scheduler returned exit code {exitCode}.");
            }
        }
        finally
        {
            File.Delete(taskXmlPath);
        }

        logger.Write("Registered the startup repair task for newly discovered microphones.");
    }

    public static void Delete(OperationLogger logger)
    {
        var exitCode = ProcessRunner.Run(
            "schtasks.exe",
            ["/Delete", "/F", "/TN", TaskName],
            hidden: true,
            logger);
        if (exitCode is not (0 or 1))
        {
            throw new InvalidOperationException($"Task Scheduler returned exit code {exitCode} while deleting the task.");
        }

        logger.Write("Removed the startup microphone repair task (or it was already absent).");
    }

    private static string BuildTaskXml(string managerPath)
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
                <Description>Registers newly discovered capture endpoints for RNNoise filtering.</Description>
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
                <Hidden>false</Hidden>
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
