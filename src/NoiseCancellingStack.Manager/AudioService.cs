// SPDX-License-Identifier: MIT
using System.ServiceProcess;

namespace NoiseCancellingStack;

public static class AudioService
{
    // Restart only after a real mutation. Callers deliberately treat restart failure as a warning
    // because a reboot can still apply an otherwise successful, safely journaled change.
    public static void Restart(OperationLogger logger)
    {
        using var service = new ServiceController("Audiosrv");
        service.Refresh();
        if (service.Status is not ServiceControllerStatus.Stopped and not ServiceControllerStatus.StopPending)
        {
            logger.Write("Restarting Windows Audio; active calls and recordings may be interrupted briefly.");
            service.Stop();
        }

        service.WaitForStatus(ServiceControllerStatus.Stopped, TimeSpan.FromSeconds(30));
        service.Start();
        service.WaitForStatus(ServiceControllerStatus.Running, TimeSpan.FromSeconds(30));
        logger.Write("Windows Audio is running.");
    }
}
