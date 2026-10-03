; SPDX-License-Identifier: MIT
; The bootstrapper bundles exact, checksum-pinned upstream payloads prepared by scripts/build.ps1.
#ifndef AppVersion
  #define AppVersion "1.0.1"
#endif

#define AppName "Noise Cancelling Stack"
#define AppPublisher "JBWortley"
#define ManagerExe "NoiseCancellingStack.Manager.exe"

[Setup]
AppId={{7865CDB5-5F43-487D-8F4F-59FF62720A66}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription=Offline RNNoise microphone filtering installer
DefaultDirName={autopf}\NoiseCancellingStack
DisableDirPage=yes
UsePreviousAppDir=no
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.10240
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=NoiseCancellingStack-Setup-{#AppVersion}
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\{#ManagerExe}
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
ChangesEnvironment=no
ChangesAssociations=no

[Files]
Source: "..\artifacts\publish\{#ManagerExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\payload\rnnoise_mono.dll"; DestDir: "{app}\plugins"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSING.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\licenses\EqualizerAPO-LICENSE.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\artifacts\licenses\RNNoisePlugin-LICENSE.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\vendor\cache\EqualizerAPO-x64-1.4.2.exe"; Flags: dontcopy

[Icons]
Name: "{group}\Repair all microphones"; Filename: "{app}\{#ManagerExe}"; Parameters: "repair"; WorkingDir: "{app}"
Name: "{group}\Microphone status"; Filename: "{app}\{#ManagerExe}"; Parameters: "status"; WorkingDir: "{app}"
Name: "{group}\Logs and backups"; Filename: "{commonappdata}\NoiseCancellingStack"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"

[Code]
const
  RequiredApoVersion = '1.4.2';
  ApoInstallerName = 'EqualizerAPO-x64-1.4.2.exe';

function InstalledApoVersion(): String;
begin
  if not RegQueryStringValue(
    HKLM64,
    'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\EqualizerAPO',
    'DisplayVersion',
    Result) then
  begin
    Result := '';
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Version: String;
  ResultCode: Integer;
begin
  Result := '';
  Version := InstalledApoVersion();

  { Install the pinned engine only when absent. A different version is left untouched because
    silently replacing an existing system-wide audio engine is not a safe compatibility policy. }
  if Version = '' then
  begin
    WizardForm.StatusLabel.Caption := 'Installing the pinned Equalizer APO audio engine...';
    ExtractTemporaryFile(ApoInstallerName);
    if not Exec(
      ExpandConstant('{tmp}\') + ApoInstallerName,
      '/S',
      '',
      SW_HIDE,
      ewWaitUntilTerminated,
      ResultCode) then
    begin
      Result := 'Equalizer APO could not be started. No microphone changes were made.';
      exit;
    end;

    if ResultCode <> 0 then
    begin
      Result := Format('Equalizer APO setup failed with exit code %d. No microphone changes were made.', [ResultCode]);
      exit;
    end;
    Version := InstalledApoVersion();
  end;

  if Version <> RequiredApoVersion then
  begin
    Result := Format(
      'This installer requires Equalizer APO %s, but version %s is installed. ' +
      'It will not downgrade or overwrite a different audio-engine version.', [RequiredApoVersion, Version]);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    WizardForm.StatusLabel.Caption := 'Registering capture endpoints and installing the RNNoise configuration...';
    if not Exec(
      ExpandConstant('{app}\{#ManagerExe}'),
      'install --quiet',
      ExpandConstant('{app}'),
      SW_HIDE,
      ewWaitUntilTerminated,
      ResultCode) then
    begin
      MsgBox(
        'The microphone manager could not start. Use the Repair shortcut after setup and consult the local logs.',
        mbError,
        MB_OK);
    end
    else if (ResultCode <> 0) and (ResultCode <> 2) then
    begin
      MsgBox(
        Format('Microphone setup failed with exit code %d. Use the Repair shortcut and consult the local logs.', [ResultCode]),
        mbError,
        MB_OK);
    end
    else if ResultCode = 2 then
    begin
      MsgBox(
        'Noise cancellation was installed, but one or more endpoints were skipped safely. ' +
        'Open Logs and backups from the Start menu for details.',
        mbInformation,
        MB_OK);
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    if FileExists(ExpandConstant('{app}\{#ManagerExe}')) then
    begin
      if not Exec(
        ExpandConstant('{app}\{#ManagerExe}'),
        'uninstall --quiet',
        ExpandConstant('{app}'),
        SW_HIDE,
        ewWaitUntilTerminated,
        ResultCode) then
      begin
        MsgBox('Safe microphone rollback could not start. Uninstall has been cancelled.', mbError, MB_OK);
        Abort;
      end;

      if ResultCode = 1 then
      begin
        MsgBox('Safe microphone rollback failed. Uninstall has been cancelled; consult the local logs.', mbError, MB_OK);
        Abort;
      end;

      { A warning means rollback was intentionally incomplete. Keep the manager installed so the
        recovery journal and repair command remain usable instead of stranding the endpoint. }
      if ResultCode = 2 then
      begin
        MsgBox(
          'Safe rollback is incomplete because externally changed registry values were preserved. ' +
          'Uninstall has been cancelled so the manager, recovery state, and backups remain available.',
          mbError,
          MB_OK);
        Abort;
      end;
    end;
  end;
end;
