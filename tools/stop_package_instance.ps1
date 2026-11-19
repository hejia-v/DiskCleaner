param([Parameter(Mandatory)][string]$Executable)

$ErrorActionPreference = 'Stop'
$target = [IO.Path]::GetFullPath($Executable)
$name = [IO.Path]::GetFileNameWithoutExtension($target)
foreach ($instance in [Diagnostics.Process]::GetProcessesByName($name)) {
    try {
        if (![StringComparer]::OrdinalIgnoreCase.Equals($instance.Path, $target)) {
            continue
        }
        $instance.Kill()
        if (!$instance.WaitForExit(10000)) {
            throw "Timed out waiting for PID $($instance.Id) to release $target"
        }
        Write-Output "Stopped PID $($instance.Id) for in-place update of $target"
    } catch {
        if (!$instance.HasExited) { throw }
    } finally {
        $instance.Dispose()
    }
}
