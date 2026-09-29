"""Explicit one-shot named Win64 engine jobs outside the authenticated editor bridge."""

import asyncio
import hashlib
import os
import shutil
import time
import uuid
from pathlib import Path

from .errors import JevError
from .job_process import start_contained
from .runtime_state import RuntimeStore
from .team_policy import JobKind, NamedJob, RuntimeConfig, digest, local_path

EXECUTABLES = {
    JobKind.BUILD: "Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe",
    JobKind.COOK: "Engine/Binaries/Win64/UnrealEditor-Cmd.exe",
    JobKind.PACKAGE: "Engine/Binaries/DotNET/AutomationTool/AutomationTool.exe",
}
LEASE_RENEW_SECONDS = 10


def file_hash(path: Path, maximum: int = 1024 * 1048576) -> str:
    local_path(str(path), exists=True)
    if path.stat().st_size > maximum:
        raise JevError("job_identity", "Pinned input exceeds its hash verification limit.")
    result = hashlib.sha256()
    count = 0
    with path.open("rb") as handle:
        while chunk := handle.read(1048576):
            count += len(chunk)
            if count > maximum:
                raise JevError("job_identity", "Pinned input changed beyond its hash limit.")
            result.update(chunk)
    return result.hexdigest()


class NamedJobs:
    def __init__(self, config: RuntimeConfig, store: RuntimeStore, policy_hash: str):
        self.config, self.store, self.policy_hash = config, store, policy_hash
        self._plans: dict[str, dict] = {}
        self._tasks: dict[str, asyncio.Task] = {}
        self._records: dict[str, dict] = {}
        self._started: set[str] = set()
        self._leases: dict[str, str] = {}
        self._lock = asyncio.Lock()

    def definitions(self) -> list[dict]:
        return [job.model_dump(mode="json") for job in self.config.jobs]

    def _job(self, name: str) -> NamedJob:
        for job in self.config.jobs:
            if job.name == name:
                return job
        raise JevError(
            "job_not_allowed", "Select a named job in the explicit project configuration."
        )

    def _inputs(self, job: NamedJob) -> dict:
        project = local_path(self.config.project_file, exists=True)
        engine = local_path(self.config.engine_root or "")
        executable = local_path(str(engine / EXECUTABLES[job.kind]), exists=True)
        executable_hash = file_hash(executable)
        if executable_hash != job.executable_sha256:
            raise JevError("job_identity", "The exact engine executable no longer matches its pin.")
        inputs = {
            "project_file": str(project),
            "project_sha256": file_hash(project, 1048576),
            "executable": str(executable),
            "executable_sha256": executable_hash,
            "engine_version_sha256": file_hash(engine / "Engine/Build/Build.version", 65536),
            "policy_sha256": self.policy_hash,
        }
        if job.kind in {JobKind.BUILD, JobKind.PACKAGE}:
            if not self.config.dotnet_relative_root or not self.config.dotnet_host_sha256:
                raise JevError(
                    "job_identity", "Pin the engine's bundled .NET root and host SHA-256."
                )
            host = local_path(
                str(engine / self.config.dotnet_relative_root / "dotnet.exe"), exists=True
            )
            host_hash = file_hash(host)
            if host_hash != self.config.dotnet_host_sha256:
                raise JevError(
                    "job_identity", "The selected bundled .NET host differs from its pin."
                )
            inputs.update(
                {
                    "dotnet_host": str(host),
                    "dotnet_host_sha256": host_hash,
                    "runtimeconfig_sha256": file_hash(
                        executable.with_suffix(".runtimeconfig.json"), 65536
                    ),
                    "managed_assembly_sha256": file_hash(executable.with_suffix(".dll")),
                }
            )
        return inputs

    def preview(self, name: str) -> dict:
        job = self._job(name)
        now = time.monotonic()
        self._plans = {key: value for key, value in self._plans.items() if value["expiry"] > now}
        if len(self._plans) >= 16:
            raise JevError("too_many_plans", "At most sixteen local job plans can await review.")
        inputs = self._inputs(job)
        plan_id = uuid.uuid4().hex
        output = local_path(str(Path(self.config.output_directory or "") / plan_id))
        command = self._command(job, inputs, output)
        plan = {
            "plan_id": plan_id,
            "job_name": name,
            "kind": job.kind.value,
            "identity": inputs,
            "command": command,
            "output_directory": str(output),
            "limits": job.model_dump(mode="json"),
            "expiry": now + 120,
        }
        self._plans[plan_id] = plan
        return {key: value for key, value in plan.items() if key != "expiry"} | {
            "expires_in_seconds": 120,
            "launched": False,
            "scope": "Trusted project build code can run. Review exact project/engine and "
            "fixed command. Native editor mutation permissions do not authorize this job.",
        }

    def _command(self, job: NamedJob, inputs: dict, output: Path) -> list[str]:
        project = inputs["project_file"]
        executable = inputs["executable"]
        if job.kind is JobKind.BUILD:
            return [
                executable,
                self.config.editor_target,
                "Win64",
                "Development",
                f"-Project={project}",
                "-WaitMutex",
                "-NoHotReloadFromIDE",
            ]
        if job.kind is JobKind.COOK:
            return [
                executable,
                project,
                "-run=cook",
                "-targetplatform=Windows",
                "-unattended",
                "-nop4",
                "-stdout",
                "-FullStdOutLogOutput",
            ]
        return [
            executable,
            "BuildCookRun",
            f"-project={project}",
            "-noP4",
            "-unattended",
            "-platform=Win64",
            "-clientconfig=Development",
            "-build",
            "-cook",
            "-stage",
            "-pak",
            "-package",
            "-archive",
            f"-archivedirectory={output}",
        ]

    def _environment(self) -> dict[str, str]:
        # Do not inherit JEV tokens/provider keys or arbitrary runtime injection variables.
        permitted = {
            "SYSTEMROOT",
            "WINDIR",
            "COMSPEC",
            "PATH",
            "PATHEXT",
            "TEMP",
            "TMP",
            "USERPROFILE",
            "LOCALAPPDATA",
            "APPDATA",
            "PROGRAMDATA",
            "PROGRAMFILES",
            "PROGRAMFILES(X86)",
            "COMMONPROGRAMFILES",
            "NUMBER_OF_PROCESSORS",
        }
        environment = {key: value for key, value in os.environ.items() if key.upper() in permitted}
        if self.config.dotnet_relative_root:
            root = str(
                local_path(str(Path(self.config.engine_root) / self.config.dotnet_relative_root))
            )
            environment.update(DOTNET_ROOT=root, DOTNET_ROOT_X64=root, DOTNET_MULTILEVEL_LOOKUP="0")
        return environment

    def _roots(self, output: Path) -> list[Path]:
        project = Path(self.config.project_file).parent
        return [output, project / "Binaries", project / "Intermediate", project / "Saved"]

    def _inventory(self, output: Path, job: NamedJob) -> dict[str, tuple[int, int]]:
        inventory = {}
        for root in self._roots(output):
            local_path(str(root))
            if not root.exists():
                continue
            for directory, dirs, files in os.walk(root, followlinks=False):
                for name in dirs:
                    local_path(str(Path(directory) / name))
                for name in files:
                    path = local_path(str(Path(directory) / name), exists=True)
                    metadata = path.stat()
                    inventory[str(path)] = (metadata.st_size, metadata.st_mtime_ns)
                    if len(inventory) > job.max_artifact_files:
                        raise JevError("artifact_limit", "Observed output tree exceeds file limit.")
        return inventory

    def _budget(self, output: Path, job: NamedJob, before: dict) -> dict:
        inventory = self._inventory(output, job)
        growth = sum(
            max(0, value[0] - before.get(path, (0, 0))[0]) for path, value in inventory.items()
        )
        if growth > job.max_artifact_bytes:
            raise JevError("artifact_limit", "Observed output growth exceeds the byte limit.")
        roots = [Path(self.config.project_file).parent, output]
        if any(shutil.disk_usage(root).free < job.minimum_free_bytes for root in roots):
            raise JevError(
                "disk_budget", "A project/output volume is below its free-space reserve."
            )
        return inventory

    async def start(self, plan_id: str) -> dict:
        async with self._lock:
            if any(not task.done() for task in self._tasks.values()):
                raise JevError("job_busy", "This client already has an active named job.")
            plan = self._plans.pop(plan_id, None)
            if plan is None or plan["expiry"] <= time.monotonic():
                raise JevError("expired_plan", "Create and review a fresh one-shot named job plan.")
            job = self._job(plan["job_name"])
            if self._inputs(job) != plan["identity"]:
                raise JevError("stale_plan", "Project, engine or policy changed after job review.")
            lease_id, acquired = self.store.acquire(60, kind="job")
            if not acquired:
                raise JevError("project_leased", "Release this client's manual lease before jobs.")
            try:
                output = local_path(plan["output_directory"])
                output.mkdir(parents=True, exist_ok=False)
                before = self._inventory(output, job)
                self._budget(output, job, before)
                job_id = uuid.uuid4().hex
                record = {
                    "receipt_id": job_id,
                    "job_id": job_id,
                    "job_name": job.name,
                    "kind": job.kind.value,
                    "project_file": self.store.project,
                    "status": "starting_uncertain",
                    "identity": plan["identity"],
                    "command_sha256": digest(plan["command"]),
                    "created_at": time.time(),
                    "output_bytes": 0,
                    "output_directory": str(output),
                    "scope": "Local build job receipt; no gameplay or visual acceptance.",
                }
                self._records[job_id] = record
                self._leases[job_id] = lease_id
                self.store.save(job_id, record)
                self._tasks[job_id] = asyncio.create_task(
                    self._run(job_id, job, plan, lease_id, before)
                )
                return dict(record)
            except BaseException:
                self.store.release(lease_id)
                raise

    async def _run(self, job_id, job, plan, lease_id, before):
        self._started.add(job_id)
        record = self._records[job_id]
        output = Path(plan["output_directory"])
        process, tree = None, None
        reader, monitor, heartbeat = None, None, None
        output_hash = hashlib.sha256()

        async def drain():
            while chunk := await process.stdout.read(8192):
                record["output_bytes"] += len(chunk)
                output_hash.update(chunk)
                if record["output_bytes"] > job.max_output_bytes:
                    raise JevError("output_limit", "Job output exceeded the configured byte limit.")

        async def watch():
            while True:
                await asyncio.to_thread(self._budget, output, job, before)
                await asyncio.sleep(0.25)

        async def renew():
            while True:
                self.store.renew(lease_id)
                await asyncio.sleep(LEASE_RENEW_SECONDS)

        try:
            async with asyncio.timeout(job.timeout_seconds):
                # Recheck immediately before spawning, in addition to reviewed-plan validation.
                if self._inputs(job) != plan["identity"]:
                    raise JevError("stale_plan", "Pinned inputs changed before process creation.")
                # Preflight scans and hashing can outlast a lease; never launch after losing it.
                self.store.renew(lease_id)
                heartbeat = asyncio.create_task(renew())
                process, tree = await start_contained(
                    plan["command"], str(Path(self.config.project_file).parent), self._environment()
                )
                record["status"] = "running"
                self.store.save(job_id, record)
                reader = asyncio.create_task(drain())
                monitor = asyncio.create_task(watch())
                waiter = asyncio.create_task(process.wait())
                try:
                    done, _ = await asyncio.wait(
                        {reader, monitor, heartbeat, waiter}, return_when=asyncio.FIRST_COMPLETED
                    )
                    # EOF can precede process exit; monitor remains authoritative while waiting.
                    if reader in done:
                        await reader
                        done, _ = await asyncio.wait(
                            {monitor, heartbeat, waiter}, return_when=asyncio.FIRST_COMPLETED
                        )
                    if heartbeat in done:
                        await heartbeat
                    if monitor in done:
                        await monitor
                    record["exit_code"] = await waiter
                    tree.close()  # End any descendant remaining after the main executable exits.
                    await reader
                finally:
                    waiter.cancel()
                    await asyncio.gather(waiter, return_exceptions=True)
                inventory = await asyncio.to_thread(self._budget, output, job, before)
                record["artifacts"] = await asyncio.to_thread(
                    self._artifacts, job, output, before, inventory
                )
                if heartbeat.done():
                    await heartbeat
                record["status"] = "succeeded" if process.returncode == 0 else "failed"
                if process.returncode == 0 and not record["artifacts"]["expected_output_observed"]:
                    record["status"] = "output_unverified"
        except TimeoutError:
            record["status"] = "timed_out"
        except asyncio.CancelledError:
            record["status"] = "cancelled"
        except JevError as exc:
            record["status"], record["error_code"] = "failed", exc.code
        except (OSError, ValueError):
            record["status"], record["error_code"] = "failed", "job_process_error"
        finally:
            if tree is not None:
                tree.close()
            if process is not None:
                if process.returncode is None:
                    process.kill()
                await process.wait()
            for task in (reader, monitor, heartbeat):
                if task is not None:
                    task.cancel()
            await asyncio.gather(
                *(task for task in (reader, monitor, heartbeat) if task), return_exceptions=True
            )
            record["output_sha256"] = output_hash.hexdigest()
            record["finished_at"] = time.time()
            try:
                self.store.save(job_id, record)
            finally:
                self.store.release(lease_id)
                self._prune_records()

    def _prune_records(self):
        # Include cancellation before coroutine entry, and all associated lifecycle metadata.
        for oldest in list(self._records):
            if len(self._records) <= 32:
                break
            if "finished_at" in self._records[oldest]:
                self._records.pop(oldest)
                self._tasks.pop(oldest, None)
                self._started.discard(oldest)
                self._leases.pop(oldest, None)

    def _artifacts(self, job, output, before, after):
        changed = [
            (path, values)
            for path, values in after.items()
            if values != before.get(path) and values[0] > 0
        ]
        suffixes = {
            JobKind.BUILD: {".dll"},
            JobKind.COOK: {".uasset", ".umap"},
            JobKind.PACKAGE: {".pak", ".utoc", ".exe"},
        }[job.kind]
        candidates = [
            (path, values) for path, values in changed if Path(path).suffix.lower() in suffixes
        ]
        if job.kind is JobKind.PACKAGE:
            candidates = [
                (path, values) for path, values in candidates if Path(path).is_relative_to(output)
            ]
        samples, hash_budget = [], 64 * 1048576
        for name, (size, _) in candidates[:32]:
            path = Path(name)
            root = next(root for root in self._roots(output) if path.is_relative_to(root))
            sample = {
                "root": root.name,
                "relative_path": str(path.relative_to(root)),
                "bytes": size,
            }
            if size <= hash_budget:
                sample["sha256"] = file_hash(path, hash_budget)
                hash_budget -= size
            else:
                sample["hash_omitted"] = "64_MiB_total_hash_budget"
            samples.append(sample)
        return {
            "changed_file_count": len(changed),
            "candidate_count": len(candidates),
            "expected_output_observed": bool(candidates),
            "samples": samples,
            "samples_truncated": len(candidates) > len(samples),
            "scope": "Changed nonempty output files only; not a runnable artifact guarantee.",
        }

    def status(self, job_id: str) -> dict:
        if job_id in self._records:
            return dict(self._records[job_id])
        record = self.store.get(job_id)
        if "job_name" not in record:
            raise JevError("unknown_job", "This ID does not identify a named build job.")
        if record["status"] in {"starting_uncertain", "running"}:
            record["status"] = "interrupted_uncertain"
            record["scope"] = (
                "Prior process outcome is unconfirmed. Inspect outputs; never retry automatically."
            )
        return record

    async def cancel(self, job_id: str) -> dict:
        task = self._tasks.get(job_id)
        if task is None:
            raise JevError("job_not_owned", "Only this client's running jobs can be cancelled.")
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
        self._finish_unstarted(job_id)
        return self.status(job_id)

    def _finish_unstarted(self, job_id: str):
        if job_id in self._records and job_id not in self._started:
            self._records[job_id]["status"] = "cancelled"
            self._records[job_id]["finished_at"] = time.time()
            try:
                self.store.save(job_id, self._records[job_id])
            finally:
                self.store.release(self._leases[job_id])
                self._prune_records()

    async def close(self):
        tasks = list(self._tasks.values())
        for task in tasks:
            if not task.done():
                task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        for job_id in list(self._tasks):
            self._finish_unstarted(job_id)
