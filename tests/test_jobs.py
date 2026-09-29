"""Fixed job planning plus controlled Python subprocess lifecycle tests; no Unreal launch."""

import asyncio
import json
import os
import subprocess
import sys
import threading
from contextlib import AsyncExitStack
from pathlib import Path

import pytest
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from jev_unreal.errors import JevError
from jev_unreal.jobs import EXECUTABLES, NamedJobs, file_hash
from jev_unreal.runtime_state import RuntimeStore
from jev_unreal.team_policy import JobKind, RuntimeConfig, digest

FIXTURE_SCRIPT = """
import os
import pathlib
import subprocess
import sys
import time
mode, output, project = sys.argv[1:]
output = pathlib.Path(output)
project = pathlib.Path(project)
if mode == "child":
    while True:
        (output / "heartbeat.txt").write_text(str(time.time_ns()))
        time.sleep(0.05)
elif mode == "tree":
    subprocess.Popen([sys.executable, __file__, "child", str(output), str(project)])
    time.sleep(30)
elif mode == "sleep":
    time.sleep(30)
elif mode == "flood":
    print("x" * 8192, flush=True)
    time.sleep(30)
elif mode == "disk":
    (output / "oversize.bin").write_bytes(b"x" * 4096)
    time.sleep(30)
elif mode == "failure":
    print("synthetic failure", flush=True)
    sys.exit(9)
elif mode == "success":
    assert "JEV_BRIDGE_TOKEN" not in os.environ
    assert "OPENROUTER_API_KEY" not in os.environ
    binary = project / "Binaries" / "Win64" / "Fixture.dll"
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.write_bytes(b"synthetic-test-artifact")
    print("synthetic success", flush=True)
"""


@pytest.fixture
def job_config(tmp_path):
    project = tmp_path / "Project" / "Fixture.uproject"
    project.parent.mkdir()
    project.write_text("{}")
    engine = tmp_path / "EngineRoot"
    version = engine / "Engine" / "Build" / "Build.version"
    version.parent.mkdir(parents=True)
    version.write_text('{"MajorVersion":5,"MinorVersion":8}')
    dotnet = engine / "Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe"
    dotnet.parent.mkdir(parents=True)
    dotnet.write_bytes(b"synthetic-dotnet-host")
    definitions = []
    for kind, relative in EXECUTABLES.items():
        executable = engine / relative
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_bytes(b"synthetic-not-executable")
        executable.with_suffix(".runtimeconfig.json").write_text("{}")
        executable.with_suffix(".dll").write_bytes(b"synthetic-managed-assembly")
        definitions.append(
            {
                "name": kind.value,
                "kind": kind.value,
                "executable_sha256": file_hash(executable),
                "minimum_free_bytes": 0,
                "timeout_seconds": 5,
            }
        )
    config = RuntimeConfig.model_validate_json(
        json.dumps(
            {
                "version": 1,
                "project_file": str(project),
                "state_directory": str(tmp_path / "state"),
                "engine_root": str(engine),
                "dotnet_relative_root": "Engine/Binaries/ThirdParty/DotNet/10.0/win-x64",
                "dotnet_host_sha256": file_hash(dotnet),
                "output_directory": str(tmp_path / "outputs"),
                "editor_target": "FixtureEditor",
                "jobs": definitions,
            }
        )
    )
    return config


def manager(config):
    return NamedJobs(config, RuntimeStore(config), digest(config.model_dump(mode="json")))


class FixtureJobs(NamedJobs):
    """Test-only command substitution cannot be selected through production config/MCP."""

    def __init__(self, config, mode, script):
        super().__init__(config, RuntimeStore(config), digest(config.model_dump(mode="json")))
        self.mode, self.script = mode, script

    def _command(self, job, inputs, output):
        return [
            sys.executable,
            "-u",
            str(self.script),
            self.mode,
            str(output),
            str(Path(self.config.project_file).parent),
        ]


def fixture_manager(config, mode, **limits):
    definition = config.jobs[0].model_copy(update=limits)
    config = config.model_copy(update={"jobs": [definition]})
    script = Path(config.project_file).parent.parent / "controlled_fixture.py"
    script.write_text(FIXTURE_SCRIPT)
    return FixtureJobs(config, mode, script)


async def complete(jobs, job_id):
    await asyncio.wait_for(jobs._tasks[job_id], timeout=10)
    return jobs.status(job_id)


@pytest.mark.parametrize("kind", list(JobKind))
def test_named_preview_exposes_only_pinned_fixed_command_and_does_not_launch(job_config, kind):
    jobs = manager(job_config)
    plan = jobs.preview(kind.value)
    assert plan["launched"] is False
    assert plan["command"][0] == str(Path(job_config.engine_root) / EXECUTABLES[kind])
    assert "shell" not in plan
    assert "Win64" in " ".join(plan["command"]) or "-targetplatform=Windows" in plan["command"]
    assert jobs.store.list_receipts() == []
    assert jobs.store.lease_status() == {"held": False}
    assert not Path(plan["output_directory"]).exists()


def test_unapproved_job_and_changed_executable_pin_are_rejected(job_config):
    jobs = manager(job_config)
    with pytest.raises(JevError) as error:
        jobs.preview("arbitrary-command")
    assert error.value.code == "job_not_allowed"
    (Path(job_config.engine_root) / EXECUTABLES[JobKind.BUILD]).write_bytes(b"changed")
    with pytest.raises(JevError) as error:
        jobs.preview("build")
    assert error.value.code == "job_identity"


@pytest.mark.parametrize("changed", ["project", "engine"])
async def test_reviewed_job_rejects_changed_inputs_and_consumes_plan(job_config, changed):
    jobs = manager(job_config)
    plan = jobs.preview("build")
    path = (
        Path(job_config.project_file)
        if changed == "project"
        else (Path(job_config.engine_root) / "Engine/Build/Build.version")
    )
    path.write_text('{"changed":true}')
    with pytest.raises(JevError) as error:
        await jobs.start(plan["plan_id"])
    assert error.value.code == "stale_plan"
    with pytest.raises(JevError) as error:
        await jobs.start(plan["plan_id"])
    assert error.value.code == "expired_plan"
    assert jobs.store.lease_status() == {"held": False}


async def test_conflicting_project_lease_prevents_launch(job_config):
    jobs = manager(job_config)
    other = RuntimeStore(job_config)
    lease, _ = other.acquire()
    plan = jobs.preview("build")
    with pytest.raises(JevError) as error:
        await jobs.start(plan["plan_id"])
    assert error.value.code == "project_leased"
    assert jobs.store.list_receipts() == []
    other.release(lease)


async def test_preflight_disk_budget_fails_before_process_creation(job_config):
    jobs = fixture_manager(job_config, "success", minimum_free_bytes=10**18)
    plan = jobs.preview("build")
    with pytest.raises(JevError) as error:
        await jobs.start(plan["plan_id"])
    assert error.value.code == "disk_budget"
    assert jobs.store.lease_status() == {"held": False}


async def test_lease_lost_during_preflight_refuses_process_creation(job_config, monkeypatch):
    jobs = manager(job_config)
    now = [1000.0]
    jobs.store.clock = lambda: now[0]
    other = RuntimeStore(job_config, clock=lambda: now[0])
    attempted = []

    def expired_inventory(*args):
        now[0] += 61
        other.acquire()
        return {}

    async def no_process(*args):
        attempted.append(True)
        raise JevError("fixture_no_launch", "A test fixture must not create a process.")

    monkeypatch.setattr(jobs, "_inventory", expired_inventory)
    monkeypatch.setattr(jobs, "_budget", lambda *args: {})
    monkeypatch.setattr("jev_unreal.jobs.start_contained", no_process)
    started = await jobs.start(jobs.preview("build")["plan_id"])
    result = await complete(jobs, started["job_id"])
    assert attempted == []
    assert result["status"] == "failed" and result["error_code"] == "lease_lost"
    assert other.lease_status()["owned_by_this_client"] is True
    await jobs.close()


@pytest.mark.skipif(os.name != "nt", reason="Windows process containment acceptance fixture")
async def test_slow_budget_scan_keeps_lease_and_lease_loss_terminates_job(job_config, monkeypatch):
    jobs = fixture_manager(job_config, "sleep")
    now = [1000.0]
    jobs.store.clock = lambda: now[0]
    other = RuntimeStore(job_config, clock=lambda: now[0])
    scan_started, renewed = asyncio.Event(), asyncio.Event()
    release_scan = threading.Event()
    loop = asyncio.get_running_loop()
    original_budget, original_renew = jobs._budget, jobs.store.renew
    scans = 0

    def slow_budget(*args):
        nonlocal scans
        scans += 1
        if scans > 1:
            loop.call_soon_threadsafe(scan_started.set)
            release_scan.wait(5)
        return original_budget(*args)

    def renew(*args):
        original_renew(*args)
        renewed.set()

    monkeypatch.setattr(jobs, "_budget", slow_budget)
    monkeypatch.setattr(jobs.store, "renew", renew)
    monkeypatch.setattr("jev_unreal.jobs.LEASE_RENEW_SECONDS", 0.01)
    started = await jobs.start(jobs.preview("build")["plan_id"])
    try:
        await asyncio.wait_for(scan_started.wait(), timeout=3)
        for _ in range(4):
            renewed.clear()
            now[0] += 30
            await asyncio.wait_for(renewed.wait(), timeout=1)
            with pytest.raises(JevError) as error:
                other.acquire()
            assert error.value.code == "project_leased"
        jobs.store.release(jobs._leases[started["job_id"]])
        other.acquire()
        result = await complete(jobs, started["job_id"])
        assert result["status"] == "failed" and result["error_code"] == "lease_lost"
        assert other.lease_status()["owned_by_this_client"] is True
    finally:
        release_scan.set()
        await jobs.close()


@pytest.mark.parametrize("cancel_before_entry", [True, False])
async def test_completed_job_lifecycle_metadata_is_bounded(
    job_config, monkeypatch, cancel_before_entry
):
    jobs = manager(job_config)

    async def no_process(*args):
        raise JevError("fixture_no_launch", "Synthetic completed lifecycle; no process launch.")

    monkeypatch.setattr("jev_unreal.jobs.start_contained", no_process)
    for _ in range(35):
        started = await jobs.start(jobs.preview("build")["plan_id"])
        if cancel_before_entry:
            await jobs.cancel(started["job_id"])
        else:
            await complete(jobs, started["job_id"])
    assert len(jobs._records) == len(jobs._tasks) == len(jobs._leases) == 32
    assert jobs._started <= jobs._records.keys()
    assert len(jobs._started) == (0 if cancel_before_entry else 32)
    assert jobs.store.lease_status() == {"held": False}
    await jobs.close()


@pytest.mark.skipif(os.name != "nt", reason="Windows process containment acceptance fixture")
async def test_contained_subprocess_success_has_exit_and_artifact_evidence(job_config, monkeypatch):
    monkeypatch.setenv("JEV_BRIDGE_TOKEN", "private-test-token")
    monkeypatch.setenv("OPENROUTER_API_KEY", "private-test-provider-key")
    jobs = fixture_manager(job_config, "success")
    started = await jobs.start(jobs.preview("build")["plan_id"])
    result = await complete(jobs, started["job_id"])
    assert result["status"] == "succeeded", result
    assert result["exit_code"] == 0
    assert result["artifacts"]["expected_output_observed"] is True
    assert result["artifacts"]["samples"][0]["sha256"]
    assert result["output_bytes"] > 0
    assert "synthetic success" not in json.dumps(result)
    assert jobs.store.lease_status() == {"held": False}
    reopened = manager(job_config).status(started["job_id"])
    assert reopened["status"] == "succeeded"
    await jobs.close()


@pytest.mark.skipif(os.name != "nt", reason="Windows process containment acceptance fixture")
@pytest.mark.parametrize(
    "mode,limits,status,error",
    [
        ("failure", {}, "failed", None),
        ("noartifact", {}, "output_unverified", None),
        ("sleep", {"timeout_seconds": 1}, "timed_out", None),
        ("flood", {"max_output_bytes": 1024}, "failed", "output_limit"),
        ("disk", {"max_artifact_bytes": 1024}, "failed", "artifact_limit"),
    ],
)
async def test_process_failure_timeout_and_budgets_are_not_success(
    job_config, mode, limits, status, error
):
    jobs = fixture_manager(job_config, mode, **limits)
    started = await jobs.start(jobs.preview("build")["plan_id"])
    result = await complete(jobs, started["job_id"])
    assert result["status"] == status, result
    if error:
        assert result["error_code"] == error
    assert jobs.store.lease_status() == {"held": False}
    await jobs.close()


async def test_cancel_before_task_starts_retains_receipt_and_releases_lease(job_config):
    jobs = fixture_manager(job_config, "sleep")
    started = await jobs.start(jobs.preview("build")["plan_id"])
    result = await jobs.cancel(started["job_id"])
    assert result["status"] == "cancelled"
    assert jobs.store.lease_status() == {"held": False}
    await jobs.close()


@pytest.mark.skipif(os.name != "nt", reason="Windows process tree termination acceptance fixture")
async def test_cancellation_terminates_descendants_and_retains_partial_evidence(job_config):
    jobs = fixture_manager(job_config, "tree")
    started = await jobs.start(jobs.preview("build")["plan_id"])
    heartbeat = Path(started["output_directory"]) / "heartbeat.txt"
    async with asyncio.timeout(5):
        while not heartbeat.exists():  # noqa: ASYNC110 - external child signals via this file
            await asyncio.sleep(0.05)
    result = await jobs.cancel(started["job_id"])
    assert result["status"] == "cancelled"
    after = heartbeat.read_bytes()
    await asyncio.sleep(0.3)
    assert heartbeat.read_bytes() == after
    assert jobs.store.lease_status() == {"held": False}
    await jobs.close()


def test_previous_process_running_receipt_remains_uncertain(job_config):
    jobs = manager(job_config)
    jobs.store.save("old-job", {"job_name": "build", "status": "running", "job_id": "old-job"})
    assert manager(job_config).status("old-job")["status"] == "interrupted_uncertain"


async def test_other_process_job_cancellation_is_refused(job_config):
    jobs = manager(job_config)
    with pytest.raises(JevError) as error:
        await jobs.cancel("someone-elses-job")
    assert error.value.code == "job_not_owned"


def test_dotnet_host_and_managed_payloads_are_bound_and_ambient_injection_is_omitted(
    job_config, monkeypatch
):
    monkeypatch.setenv("DOTNET_ROOT", "untrusted-ambient-root")
    monkeypatch.setenv("DOTNET_STARTUP_HOOKS", "untrusted-hook")
    jobs = manager(job_config)
    plan = jobs.preview("build")
    assert plan["identity"]["dotnet_host_sha256"] == job_config.dotnet_host_sha256
    assert plan["identity"]["runtimeconfig_sha256"]
    assert plan["identity"]["managed_assembly_sha256"]
    environment = jobs._environment()
    assert environment["DOTNET_ROOT"] == str(
        Path(job_config.engine_root) / job_config.dotnet_relative_root
    )
    assert "DOTNET_STARTUP_HOOKS" not in environment
    Path(plan["identity"]["dotnet_host"]).write_bytes(b"changed")
    with pytest.raises(JevError) as error:
        jobs.preview("build")
    assert error.value.code == "job_identity"


@pytest.mark.skipif(os.name != "nt", reason="Windows owner-crash process-tree fixture")
async def test_owner_process_crash_closes_job_object_and_stops_descendant(job_config):
    jobs = fixture_manager(job_config, "tree")
    output = Path(job_config.output_directory)
    await asyncio.to_thread(output.mkdir)
    owner_script = (
        "import asyncio,os,sys\nfrom pathlib import Path\n"
        "from jev_unreal.job_process import start_contained\n"
        "async def main():\n"
        " process,tree=await start_contained([sys.executable,'-u',sys.argv[1],"
        "'child',sys.argv[2],sys.argv[3]],sys.argv[3],dict(os.environ))\n"
        " async with asyncio.timeout(5):\n"
        "  while not (Path(sys.argv[2])/'heartbeat.txt').exists():\n"
        "   await asyncio.sleep(0.02)\n"
        " os._exit(19)\n"
        "asyncio.run(main())\n"
    )
    result = await asyncio.to_thread(
        subprocess.run,
        [
            sys.executable,
            "-c",
            owner_script,
            str(jobs.script),
            str(output),
            str(Path(job_config.project_file).parent),
        ],
        capture_output=True,
        timeout=10,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    assert result.returncode == 19, result.stderr.decode(errors="replace")
    heartbeat = output / "heartbeat.txt"
    after = heartbeat.read_bytes()
    await asyncio.sleep(0.3)
    assert heartbeat.read_bytes() == after


async def test_real_stdio_returns_structured_preview_lease_status_and_refusal(job_config):
    """Official MCP transport, synthetic pinned files; never start an engine job."""
    config_path = Path(job_config.state_directory).parent / "stdio-runtime.json"
    await asyncio.to_thread(config_path.write_text, job_config.model_dump_json())
    environment = {
        **os.environ,
        "JEV_RUNTIME_CONFIG": str(config_path),
        "JEV_EXPECTED_PROJECT": job_config.project_file,
        "JEV_PROFILES_FILE": "",
        "JEV_PROFILE": "",
        "JEV_CATALOG_FILE": "",
        "JEV_BRIDGE_TOKEN": "",
        "JEV_BRIDGE_TOKEN_FILE": "",
        "OPENROUTER_API_KEY": "",
        "TYPESAFE_API_KEY": "",
    }
    parameters = StdioServerParameters(
        command=sys.executable, args=["-m", "jev_unreal", "serve"], env=environment
    )

    async def invoke(client, name, arguments=None):
        response = await client.call_tool(name, arguments or {})
        assert not response.isError
        assert isinstance(response.structuredContent, dict)
        return response.structuredContent

    async with AsyncExitStack() as stack:
        clients = []
        for _ in range(2):
            read, write = await stack.enter_async_context(stdio_client(parameters))
            client = await stack.enter_async_context(ClientSession(read, write))
            await client.initialize()
            clients.append(client)
        first, second = clients
        status = await invoke(first, "unreal_team_status")
        assert status["ok"] and status["result"]["configured"]
        preview = await invoke(first, "unreal_named_job_preview", {"name": "build"})
        assert preview["ok"] and preview["result"]["launched"] is False
        assert preview["result"]["identity"]["dotnet_host_sha256"]
        lease = await invoke(first, "unreal_project_lease", {"seconds": 20})
        refusal = await invoke(second, "unreal_project_lease", {"seconds": 20})
        assert refusal["error"]["code"] == "project_leased"
        released = await invoke(
            first, "unreal_project_lease_release", {"lease_id": lease["result"]["lease_id"]}
        )
        assert released["result"]["released"] is True
        status = await invoke(second, "unreal_team_status")
        assert status["result"]["lease"]["held"] is False
        receipts = await invoke(first, "unreal_durable_receipts")
        assert receipts["result"] == []
