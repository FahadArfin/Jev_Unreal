"""Exercise the Windows credential dependency in the actual PowerShell 5.1 host.

Only synthetic data in pytest's temporary directory is encrypted. The setup
script's storage directory and interactive prompt are isolated before invocation;
the user's credential store is never accessed.
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(os.name != "nt", reason="Windows PowerShell and DPAPI only")

REPOSITORY = Path(__file__).resolve().parents[1]
SYNTHETIC_KEY = "jev-synthetic-regression-key-never-a-provider-credential"
SECURITY_MANIFEST = Path("Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1")


@pytest.fixture
def windows_powershell():
    executable = shutil.which("powershell.exe")
    if executable is None:
        pytest.skip("Windows PowerShell 5.1 is not installed")
    return Path(executable)


def _core_modules_directory() -> Path:
    """Find a real Core module directory without loading modules or user profiles."""
    candidates = [
        Path(value) for value in os.environ.get("PSModulePath", "").split(os.pathsep) if value
    ]
    if executable := shutil.which("pwsh.exe"):
        candidates.append(Path(executable).parent / "Modules")
    if program_files := os.environ.get("ProgramFiles"):
        candidates.append(Path(program_files) / "PowerShell/7/Modules")
    for directory in candidates:
        manifest = directory / SECURITY_MANIFEST
        if manifest.is_file():
            contents = manifest.read_text(encoding="utf-8-sig")
            if 'CompatiblePSEditions = @("Core")' in contents:
                return directory
    pytest.skip("PowerShell Core is not installed; cross-edition regression unavailable")


def _subprocess_environment(windows_powershell: Path, module_path: str) -> dict[str, str]:
    environment = os.environ.copy()
    # Inherited provider credentials are irrelevant and must never reach this subprocess.
    for name in tuple(environment):
        if name.upper().startswith(("OPENROUTER_", "TYPESAFE_", "JEV_")):
            del environment[name]
    system_modules = windows_powershell.parent / "Modules"
    if module_path == "clean":
        environment["PSModulePath"] = str(system_modules)
    elif module_path == "core_first":
        # Mirrors powershell.exe launched by bundled pwsh: its inherited search path
        # resolves the Core Security module before Windows PowerShell's own module.
        environment["PSModulePath"] = os.pathsep.join(
            [str(_core_modules_directory()), str(system_modules)]
        )
    environment["JEV_TEST_SYNTHETIC_KEY"] = SYNTHETIC_KEY
    return environment


def _run_powershell(windows_powershell, script, environment):
    return subprocess.run(
        [
            str(windows_powershell),
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
        ],
        env=environment,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )


@pytest.mark.parametrize("module_path", ["clean", "inherited", "core_first"])
@pytest.mark.parametrize("preload", [False, True], ids=["fresh", "already_imported"])
def test_security_import_and_dpapi_round_trip(windows_powershell, tmp_path, module_path, preload):
    environment = _subprocess_environment(windows_powershell, module_path)
    environment["JEV_TEST_SECURITY_HELPER"] = str(REPOSITORY / "scripts/Import-JevSecurity.ps1")
    environment["JEV_TEST_DPAPI_PATH"] = str(tmp_path / "synthetic.dpapi")
    environment["JEV_TEST_PRELOAD"] = "1" if preload else "0"
    script = tmp_path / "check-security.ps1"
    script.write_text(
        r"""
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This regression must run under Windows PowerShell 5.1.'
}
$jevTestOriginalModulePath = $env:PSModulePath
$jevTestExpectedModule = Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security'
$jevTestExpectedManifest = Join-Path $jevTestExpectedModule 'Microsoft.PowerShell.Security.psd1'
if ($env:JEV_TEST_PRELOAD -eq '1') {
    Import-Module $jevTestExpectedManifest
}
. $env:JEV_TEST_SECURITY_HELPER
. $env:JEV_TEST_SECURITY_HELPER
if ($env:PSModulePath -cne $jevTestOriginalModulePath) {
    throw 'The helper changed the caller module search path.'
}
foreach ($jevTestCommandName in @('ConvertTo-SecureString', 'ConvertFrom-SecureString')) {
    $jevTestCommand = Get-Command $jevTestCommandName
    if ($jevTestCommand.Module.Path -ine $jevTestExpectedManifest) {
        throw 'The command was not loaded from the current PowerShell installation.'
    }
}
$jevTestSecure = $null
$jevTestRestored = $null
$jevTestPointer = [IntPtr]::Zero
try {
    $jevTestSecure = ConvertTo-SecureString $env:JEV_TEST_SYNTHETIC_KEY -AsPlainText -Force
    $jevTestCipher = ConvertFrom-SecureString $jevTestSecure
    if ($jevTestCipher -eq $env:JEV_TEST_SYNTHETIC_KEY) {
        throw 'The synthetic value was not encrypted.'
    }
    [IO.File]::WriteAllText($env:JEV_TEST_DPAPI_PATH, $jevTestCipher)
    $jevTestRestored = ConvertTo-SecureString ([IO.File]::ReadAllText($env:JEV_TEST_DPAPI_PATH))
    $jevTestPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($jevTestRestored)
    if ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($jevTestPointer) -cne
        $env:JEV_TEST_SYNTHETIC_KEY) {
        throw 'DPAPI did not preserve the synthetic value.'
    }
    [Console]::Out.WriteLine('DPAPI_OK')
} finally {
    if ($jevTestPointer -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($jevTestPointer)
    }
    if ($null -ne $jevTestSecure) { $jevTestSecure.Dispose() }
    if ($null -ne $jevTestRestored) { $jevTestRestored.Dispose() }
}
""",
        encoding="utf-8",
    )
    result = _run_powershell(windows_powershell, script, environment)
    assert SYNTHETIC_KEY not in result.stdout + result.stderr
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "DPAPI_OK"
    assert not result.stderr.strip()
    ciphertext = (tmp_path / "synthetic.dpapi").read_text(encoding="utf-8-sig")
    assert SYNTHETIC_KEY not in ciphertext
    # ConvertFrom-SecureString without -Key produces the Windows DPAPI blob format.
    assert bytes.fromhex(ciphertext).startswith(bytes.fromhex("01000000d08c9ddf"))


@pytest.mark.parametrize("module_path", ["clean", "inherited", "core_first"])
def test_key_setup_encrypts_with_hidden_prompt(windows_powershell, tmp_path, module_path):
    """Run the setup flow with a temporary storage path and a synthetic prompt response."""
    environment = _subprocess_environment(windows_powershell, module_path)
    environment["JEV_TEST_LOCAL_DATA"] = str(tmp_path / "local-data")
    environment["JEV_TEST_SETUP_SCRIPT"] = str(tmp_path / "Set-OpenRouterKey.ps1")
    setup = (REPOSITORY / "scripts/Set-OpenRouterKey.ps1").read_text(encoding="utf-8-sig")
    real_storage = (
        "$jevLocalData = "
        "[Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)"
    )
    # Fail before invocation if the known redirect point changes. Do not risk a
    # test fallback that could use the user's real credential directory.
    assert setup.count(real_storage) == 1
    setup = setup.replace(real_storage, "$jevLocalData = $env:JEV_TEST_LOCAL_DATA")
    assert "GetFolderPath" not in setup
    (tmp_path / "Set-OpenRouterKey.ps1").write_text(setup, encoding="utf-8")
    shutil.copyfile(
        REPOSITORY / "scripts/Import-JevSecurity.ps1", tmp_path / "Import-JevSecurity.ps1"
    )
    script = tmp_path / "check-setup.ps1"
    script.write_text(
        r"""
$ErrorActionPreference = 'Stop'
function Read-Host {
    param([string]$Prompt, [switch]$AsSecureString)
    if (-not $AsSecureString) { throw 'Setup requested a visible credential prompt.' }
    ConvertTo-SecureString $env:JEV_TEST_SYNTHETIC_KEY -AsPlainText -Force
}
& $env:JEV_TEST_SETUP_SCRIPT
$jevTestPath = Join-Path $env:JEV_TEST_LOCAL_DATA 'JevUnreal\openrouter.dpapi'
$jevTestStored = ConvertTo-SecureString ([IO.File]::ReadAllText($jevTestPath))
$jevTestPointer = [IntPtr]::Zero
try {
    $jevTestPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($jevTestStored)
    if ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($jevTestPointer) -cne
        $env:JEV_TEST_SYNTHETIC_KEY) {
        throw 'The setup script did not preserve the synthetic credential.'
    }
} finally {
    if ($jevTestPointer -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($jevTestPointer)
    }
    $jevTestStored.Dispose()
}
""",
        encoding="utf-8",
    )
    result = _run_powershell(windows_powershell, script, environment)
    assert SYNTHETIC_KEY not in result.stdout + result.stderr
    assert result.returncode == 0, result.stderr
    assert not result.stderr.strip()
    assert "Saved the encrypted OpenRouter key" in result.stdout
    credential = tmp_path / "local-data/JevUnreal/openrouter.dpapi"
    ciphertext = credential.read_text(encoding="utf-8-sig")
    assert SYNTHETIC_KEY not in ciphertext
    assert bytes.fromhex(ciphertext).startswith(bytes.fromhex("01000000d08c9ddf"))
