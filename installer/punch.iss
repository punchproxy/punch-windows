#ifndef SourceDir
#define SourceDir "..\dist"
#endif

#ifndef OutputDir
#define OutputDir "."
#endif

#ifndef Version
#define Version "0.0.0"
#endif

#define AppName "Punch"
#define Publisher "punchproxy"
#define AppExeName "punch-windows.exe"

[Setup]
AppId={{C8AB30BF-D312-4F1E-BB72-FBD8AA88C537}
AppName={#AppName}
AppVersion={#Version}
AppPublisher={#Publisher}
AppPublisherURL=https://github.com/punchproxy
AppSupportURL=https://github.com/punchproxy/punch-windows/issues
AppUpdatesURL=https://github.com/punchproxy/punch-windows/releases
DefaultDirName={autopf}\Punch
DefaultGroupName=Punch
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=punch-windows-installer
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupIconFile=..\punch-windows.ico
UninstallDisplayIcon={app}\{#AppExeName}
CloseApplications=yes
CloseApplicationsFilter={#AppExeName},punchd.exe,punchctl.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Start Punch tray when Windows starts"; GroupDescription: "Startup:"; Flags: checkedonce
Name: "launch"; Description: "Launch Punch tray after installation"; GroupDescription: "After install:"; Flags: checkedonce

[Files]
Source: "{#SourceDir}\punch-windows.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\punchd.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\punchctl.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Punch"; Filename: "{app}\{#AppExeName}"

[Registry]
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Punch"; ValueData: """{app}\{#AppExeName}"""; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch Punch tray"; Flags: nowait postinstall skipifsilent; Tasks: launch

[Code]
function SendMessageTimeout(hWnd: LongWord; Msg: LongWord; wParam: LongWord; lParam: String;
  fuFlags: LongWord; uTimeout: LongWord; var lpdwResult: LongWord): LongWord;
  external 'SendMessageTimeoutW@user32.dll stdcall';

function NormalizePathForCompare(Value: String): String;
begin
  Result := ExpandConstant(Trim(Value));
  while (Length(Result) > 3) and
    ((Copy(Result, Length(Result), 1) = '\') or (Copy(Result, Length(Result), 1) = '/')) do
  begin
    Delete(Result, Length(Result), 1);
  end;
  Result := Lowercase(Result);
end;

function PathContainsEntry(PathValue: String; Entry: String): Boolean;
var
  Parts: TArrayOfString;
  I: Integer;
begin
  Result := False;
  Entry := NormalizePathForCompare(Entry);
  StringChangeEx(PathValue, ';;', ';', True);
  Parts := StringSplit(PathValue, [';'], stExcludeEmpty);
  for I := 0 to GetArrayLength(Parts) - 1 do
  begin
    if NormalizePathForCompare(Parts[I]) = Entry then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function PathWithoutEntry(PathValue: String; Entry: String): String;
var
  Parts: TArrayOfString;
  I: Integer;
  Candidate: String;
begin
  Result := '';
  Entry := NormalizePathForCompare(Entry);
  Parts := StringSplit(PathValue, [';'], stExcludeEmpty);
  for I := 0 to GetArrayLength(Parts) - 1 do
  begin
    Candidate := Trim(Parts[I]);
    if (Candidate <> '') and (NormalizePathForCompare(Candidate) <> Entry) then
    begin
      if Result <> '' then
        Result := Result + ';';
      Result := Result + Candidate;
    end;
  end;
end;

procedure BroadcastEnvironmentChange();
var
  ResultCode: LongWord;
begin
  SendMessageTimeout($FFFF, $001A, 0, 'Environment', $0002, 5000, ResultCode);
end;

procedure AddInstallDirToPath();
var
  PathValue: String;
  InstallDir: String;
begin
  InstallDir := ExpandConstant('{app}');
  if not RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', PathValue) then
    PathValue := '';

  if not PathContainsEntry(PathValue, InstallDir) then
  begin
    if (PathValue <> '') and (Copy(PathValue, Length(PathValue), 1) <> ';') then
      PathValue := PathValue + ';';
    PathValue := PathValue + InstallDir;
    RegWriteExpandStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', PathValue);
    BroadcastEnvironmentChange();
  end;
end;

procedure RemoveInstallDirFromPath();
var
  PathValue: String;
  NewPathValue: String;
begin
  if RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', PathValue) then
  begin
    NewPathValue := PathWithoutEntry(PathValue, ExpandConstant('{app}'));
    if NewPathValue <> PathValue then
    begin
      RegWriteExpandStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', NewPathValue);
      BroadcastEnvironmentChange();
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    AddInstallDirToPath();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveInstallDirFromPath();
end;
