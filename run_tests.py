#!/usr/bin/env python3

import os
import platform
import re
import subprocess
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ANSI_ESCAPE_RE = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
SUMMARY_RE = re.compile(
    r"applications:\s*(?P<applications>\d+),\s*packages:\s*(?P<packages>\d+),\s*templates:\s*(?P<templates>\d+)"
)
LIST_ENTRY_RE = re.compile(r"^(?P<kind>[APT])\s+(?P<name>.+?)\s+=>")


@dataclass(frozen=True)
class ListState:
    applications: int
    packages: int
    templates: int
    application_names: frozenset[str]
    package_names: frozenset[str]
    template_names: frozenset[str]


class BakeTests(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.repo_root = Path(__file__).resolve().parent
        cls.bake_bin = cls.repo_root / "build" / "bake"
        cls.bake_home = cls.repo_root / "test" / "tmp" / "bake_home"
        cls.env = os.environ.copy()
        cls.env["BAKE_HOME"] = str(cls.bake_home)

        cls._require_supported_os()
        cls.bake_home.parent.mkdir(parents=True, exist_ok=True)
        cls.git_snapshot_before = cls.git_snapshot()

        cls.run_cmd(["make", "clean"])
        cls.run_cmd(["make", "-j", "8"])
        cls.bake(["setup", "--local"])

    def setUp(self) -> None:
        self.bake(["clean", "test"])
        self.bake(["reset"])
        self.assert_empty_list_state()

    @classmethod
    def _require_supported_os(cls) -> None:
        if platform.system() not in {"Linux", "Darwin"}:
            raise unittest.SkipTest("run_tests.py supports Linux and macOS only")

    @classmethod
    def run_cmd(
        cls,
        args: list[str],
        cwd: Path | None = None,
    ) -> str:
        run_cwd = str(cwd if cwd is not None else cls.repo_root)
        proc = subprocess.run(
            args,
            cwd=run_cwd,
            env=cls.env,
            text=True,
            capture_output=True,
            check=False,
        )

        output = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0:
            raise AssertionError(
                "Command failed with non-zero exit status\n"
                f"cwd: {run_cwd}\n"
                f"cmd: {' '.join(args)}\n"
                f"exit code: {proc.returncode}\n"
                f"output:\n{output}"
            )
        return output

    @classmethod
    def bake(cls, args: list[str], cwd: Path | None = None) -> str:
        return cls.run_cmd([str(cls.bake_bin), *args], cwd=cwd)

    @staticmethod
    def strip_ansi(text: str) -> str:
        return ANSI_ESCAPE_RE.sub("", text)

    @classmethod
    def list_state(cls, cwd: Path | None = None) -> ListState:
        raw = cls.bake(["list"], cwd=cwd)
        text = cls.strip_ansi(raw)

        apps: set[str] = set()
        packages: set[str] = set()
        templates: set[str] = set()

        for line in text.splitlines():
            entry_match = LIST_ENTRY_RE.match(line.strip())
            if not entry_match:
                continue

            kind = entry_match.group("kind")
            name = entry_match.group("name").strip()
            if kind == "A":
                apps.add(name)
            elif kind == "P":
                packages.add(name)
            elif kind == "T":
                templates.add(name)

        summary_match = SUMMARY_RE.search(text)
        if not summary_match:
            raise AssertionError(f"Failed to parse bake list summary:\n{text}")

        return ListState(
            applications=int(summary_match.group("applications")),
            packages=int(summary_match.group("packages")),
            templates=int(summary_match.group("templates")),
            application_names=frozenset(apps),
            package_names=frozenset(packages),
            template_names=frozenset(templates),
        )

    def assert_empty_list_state(self, cwd: Path | None = None) -> None:
        state = self.list_state(cwd=cwd)
        self.assertEqual(
            (state.applications, state.packages, state.templates),
            (0, 0, 0),
            f"Expected empty bake environment, got: {state}",
        )
        self.assertEqual(state.application_names, frozenset())
        self.assertEqual(state.package_names, frozenset())
        self.assertEqual(state.template_names, frozenset())

    @classmethod
    def git_snapshot(cls) -> tuple[str, ...]:
        output = cls.run_cmd(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=cls.repo_root,
        )
        lines: Iterable[str] = (line for line in output.splitlines() if line.strip())
        return tuple(sorted(lines))

    def test_build_test_target(self) -> None:
        self.bake(["build", "test"])
        state = self.list_state()
        self.assertGreater(state.applications + state.packages + state.templates, 0)
        self.assertIn("flecs", state.package_names)
        self.assertIn("flecs.components.graphics", state.package_names)

    def test_build_projects_target(self) -> None:
        self.bake(["build", "test/projects"])
        state = self.list_state()
        self.assertIn("examples.c.app_clib", state.application_names)
        self.assertIn("examples.c.pkg_helloworld", state.package_names)

    def test_build_integration_target(self) -> None:
        self.bake(["build", "test/integration"])
        state = self.list_state()
        self.assertIn("flecs", state.package_names)
        self.assertIn("city", state.application_names)
        self.assertIn("flecs.components.graphics", state.package_names)

    def test_build_flecs_modules_target(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        state = self.list_state()
        self.assertIn("flecs", state.package_names)
        self.assertIn("flecs.components.transform", state.package_names)
        self.assertIn("city", state.application_names)

    def test_rebuild_and_incremental_from_repo_root(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        self.bake(["rebuild", "test/integration/flecs-modules-test/apps/city"])
        self.bake(["rebuild", "test/integration/flecs-modules-test/apps/tower_defense"])
        self.bake(["build", "test/integration/flecs-modules-test/apps/city"])
        self.bake(["build", "test/integration/flecs-modules-test/apps/tower_defense"])

    def test_tower_defense_incremental_does_not_rebuild_main_cpp(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        output = self.bake(["build", "test/integration/flecs-modules-test/apps/tower_defense"])
        self.assertNotIn("main.cpp", self.strip_ansi(output))

    def test_rebuild_from_test_directory(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        test_dir = self.repo_root / "test"
        self.bake(["rebuild", "integration/flecs-modules-test/apps/city"], cwd=test_dir)
        self.bake(
            ["rebuild", "integration/flecs-modules-test/apps/tower_defense"], cwd=test_dir
        )

    def test_rebuild_from_integration_directory(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        integration_dir = self.repo_root / "test" / "integration"
        self.bake(["rebuild", "flecs-modules-test/apps/city"], cwd=integration_dir)
        self.bake(
            ["rebuild", "flecs-modules-test/apps/tower_defense"], cwd=integration_dir
        )
        state = self.list_state(cwd=integration_dir)
        self.assertIn("flecs", state.package_names)
        self.assertIn("city", state.application_names)
        self.assertIn("tower_defense", state.application_names)

    def test_run_query_from_root(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test/flecs"])
        output = self.bake(
            ["run", "test/integration/flecs-modules-test/flecs/test/query", "--", "-j", "12"]
        )
        self.assertIn("PASS:", self.strip_ansi(output))

    def test_run_query_from_flecs_directory_and_empty_env(self) -> None:
        flecs_dir = self.repo_root / "test" / "integration" / "flecs-modules-test" / "flecs"
        self.bake(["run", "test/query", "--", "-j", "12"], cwd=flecs_dir)
        self.bake(["clean"], cwd=flecs_dir)
        self.bake(["reset"], cwd=flecs_dir)
        self.bake(["run", "test/query", "--", "-j", "12"], cwd=flecs_dir)
        self.bake(["run", "test/query", "--", "-j", "12"], cwd=flecs_dir)

        state = self.list_state(cwd=flecs_dir)
        self.assertIn("flecs", state.package_names)
        self.assertEqual(state.application_names, frozenset())
        self.assertEqual(state.package_names - {"flecs"}, frozenset())

    def test_worktree_unchanged_after_run(self) -> None:
        current_snapshot = self.git_snapshot()
        self.assertEqual(
            self.git_snapshot_before,
            current_snapshot,
            "Tracked files changed during run_tests.py execution",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
