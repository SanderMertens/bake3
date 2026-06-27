#!/usr/bin/env python3

import os
import platform
import re
import shutil
import stat
import subprocess
import time
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


EXE_SUFFIX = ".exe" if platform.system() == "Windows" else ""

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
        cls.bake_bin = cls.repo_root / "build" / f"bake{EXE_SUFFIX}"
        cls.bake_home = cls.repo_root / "test" / "tmp" / "bake_home"
        cls.env = os.environ.copy()
        cls.env["BAKE_HOME"] = str(cls.bake_home)

        cls._require_supported_os()
        cls.bake_home.parent.mkdir(parents=True, exist_ok=True)
        cls.git_snapshot_before = cls.git_snapshot()

        cls._remove_stale_build_state()
        cls.run_cmd(["make", "clean"])
        cls.run_cmd(["make", "-j", "8"])
        cls.bake(["setup", "--local"])

    @classmethod
    def _remove_stale_build_state(cls) -> None:
        """Delete .bake build dirs left by previous runs or manual builds.

        Local trees accumulate incremental state built by other bake binaries
        or with other flags; tests must start from the same blank slate CI
        gets from a fresh checkout. Nothing under these roots is tracked by
        git, so removal is safe.
        """
        # Limited to trees the suite builds; other integration projects (e.g.
        # flecs-engine) keep expensive local bundle caches under .bake that
        # the suite never uses.
        for root in ("projects", "tests", os.path.join("integration", "flecs-modules-test")):
            base = cls.repo_root / "test" / root
            if not base.is_dir():
                continue
            for bake_dir in base.glob("**/.bake"):
                if bake_dir.is_dir() and not bake_dir.is_symlink():
                    shutil.rmtree(bake_dir)

    def setUp(self) -> None:
        self.bake(["clean", "test"])
        self.bake(["reset"])
        self.assert_empty_list_state()

    @classmethod
    def _require_supported_os(cls) -> None:
        if platform.system() not in {"Linux", "Darwin", "Windows"}:
            raise unittest.SkipTest("run_tests.py supports Linux, macOS and Windows only")

    @classmethod
    def run_cmd(
        cls,
        args: list[str],
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
    ) -> str:
        run_cwd = str(cwd if cwd is not None else cls.repo_root)
        proc = subprocess.run(
            args,
            cwd=run_cwd,
            env=env if env is not None else cls.env,
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
    def bake(
        cls,
        args: list[str],
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
    ) -> str:
        return cls.run_cmd([str(cls.bake_bin), *args], cwd=cwd, env=env)

    @classmethod
    def bake_expect_failure(
        cls,
        args: list[str],
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
    ) -> str:
        run_cwd = str(cwd if cwd is not None else cls.repo_root)
        proc = subprocess.run(
            [str(cls.bake_bin), *args],
            cwd=run_cwd,
            env=env if env is not None else cls.env,
            text=True,
            capture_output=True,
            check=False,
        )

        output = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode == 0:
            raise AssertionError(
                "Expected command to fail but it succeeded\n"
                f"cwd: {run_cwd}\n"
                f"cmd: {str(cls.bake_bin)} {' '.join(args)}\n"
                f"output:\n{output}"
            )

        return output

    @staticmethod
    def strip_ansi(text: str) -> str:
        return ANSI_ESCAPE_RE.sub("", text)

    @staticmethod
    def strict_link_warning_flag() -> str:
        if platform.system() == "Darwin":
            return "-Wl,-fatal_warnings"
        return "-Wl,--fatal-warnings"

    def artefact_path(self, target: str) -> Path:
        project_dir = self.repo_root / target
        info = self.strip_ansi(self.bake(["info", str(project_dir)], cwd=project_dir))

        kind_match = re.search(r"^kind:\s*(.+)$", info, re.MULTILINE)
        output_match = re.search(r"^output:\s*(.+)$", info, re.MULTILINE)
        path_match = re.search(r"^path:\s*(.+)$", info, re.MULTILINE)
        if not kind_match or not output_match or not path_match:
            raise AssertionError(f"Failed to parse bake info output:\n{info}")

        kind = kind_match.group(1).strip()
        output_name = output_match.group(1).strip()
        project_path = Path(path_match.group(1).strip())

        if kind == "package":
            artefact_name = f"{output_name}.lib" if platform.system() == "Windows" else f"lib{output_name}.a"
        else:
            artefact_name = f"{output_name}.exe" if platform.system() == "Windows" else output_name

        matches = sorted(project_path.glob(f".bake/*/{artefact_name}"))
        if not matches:
            raise AssertionError(
                f"Could not find artefact '{artefact_name}' for target '{target}' under {project_path / '.bake'}"
            )

        return max(matches, key=lambda p: p.stat().st_mtime_ns)

    def object_paths(self, target: str) -> list[Path]:
        project_dir = self.repo_root / target
        obj_dirs = sorted(project_dir.glob(".bake/*/obj"))
        if not obj_dirs:
            raise AssertionError(f"Could not find object directory for target '{target}'")

        obj_dir = max(obj_dirs, key=lambda p: p.stat().st_mtime_ns)
        obj_ext = ".obj" if platform.system() == "Windows" else ".o"
        objects = sorted(p for p in obj_dir.rglob(f"*{obj_ext}") if p.is_file())
        if not objects:
            raise AssertionError(f"Could not find object files in {obj_dir}")

        return objects

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

    def test_amalgamate_list_format_supports_prefix_and_disable_flags(self) -> None:
        target = "test/projects/c/pkg_amalgamate_disable"
        distr = self.repo_root / target / "distr"
        if distr.exists():
            shutil.rmtree(distr)

        self.bake(["build", target])

        # First config: default name, no disable-flags -> full output preserved
        full_header = (distr / "examples_c_pkg_amalgamate_disable.h").read_text()
        self.assertIn("#ifdef EXAMPLES_FEATURE_REMOVED", full_header)
        self.assertIn("#if defined(EXAMPLES_FLAG_OFF)", full_header)
        self.assertIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_EXTRA", full_header)
        self.assertIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE (1)", full_header)

        # Second config: prefix "mini" + disable-flags -> separate, stripped files
        mini_header = (distr / "mini.h").read_text()
        mini_source = (distr / "mini.c").read_text()

        # Simple #ifdef / #if defined guards on disabled flags are stripped,
        # including the addon-style "#define FLAG" enable line (so the flag is
        # genuinely undefined for the compiler).
        self.assertNotIn("EXAMPLES_FEATURE_REMOVED", mini_header)
        self.assertNotIn("removed_decl", mini_header)
        self.assertNotIn("flag_only", mini_header)
        self.assertNotIn("EXAMPLES_FLAG_OFF", mini_header)
        self.assertNotIn("#define EXAMPLES_FLAG_OFF", mini_header)
        self.assertNotIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_EXTRA", mini_header)
        self.assertNotIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_NESTED", mini_header)
        self.assertNotIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE (1)", mini_header)
        self.assertNotIn("EXAMPLES_FEATURE_REMOVED", mini_source)
        self.assertNotIn("removed_decl", mini_source)

        # #else branch kept, #ifndef body kept, non-disabled flags preserved
        self.assertIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE (2)", mini_header)
        self.assertIn("examples_c_pkg_amalgamate_disable_kept_decl", mini_header)
        self.assertIn("#ifdef EXAMPLES_KEEP_THIS", mini_header)
        self.assertIn("examples_c_pkg_amalgamate_disable_other_decl", mini_header)
        self.assertIn("examples_c_pkg_amalgamate_disable_value", mini_header)
        self.assertIn("EXAMPLES_C_PKG_AMALGAMATE_DISABLE_BIAS (10)", mini_header)
        self.assertIn("#define EXAMPLES_DEBUG_INFO", mini_header)

        # The prefixed source includes its own prefixed header
        self.assertIn('#include "mini.h"', mini_source)

    def test_amalgamate_disabled_flags_produce_compilable_output(self) -> None:
        # Regression: a disabled flag's enable "#define FLAG" used to be left in
        # the output, so the compiler saw the flag as defined and activated
        # compound guards (e.g. "#if defined(X) && defined(FLAG)") whose
        # declarations had been stripped -> dangling references that fail to
        # compile. The enable #define must be stripped so the flag is undefined.
        compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
        if not compiler:
            self.skipTest("no C compiler available")

        target = "test/projects/c/pkg_amalgamate_disable"
        distr = self.repo_root / target / "distr"
        if distr.exists():
            shutil.rmtree(distr)
        self.bake(["build", target])

        for name in ("mini.c", "examples_c_pkg_amalgamate_disable.c"):
            obj = distr / (name + ".o")
            self.run_cmd([compiler, "-c", str(distr / name), "-o", str(obj)])
            self.assertTrue(obj.exists(), f"expected {name} to compile")
            obj.unlink()

    def test_amalgamate_output_is_cleaned(self) -> None:
        # The amalgamated output drops comment blocks containing an @file
        # directive and collapses runs of blank lines down to a single blank
        # line. A blank line that terminates a backslash-continued macro is
        # significant; capping at one blank line preserves it (collapsing it
        # away would splice the next line into the macro).
        target = "test/projects/c/pkg_amalgamate_disable"
        distr = self.repo_root / target / "distr"
        if distr.exists():
            shutil.rmtree(distr)
        self.bake(["build", target])

        for name in (
            "examples_c_pkg_amalgamate_disable.h",
            "examples_c_pkg_amalgamate_disable.c",
            "mini.h",
            "mini.c",
        ):
            text = (distr / name).read_text()
            self.assertNotIn("@file", text)
            self.assertNotIn("\n\n\n", text)

    def test_amalgamate_regenerates_when_output_is_stale(self) -> None:
        # Regression: amalgamation used to skip regeneration when the existing
        # output was newer than the source files (mtime heuristic). A stale or
        # incorrect output (e.g. from an older binary, or after a config change
        # that left source mtimes untouched) would never be rewritten. The
        # replacement must be content-based instead.
        target = "test/projects/c/pkg_amalgamate_disable"
        distr = self.repo_root / target / "distr"
        if distr.exists():
            shutil.rmtree(distr)
        self.bake(["build", target])

        mini = distr / "mini.h"
        stripped = mini.read_text()
        self.assertNotIn("EXAMPLES_FLAG_OFF", stripped)

        # Overwrite the stripped output with the full (unstripped) header and
        # make it newer than every source file.
        full = (distr / "examples_c_pkg_amalgamate_disable.h").read_text()
        mini.write_text(full)
        os.utime(mini, None)
        self.assertIn("EXAMPLES_FLAG_OFF", mini.read_text())

        # Rebuilding without deleting must restore the stripped output.
        self.bake(["build", target])
        regenerated = mini.read_text()
        self.assertNotIn("EXAMPLES_FLAG_OFF", regenerated)
        self.assertNotIn("EXAMPLES_FEATURE_REMOVED", regenerated)
        self.assertEqual(regenerated, stripped)

    def test_build_distinguishes_sources_with_colliding_flat_names(self) -> None:
        self.bake(["build", "test/projects/c/app_obj_collision"])
        state = self.list_state()
        self.assertIn("examples.c.app_obj_collision", state.application_names)

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style flags; bake defaults to MSVC on Windows")
    def test_strict_build_enables_extended_c_warning_flags(self) -> None:
        output = self.strip_ansi(
            self.bake(["--trace", "--strict", "build", "test/projects/c/app_helloworld"])
        )

        self.assertIn("-Wall", output)
        self.assertIn("-Wextra", output)
        self.assertIn("-Wcast-align", output)
        self.assertIn("-Wformat=2", output)
        self.assertIn("-Wmissing-prototypes", output)
        self.assertIn("-Wstrict-prototypes", output)
        self.assertIn("-Wold-style-definition", output)
        self.assertIn(self.strict_link_warning_flag(), output)

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style flags; bake defaults to MSVC on Windows")
    def test_strict_build_enables_extended_cpp_warning_flags(self) -> None:
        output = self.strip_ansi(
            self.bake(["--trace", "--strict", "build", "test/projects/cpp/app_helloworld"])
        )

        self.assertIn("-Wall", output)
        self.assertIn("-Wextra", output)
        self.assertIn("-Wcast-align", output)
        self.assertIn("-Wformat=2", output)
        self.assertIn("-Wnon-virtual-dtor", output)
        self.assertIn("-Wold-style-cast", output)
        self.assertIn("-Woverloaded-virtual", output)
        self.assertIn("-Wzero-as-null-pointer-constant", output)
        self.assertIn(self.strict_link_warning_flag(), output)

    @unittest.skipIf(platform.system() == "Windows", "checks gcc-style -l flags; bake defaults to MSVC on Windows")
    def test_strict_build_avoids_duplicate_dependency_link_inputs(self) -> None:
        self.bake(["build", "test/projects/envpkgs/libmath"])
        output = self.strip_ansi(
            self.bake(["--trace", "--strict", "build", "test/projects/ws/apps/use_env"])
        )

        self.assertIn(self.strict_link_warning_flag(), output)
        self.assertNotIn(" -lenvmath -lenvmath", output)

    def test_build_integration_target(self) -> None:
        self.bake(["build", "test/integration"])
        state = self.list_state()
        self.assertIn("flecs", state.package_names)
        self.assertIn("city", state.application_names)
        self.assertIn("flecs.components.graphics", state.package_names)

    @unittest.skipIf(platform.system() != "Darwin", "flecs-engine ships a Cocoa-only native surface")
    def test_build_flecs_engine_target(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available on PATH")
        if shutil.which("cargo") is None:
            self.skipTest("cargo not available on PATH")
        self.bake(["build", "test/integration/flecs-engine"])
        state = self.list_state()
        self.assertIn("flecs_engine", state.application_names)

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

    def test_rebuild_recursive_rebuilds_dependency_from_env_source(self) -> None:
        dep_target = "test/projects/envpkgs/libmath"
        app_dir = self.repo_root / "test" / "projects" / "ws" / "apps" / "use_env"

        self.bake(["build", dep_target])
        self.bake(["build", str(app_dir)])

        artefact = self.artefact_path(dep_target)
        initial_mtime = artefact.stat().st_mtime_ns

        self.bake(["rebuild"], cwd=app_dir)
        self.assertEqual(
            artefact.stat().st_mtime_ns,
            initial_mtime,
            "Expected non-recursive rebuild to leave the env dependency untouched",
        )

        time.sleep(0.02)
        output = self.strip_ansi(self.bake(["rebuild", "-r"], cwd=app_dir))
        self.assertIn("env.libs.math", output)
        self.assertTrue(artefact.exists(), f"Expected dependency artefact at {artefact}")
        self.assertGreater(
            artefact.stat().st_mtime_ns,
            initial_mtime,
            "Expected recursive rebuild to rebuild the dependency from its source location",
        )

    @unittest.skipIf(platform.system() == "Windows", "incremental rebuild check is flaky on Windows; see CI investigation")
    def test_tower_defense_incremental_does_not_rebuild_main_cpp(self) -> None:
        self.bake(["build", "test/integration/flecs-modules-test"])
        output = self.bake(["build", "test/integration/flecs-modules-test/apps/tower_defense"])
        self.assertNotIn("main.cpp", self.strip_ansi(output))

    def test_project_json_newer_than_artefact_triggers_rebuild(self) -> None:
        target = "test/projects/c/app_helloworld"
        project_json = self.repo_root / target / "project.json"

        self.bake(["build", target])
        artefact = self.artefact_path(target)
        objects_before = {obj: obj.stat().st_mtime_ns for obj in self.object_paths(target)}
        before_mtime = artefact.stat().st_mtime_ns

        time.sleep(0.02)
        os.utime(project_json, None)
        self.bake(["build", target])

        objects_after = {obj: obj.stat().st_mtime_ns for obj in self.object_paths(target)}
        after_mtime = artefact.stat().st_mtime_ns
        rebuilt_object = any(
            objects_after.get(obj, 0) > before
            for obj, before in objects_before.items()
        )

        self.assertTrue(
            rebuilt_object,
            "Expected at least one object file to be rebuilt after touching project.json",
        )
        self.assertGreater(
            after_mtime,
            before_mtime,
            "Expected build to update artefact after touching project.json",
        )

    def test_build_app_with_json_comments(self) -> None:
        target = "test/projects/c/app_w_comments"
        self.bake(["build", target])
        artefact = self.artefact_path(target)
        self.assertTrue(artefact.exists(), f"Expected artefact at {artefact}")
        state = self.list_state()
        self.assertIn("examples.c.app_w_comments", state.application_names)

    def test_build_app_with_non_c_source_extension_skips_inl(self) -> None:
        target = "test/projects/c/app_w_non_c_ext"
        self.bake(["build", target])
        objects = self.object_paths(target)
        names = [o.name for o in objects]
        self.assertTrue(
            any("main" in name for name in names),
            f"Expected main object to be compiled, got objects: {names}",
        )
        for name in names:
            self.assertFalse(
                "foo" in name and "inl" in name,
                f"Expected foo.inl to be skipped, but found compiled object: {name}",
            )

    def test_run_app_dependee_resolves_transitive_dependee_use(self) -> None:
        self.bake(["build", "test/projects/c/pkg_helloworld"])
        self.bake(["build", "test/projects/c/pkg_w_dependee"])
        self.bake(["build", "test/projects/c/app_dependee"])
        output = self.bake(["run", "test/projects/c/app_dependee"])
        text = self.strip_ansi(output)
        self.assertIn("pkg_w_dependee", text)
        self.assertIn("Hello world", text)

    def test_build_pkg_dependency_private_with_use_private(self) -> None:
        self.bake(["build", "test/projects/c/pkg_helloworld"])
        target = "test/projects/c/pkg_dependency_private"
        self.bake(["build", target])
        artefact = self.artefact_path(target)
        self.assertTrue(artefact.exists(), f"Expected artefact at {artefact}")

    def write_standalone_workspace(
        self, name: str, message: str = "Hello world"
    ) -> tuple[Path, str, str]:
        """Create a tmp workspace with a package and a standalone app using it.

        Returns (workspace dir, package id, app id). Project directories are
        <ws>/pkg and <ws>/app.
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"standalone_pkg_{stamp}"
        app_id = f"standalone_app_{stamp}"

        pkg_include = ws / "pkg" / "include"
        pkg_src = ws / "pkg" / "src"
        app_src = ws / "app" / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (ws / "pkg" / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package"}}\n'
        )
        guard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {guard}\n"
            f"#define {guard}\n"
            "void pkg_print(void);\n"
            "#endif\n"
        )
        (pkg_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "void pkg_print(void) {\n"
            f'    printf("{message}\\n");\n'
            "}\n"
        )

        (ws / "app" / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"], "standalone": true}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "int main(void) {\n"
            "    pkg_print();\n"
            "    return 0;\n"
            "}\n"
        )

        return ws, pkg_id, app_id

    def test_standalone_flag_generates_amalgamated_deps(self) -> None:
        self.bake(["build", "test/projects/c/pkg_helloworld"])
        target = "test/projects/c/app_standalone"
        deps_dir = self.repo_root / target / "deps"
        self.addCleanup(shutil.rmtree, deps_dir, ignore_errors=True)
        if deps_dir.exists():
            shutil.rmtree(deps_dir)

        output = self.bake(["run", target])
        self.assertIn("Hello world", self.strip_ansi(output))

        header = deps_dir / "examples_c_pkg_helloworld.h"
        source = deps_dir / "examples_c_pkg_helloworld.c"
        marker = deps_dir / ".bake_standalone_deps"
        self.assertTrue(header.exists(), f"Expected amalgamated header at {header}")
        self.assertTrue(source.exists(), f"Expected amalgamated source at {source}")
        self.assertTrue(marker.exists(), f"Expected marker at {marker}")

        header_text = header.read_text()
        self.assertIn("#define examples_c_pkg_helloworld_STATIC", header_text)
        self.assertIn("void pkg_helloworld(void);", header_text)

        marker_text = marker.read_text()
        self.assertIn("examples.c.pkg_helloworld=", marker_text)

        obj_names = [p.name for p in self.object_paths(target)]
        self.assertTrue(
            any("examples_c_pkg_helloworld" in name for name in obj_names),
            f"Expected dependency object among {obj_names}",
        )

    def test_standalone_deps_refresh_when_dependency_changes(self) -> None:
        ws, pkg_id, _ = self.write_standalone_workspace("standalone_refresh")

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("Hello world", self.strip_ansi(output))

        pkg_main = ws / "pkg" / "src" / "main.c"
        pkg_main.write_text(pkg_main.read_text().replace("Hello world", "Hello universe"))

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("Hello universe", self.strip_ansi(output))

        deps_source = ws / "app" / "deps" / f"{pkg_id}.c"
        self.assertIn("Hello universe", deps_source.read_text())

    def test_standalone_rebuild_uses_existing_deps_when_dependency_missing(self) -> None:
        ws, pkg_id, _ = self.write_standalone_workspace("standalone_no_dep")

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("Hello world", self.strip_ansi(output))

        shutil.rmtree(ws / "pkg")

        self.bake(["rebuild", "app"], cwd=ws)
        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("Hello world", self.strip_ansi(output))

        deps_dir = ws / "app" / "deps"
        self.assertTrue((deps_dir / f"{pkg_id}.h").exists())
        self.assertTrue((deps_dir / f"{pkg_id}.c").exists())

    def test_standalone_build_fails_clearly_when_dependency_missing(self) -> None:
        ws, _, _ = self.write_standalone_workspace("standalone_missing")
        shutil.rmtree(ws / "pkg")

        output = self.bake_expect_failure(["build", "app"], cwd=ws)
        self.assertIn(
            "cannot generate standalone sources", self.strip_ansi(output)
        )

    @classmethod
    def _git_init_repo(cls, path: Path) -> None:
        git_env = {
            **cls.env,
            "GIT_AUTHOR_NAME": "bake",
            "GIT_AUTHOR_EMAIL": "bake@example.com",
            "GIT_COMMITTER_NAME": "bake",
            "GIT_COMMITTER_EMAIL": "bake@example.com",
        }
        for cmd in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "init"]):
            subprocess.run(["git", *cmd], cwd=path, check=True, env=git_env)

    def write_bundle_consumer_workspace(self, name: str) -> tuple[Path, Path, Path]:
        """Create a package that re-exports a bundle header through its public
        header, plus a separate application that consumes the package.

        The bundle is a header-only local git repository. Because the app lives
        in its own directory tree, the package is resolved as an external
        (installed) dependency rather than discovered locally -- the same shape
        as an app using flecs_engine, whose public header includes
        <cglm/cglm.h> from a bundle. Returns (workspace, pkg dir, app dir).
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"bundle_pkg_{stamp}"
        app_id = f"bundle_app_{stamp}"
        bundle_id = f"bundlelib_{stamp}"

        # Header-only bundle source as a standalone local git repository. The
        # header sits under <bundle_id>/ so it is reachable as
        # <bundle_id>/<bundle_id>.h once the bundle's include path is added.
        repo = ws / "bundle_repo"
        (repo / bundle_id).mkdir(parents=True)
        bundle_guard = f"{bundle_id.upper()}_H"
        (repo / bundle_id / f"{bundle_id}.h").write_text(
            f"#ifndef {bundle_guard}\n"
            f"#define {bundle_guard}\n"
            "#define BUNDLE_VALUE 42\n"
            "#endif\n"
        )
        self._git_init_repo(repo)

        pkg_dir = ws / "pkg"
        app_dir = ws / "app"
        pkg_include = pkg_dir / "include"
        pkg_src = pkg_dir / "src"
        app_src = app_dir / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (pkg_dir / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package", '
            f'"bundle": {{"{bundle_id}": '
            f'{{"repository": "{repo.as_posix()}", "header-only": true}}}}}}\n'
        )
        pkg_guard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {pkg_guard}\n"
            f"#define {pkg_guard}\n"
            f"#include <{bundle_id}/{bundle_id}.h>\n"
            "int pkg_value(void);\n"
            "#endif\n"
        )
        (pkg_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "int pkg_value(void) {\n"
            "    return BUNDLE_VALUE;\n"
            "}\n"
        )

        (app_dir / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"]}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("value=%d\\n", pkg_value());\n'
            "    return 0;\n"
            "}\n"
        )

        return ws, pkg_dir, app_dir

    def test_external_dependency_bundle_include_propagates_to_consumer(self) -> None:
        _, pkg_dir, app_dir = self.write_bundle_consumer_workspace("bundle_consumer")

        # Build and install the package; this prepares (clones) its bundle.
        self.bake(["build"], cwd=pkg_dir)

        # The app pulls in the package's public header, which includes the
        # bundle header. The package is resolved from the environment, so its
        # bundle include path must propagate to the consumer for this to build.
        output = self.bake(["run"], cwd=app_dir)
        self.assertIn("value=42", self.strip_ansi(output))

    def write_mixed_language_standalone_workspace(
        self, name: str
    ) -> tuple[Path, str, str]:
        """Workspace with a C++ package that also contains C sources, consumed
        by a standalone app. The C source uses a C-only idiom (implicit void*
        conversion) that fails when compiled as C++, so amalgamating it into a
        single .cpp would break the build -- it must land in its own .c file.
        Mirrors a C++ package like flecs_engine whose implementation is mostly
        C. Returns (workspace, package id, app id).
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"mixed_pkg_{stamp}"
        app_id = f"mixed_app_{stamp}"

        pkg_include = ws / "pkg" / "include"
        pkg_src = ws / "pkg" / "src"
        app_src = ws / "app" / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (ws / "pkg" / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package", '
            f'"value": {{"language": "c++"}}}}\n'
        )
        guard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {guard}\n"
            f"#define {guard}\n"
            "#ifdef __cplusplus\n"
            'extern "C" {\n'
            "#endif\n"
            "int c_part_value(void);\n"
            "int cpp_part_value(void);\n"
            "#ifdef __cplusplus\n"
            "}\n"
            "#endif\n"
            "#endif\n"
        )
        # C source: the implicit void*->int* conversion is valid C but an error
        # in C++, so this only compiles when emitted to a .c file.
        (pkg_src / "c_part.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "int c_part_value(void) {\n"
            "    void *v = 0;\n"
            "    int *p = v;\n"
            "    (void)p;\n"
            "    return 21;\n"
            "}\n"
        )
        # C++ source: a template makes this valid C++ but not C.
        (pkg_src / "cpp_part.cpp").write_text(
            f"#include <{pkg_id}.h>\n"
            "template<typename T> static T add21(T a) { return a + T(21); }\n"
            "int cpp_part_value(void) {\n"
            "    return add21<int>(0);\n"
            "}\n"
        )

        (ws / "app" / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"], "standalone": true}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("sum=%d\\n", c_part_value() + cpp_part_value());\n'
            "    return 0;\n"
            "}\n"
        )

        return ws, pkg_id, app_id

    def test_standalone_amalgamation_splits_c_and_cpp_sources(self) -> None:
        ws, pkg_id, _ = self.write_mixed_language_standalone_workspace(
            "mixed_lang")

        # Without the C/C++ split the package's C source would be compiled as
        # C++ and fail; the app builds and runs only when they are separated.
        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("sum=42", self.strip_ansi(output))

        deps_dir = ws / "app" / "deps"
        self.assertTrue(
            (deps_dir / f"{pkg_id}.c").exists(),
            f"Expected C amalgamation at {deps_dir / f'{pkg_id}.c'}")
        self.assertTrue(
            (deps_dir / f"{pkg_id}.cpp").exists(),
            f"Expected C++ amalgamation at {deps_dir / f'{pkg_id}.cpp'}")

        # The C source's body must live in the .c file, not the .cpp file.
        self.assertIn("c_part_value", (deps_dir / f"{pkg_id}.c").read_text())
        self.assertIn("cpp_part_value", (deps_dir / f"{pkg_id}.cpp").read_text())

    def test_standalone_propagates_dependency_defines(self) -> None:
        """A standalone app compiles its dependency's amalgamated source into
        its own binary. Defines the dependency declares in its project.json
        (like flecs_engine's GLFW_EXPOSE_NATIVE_COCOA) must reach that source,
        even though it is built with the app's compile flags.
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"define_dep_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"define_pkg_{stamp}"
        app_id = f"define_app_{stamp}"
        flag = f"NEED_FLAG_{stamp}"

        pkg_include = ws / "pkg" / "include"
        pkg_src = ws / "pkg" / "src"
        app_src = ws / "app" / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (ws / "pkg" / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package", '
            f'"lang.c": {{"defines": ["{flag}"]}}}}\n'
        )
        guard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {guard}\n#define {guard}\n"
            "int dep_value(void);\n#endif\n"
        )
        # Only compiles when the package's own define is in effect.
        (pkg_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            f"#ifndef {flag}\n"
            f'#error "{flag} not defined"\n'
            "#endif\n"
            "int dep_value(void) {\n"
            "    return 42;\n"
            "}\n"
        )

        (ws / "app" / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"], "standalone": true}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("value=%d\\n", dep_value());\n'
            "    return 0;\n"
            "}\n"
        )

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("value=42", self.strip_ansi(output))

    def test_standalone_amalgamation_dedups_unconditional_includes(self) -> None:
        """Multiple source files in an amalgamated unit that each include the
        same external header would otherwise repeat it -- harmless for guarded
        headers but fatal for single-header libraries whose implementation macro
        leaks across the merged unit (e.g. stb_image.h via STB_IMAGE_IMPLEMENTATION).
        A top-level repeat is dropped; a repeat guarded by #if is preserved,
        since the guard may be selecting between alternatives.
        """
        stamp = int(time.time() * 1_000_000)
        ws = self.repo_root / "test" / "tmp" / f"dedup_inc_{stamp}"
        self.addCleanup(shutil.rmtree, ws, ignore_errors=True)
        pkg_id = f"dedup_pkg_{stamp}"
        app_id = f"dedup_app_{stamp}"
        guard = f"DEDUP_GUARD_{stamp}"

        pkg_include = ws / "pkg" / "include"
        pkg_src = ws / "pkg" / "src"
        app_src = ws / "app" / "src"
        pkg_include.mkdir(parents=True)
        pkg_src.mkdir(parents=True)
        app_src.mkdir(parents=True)

        (ws / "pkg" / "project.json").write_text(
            f'{{"id": "{pkg_id}", "type": "package"}}\n'
        )
        hguard = f"{pkg_id.upper()}_H"
        (pkg_include / f"{pkg_id}.h").write_text(
            f"#ifndef {hguard}\n#define {hguard}\n"
            "int a_val(void);\nint b_val(void);\n#endif\n"
        )
        (pkg_src / "a.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int a_val(void) {\n    return 1;\n}\n"
        )
        # Second top-level <stdio.h> is a duplicate (dropped); the one guarded by
        # #ifdef is kept.
        (pkg_src / "b.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            f"#ifdef {guard}\n"
            "#include <stdio.h>\n"
            "#endif\n"
            "int b_val(void) {\n    return 2;\n}\n"
        )

        (ws / "app" / "project.json").write_text(
            f'{{"id": "{app_id}", "type": "application", '
            f'"value": {{"use": ["{pkg_id}"], "standalone": true}}}}\n'
        )
        (app_src / "main.c").write_text(
            f"#include <{pkg_id}.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("sum=%d\\n", a_val() + b_val());\n'
            "    return 0;\n"
            "}\n"
        )

        output = self.bake(["run", "app"], cwd=ws)
        self.assertIn("sum=3", self.strip_ansi(output))

        amalg = (ws / "app" / "deps" / f"{pkg_id}.c").read_text()
        # One top-level include survives plus the #ifdef-guarded one: two total.
        self.assertEqual(
            amalg.count("#include <stdio.h>"), 2,
            f"expected the top-level duplicate dropped and the guarded one kept:\n{amalg}")
        self.assertIn(f"#ifdef {guard}", amalg)

    def test_header_mtime_triggers_rebuild(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_dir = self.repo_root / "test" / "tmp" / f"header_mtime_{stamp}"
        src_dir = project_dir / "src"
        include_dir = project_dir / "include"
        src_dir.mkdir(parents=True, exist_ok=True)
        include_dir.mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.header_mtime.{stamp}\",\n"
            "    \"type\": \"application\"\n"
            "}\n"
        )
        header = include_dir / "myheader.h"
        header.write_text(
            "#ifndef MYHEADER_H\n"
            "#define MYHEADER_H\n"
            "static int header_value(void) { return 1; }\n"
            "#endif\n"
        )
        (src_dir / "main.c").write_text(
            "#include \"myheader.h\"\n"
            "int main(void) {\n"
            "    return header_value() - 1;\n"
            "}\n"
        )

        self.bake(["build", str(project_dir)])
        objects_before = {
            obj: obj.stat().st_mtime_ns for obj in self.object_paths(str(project_dir.relative_to(self.repo_root)))
        }
        self.assertGreater(len(objects_before), 0, "Expected at least one object file")

        time.sleep(1.1)
        os.utime(header, None)

        self.bake(["build", str(project_dir)])
        objects_after = {
            obj: obj.stat().st_mtime_ns for obj in self.object_paths(str(project_dir.relative_to(self.repo_root)))
        }
        rebuilt = any(
            objects_after.get(obj, 0) > before
            for obj, before in objects_before.items()
        )
        self.assertTrue(
            rebuilt,
            "Expected object file to be rebuilt after touching included header",
        )

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

    def test_build_from_flecs_directory_with_no_target(self) -> None:
        flecs_dir = self.repo_root / "test" / "integration" / "flecs-modules-test" / "flecs"
        self.bake([], cwd=flecs_dir)

        state = self.list_state(cwd=flecs_dir)
        self.assertIn("flecs", state.package_names)
        self.assertEqual(state.application_names, frozenset())
        self.assertEqual(state.package_names - {"flecs"}, frozenset())

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

    def test_test_command_builds_and_runs_test_project(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.testcmd.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"test_cmd_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True)

        (project_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"test_cmd_project\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )
        (src_dir / "Math.c").write_text("void Math_add(void) { }\n")

        output = self.strip_ansi(self.bake(["test", str(project_dir)]))
        self.assertIn("PASS", output)

    def test_project_json_testsuites_generate_missing_stubs(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        math_c = src_dir / "Math.c"
        initial_math = (
            "int helper_value  (void) {\n"
            "    return 42;\n"
            "}\n"
            "\n"
            "void Math_add(void){ }\n"
        )
        math_c.write_text(initial_math)

        self.bake(["build", str(project_dir)])

        time.sleep(1.1)
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\", \"sub\"]\n"
            "        }, {\n"
            "            \"id\": \"Util\",\n"
            "            \"teardown\": true,\n"
            "            \"testcases\": [\"case_1\"],\n"
            "            \"params\": {\n"
            "                \"mode\": [\"fast\", \"slow\"]\n"
            "            }\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        self.bake(["build", str(project_dir)])

        math_text = math_c.read_text()
        self.assertTrue(
            math_text.startswith(initial_math),
            "Existing code in suite source file changed unexpectedly",
        )
        self.assertEqual(math_text.count("void Math_add(void)"), 1)
        self.assertIn("void Math_sub(void) {\n}\n", math_text)
        self.assertIn("int helper_value  (void)", math_text)

        util_c = src_dir / "Util.c"
        self.assertTrue(util_c.exists(), "Expected missing suite source file to be created")
        util_text = util_c.read_text()
        self.assertIn("void Util_teardown(void) {\n}\n", util_text)
        self.assertIn("void Util_case_1(void) {\n}\n", util_text)

        main_c = src_dir / "main.c"
        self.assertTrue(main_c.exists(), "Expected generated src/main.c file")
        main_text = main_c.read_text()
        self.assertEqual(
            main_text,
            (
                "\n"
                "/* A friendly warning from bake.test\n"
                " * ----------------------------------------------------------------------------\n"
                " * This file is generated. To add/remove testcases modify the 'project.json' of\n"
                " * the test project. ANY CHANGE TO THIS FILE IS LOST AFTER (RE)BUILDING!\n"
                " * ----------------------------------------------------------------------------\n"
                " */\n"
                "\n"
                "#include <bake_test.h>\n"
                "\n"
                "// Testsuite 'Math'\n"
                "void Math_add(void);\n"
                "void Math_sub(void);\n"
                "\n"
                "// Testsuite 'Util'\n"
                "void Util_teardown(void);\n"
                "void Util_case_1(void);\n"
                "\n"
                "bake_test_case Math_testcases[] = {\n"
                "    {\n"
                "        \"add\",\n"
                "        Math_add\n"
                "    },\n"
                "    {\n"
                "        \"sub\",\n"
                "        Math_sub\n"
                "    }\n"
                "};\n"
                "\n"
                "bake_test_case Util_testcases[] = {\n"
                "    {\n"
                "        \"case_1\",\n"
                "        Util_case_1\n"
                "    }\n"
                "};\n"
                "\n"
                "const char* Util_mode_param[] = {\"fast\", \"slow\"};\n"
                "bake_test_param Util_params[] = {\n"
                "    {\"mode\", (char**)Util_mode_param, 2}\n"
                "};\n"
                "\n"
                "static bake_test_suite suites[] = {\n"
                "    {\n"
                "        \"Math\",\n"
                "        NULL,\n"
                "        NULL,\n"
                "        2,\n"
                "        Math_testcases\n"
                "    },\n"
                "    {\n"
                "        \"Util\",\n"
                "        NULL,\n"
                "        Util_teardown,\n"
                "        1,\n"
                "        Util_testcases,\n"
                "        1,\n"
                "        Util_params\n"
                "    }\n"
                "};\n"
                "\n"
                "int main(int argc, char *argv[]) {\n"
                f"    return bake_test_run(\"{project_id}\", argc, argv, suites, 2);\n"
                "}\n"
            ),
        )

    def test_project_json_testsuites_generate_stub_when_only_commented_definition(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.comment.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_comment_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project_comment\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        math_c = src_dir / "Math.c"
        math_c.write_text(
            "// void Math_add(void) { /* commented out */ }\n"
            "/* void Math_add(void) {\n"
            "    return;\n"
            "} */\n"
        )

        self.bake(["build", str(project_dir)])

        math_text = math_c.read_text()
        self.assertIn("void Math_add(void) {\n}\n", math_text)

    def test_project_json_testsuites_main_uses_project_header_when_available(self) -> None:
        stamp = int(time.time() * 1_000_000)
        header_name = f"header_{stamp}"
        project_id = f"compat.{header_name}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_header_{stamp}"
        src_dir = project_dir / "src"
        include_dir = project_dir / "include"
        src_dir.mkdir(parents=True, exist_ok=True)
        include_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project_header\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        header_guard = f"{header_name.upper()}_H"
        (include_dir / f"{header_name}.h").write_text(
            f"#ifndef {header_guard}\n"
            f"#define {header_guard}\n\n"
            f"#include <compat-{header_name}/bake_config.h>\n\n"
            "#endif\n"
        )

        self.bake(["build", str(project_dir)])

        main_c = src_dir / "main.c"
        self.assertTrue(main_c.exists(), "Expected generated src/main.c file")
        main_text = main_c.read_text()
        self.assertIn(f"#include <{header_name}.h>", main_text)
        self.assertNotIn("#include <bake_test.h>", main_text)

    def test_project_json_testsuites_do_not_rewrite_unchanged_files(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.mtime.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_mtime_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json_content = (
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project_mtime\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\", \"sub\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )
        project_json.write_text(project_json_content)

        self.bake(["build", str(project_dir)])

        math_c = src_dir / "Math.c"
        main_c = src_dir / "main.c"
        self.assertTrue(math_c.exists())
        self.assertTrue(main_c.exists())

        math_before = math_c.stat().st_mtime_ns
        main_before = main_c.stat().st_mtime_ns

        self.bake(["build", str(project_dir)])
        self.assertEqual(math_before, math_c.stat().st_mtime_ns)
        self.assertEqual(main_before, main_c.stat().st_mtime_ns)

        time.sleep(1.1)
        project_json.write_text(project_json_content)
        self.bake(["build", str(project_dir)])

        self.assertEqual(
            math_before,
            math_c.stat().st_mtime_ns,
            "Suite source was rewritten even though generated content was unchanged",
        )
        self.assertEqual(
            main_before,
            main_c.stat().st_mtime_ns,
            "main.c was rewritten even though generated content was unchanged",
        )

    def test_project_json_testsuites_report_empty_testcases(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.harness.empty.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"harness_project_empty_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"harness_project_empty\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        self.bake(["build", str(project_dir)])

        output = self.strip_ansi(self.bake(["run", str(project_dir)]))

        self.assertIn("EMPTY Math.add (add test statements)", output)
        self.assertIn("PASS:  0, FAIL:  0, EMPTY:  1", output)
        self.assertNotIn("PASS:  1, FAIL:  0, EMPTY:  0", output)

    def test_setup_local_reinstalls_executable_bake_binary(self) -> None:
        installed_bake = self.bake_home / f"bake3{EXE_SUFFIX}"
        self.assertTrue(installed_bake.is_file(), f"Expected installed bake binary at {installed_bake}")

        if platform.system() == "Windows":
            self.bake(["setup", "--local"])
            self.assertTrue(
                installed_bake.is_file(),
                f"Expected setup to keep installed bake binary at {installed_bake}",
            )
            return

        self.assertTrue(
            installed_bake.stat().st_mode & stat.S_IXUSR,
            f"Expected installed bake binary to be executable: {installed_bake}",
        )

        installed_bake.chmod(installed_bake.stat().st_mode & ~0o111)
        self.assertFalse(
            installed_bake.stat().st_mode & stat.S_IXUSR,
            f"Test setup failed to remove execute bit from {installed_bake}",
        )

        self.bake(["setup", "--local"])

        self.assertTrue(
            installed_bake.stat().st_mode & stat.S_IXUSR,
            f"Expected setup to restore execute bit on {installed_bake}",
        )

    def test_local_env_routes_build_outputs_to_workspace_env(self) -> None:
        target = "test/projects/c/app_helloworld"
        project_id = "examples.c.app_helloworld"
        project_dir = self.repo_root / target
        local_env_home = self.repo_root / ".bake" / "local_env"
        project_local_bake = project_dir / ".bake"

        if local_env_home.exists():
            shutil.rmtree(local_env_home)
        if project_local_bake.exists():
            shutil.rmtree(project_local_bake)

        self.bake(["--local-env", "build", target])

        self.assertTrue(local_env_home.is_dir(), f"Expected local env dir at {local_env_home}")
        self.assertFalse(
            project_local_bake.exists(),
            f"Project-local build dir should not exist with --local-env: {project_local_bake}",
        )

        project_build_root = local_env_home / "build" / project_id
        self.assertTrue(
            project_build_root.is_dir(),
            f"Expected project build root in local env: {project_build_root}",
        )

        triplet_dirs = sorted(p for p in project_build_root.iterdir() if p.is_dir())
        self.assertTrue(triplet_dirs, f"Expected triplet build directories in {project_build_root}")
        build_dir = max(triplet_dirs, key=lambda p: p.stat().st_mtime_ns)

        self.assertTrue((build_dir / "obj").is_dir(), f"Expected obj dir in {build_dir}")
        self.assertTrue((build_dir / "generated").is_dir(), f"Expected generated dir in {build_dir}")
        artefacts = sorted(p for p in build_dir.iterdir() if p.is_file())
        self.assertTrue(artefacts, f"Expected build artefacts in {build_dir}")

    def test_local_env_overrides_preset_bake_home(self) -> None:
        target = "test/projects/c/app_helloworld"
        project_id = "examples.c.app_helloworld"
        local_env_home = self.repo_root / ".bake" / "local_env"
        override_home = self.repo_root / "test" / "tmp" / "bake_home_override_probe"

        if local_env_home.exists():
            shutil.rmtree(local_env_home)
        if override_home.exists():
            shutil.rmtree(override_home)

        env = self.env.copy()
        env["BAKE_HOME"] = str(override_home)
        env["BAKE_LOCAL_ENV"] = "0"

        self.bake(["--local-env", "build", target], env=env)

        self.assertTrue(
            (local_env_home / "meta" / project_id).is_dir(),
            "Expected project metadata in local env BAKE_HOME",
        )
        self.assertFalse(
            (override_home / "meta" / project_id).exists(),
            "Preset BAKE_HOME should be ignored when --local-env is set",
        )
        self.assertFalse(
            (override_home / "build" / project_id).exists(),
            "Preset BAKE_HOME should not receive local-env build output",
        )

    def test_local_env_named_variants_do_not_interfere(self) -> None:
        workspace = self.repo_root / "test" / "projects" / "ws"
        target = "apps/hello"
        project_id = "ws.apps.hello"
        stamp = int(time.time() * 1_000_000)
        env_a = f"named_a_{stamp}"
        env_b = f"named_b_{stamp}"
        local_env_root = workspace / ".bake" / "local_env"
        env_a_home = local_env_root / env_a
        env_b_home = local_env_root / env_b
        env_a_build_root = env_a_home / "build" / project_id
        env_b_build_root = env_b_home / "build" / project_id

        if env_a_home.exists():
            shutil.rmtree(env_a_home)
        if env_b_home.exists():
            shutil.rmtree(env_b_home)

        self.bake([f"--local-env={env_a}", "build", target], cwd=workspace)
        self.bake([f"--local-env={env_b}", "build", target], cwd=workspace)

        self.assertTrue(env_a_build_root.is_dir(), f"Expected named local env build root: {env_a_build_root}")
        self.assertTrue(env_b_build_root.is_dir(), f"Expected named local env build root: {env_b_build_root}")
        self.assertTrue(
            (env_a_home / "meta" / project_id).is_dir(),
            f"Expected project metadata in named local env: {env_a_home / 'meta' / project_id}",
        )
        self.assertTrue(
            (env_b_home / "meta" / project_id).is_dir(),
            f"Expected project metadata in named local env: {env_b_home / 'meta' / project_id}",
        )

        self.bake([f"--local-env={env_a}", "clean", target], cwd=workspace)

        self.assertFalse(
            env_a_build_root.exists(),
            f"Cleaning one named local env should not leave build output behind: {env_a_build_root}",
        )
        self.assertTrue(
            env_b_build_root.is_dir(),
            f"Cleaning one named local env should not remove the other: {env_b_build_root}",
        )

        output = self.strip_ansi(self.bake([f"--local-env={env_b}", "run", target], cwd=workspace))
        self.assertIn("hello 42", output)

    def test_local_env_dependency_isolation_between_workspaces(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"local_env_isolation_{stamp}"
        ws_a = root / "ws_a"
        ws_b = root / "ws_b"

        ws_a_src = ws_a / "src"
        ws_a_inc = ws_a / "include"
        ws_a_src.mkdir(parents=True, exist_ok=True)
        ws_a_inc.mkdir(parents=True, exist_ok=True)

        (ws_a / "project.json").write_text(
            "{\n"
            "    \"id\": \"dep.isolated.lib\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (ws_a_inc / "dep_isolated_lib.h").write_text(
            "#ifndef DEP_ISOLATED_LIB_H\n"
            "#define DEP_ISOLATED_LIB_H\n"
            "\n"
            "#include \"dep-isolated-lib/bake_config.h\"\n"
            "\n"
            "DEP_ISOLATED_LIB_API\n"
            "int dep_isolated_lib_value(void);\n"
            "\n"
            "#endif\n"
        )
        (ws_a_src / "main.c").write_text(
            "#include <dep_isolated_lib.h>\n"
            "\n"
            "int dep_isolated_lib_value(void) {\n"
            "    return 42;\n"
            "}\n"
        )

        ws_b_src = ws_b / "src"
        ws_b_inc = ws_b / "include"
        ws_b_src.mkdir(parents=True, exist_ok=True)
        ws_b_inc.mkdir(parents=True, exist_ok=True)

        (ws_b / "project.json").write_text(
            "{\n"
            "    \"id\": \"app.isolated.consumer\",\n"
            "    \"type\": \"application\",\n"
            "    \"value\": {\n"
            "        \"use\": [\"dep.isolated.lib\"]\n"
            "    }\n"
            "}\n"
        )
        (ws_b_inc / "app_isolated_consumer.h").write_text(
            "#ifndef APP_ISOLATED_CONSUMER_H\n"
            "#define APP_ISOLATED_CONSUMER_H\n"
            "\n"
            "#include \"app-isolated-consumer/bake_config.h\"\n"
            "\n"
            "#endif\n"
        )
        (ws_b_src / "main.c").write_text(
            "#include <app_isolated_consumer.h>\n"
            "\n"
            "int main(void) {\n"
            "    return dep_isolated_lib_value() == 42 ? 0 : 1;\n"
            "}\n"
        )

        self.bake(["--local-env", "build", "."], cwd=ws_a)
        fail_output = self.strip_ansi(
            self.bake_expect_failure(["--local-env", "build", "."], cwd=ws_b)
        )

        self.assertIn("unresolved dependency: dep.isolated.lib", fail_output)
        self.assertTrue(
            (ws_a / ".bake" / "local_env" / "meta" / "dep.isolated.lib").is_dir(),
            "Workspace A should contain dependency in its local env",
        )
        self.assertFalse(
            (ws_b / ".bake" / "local_env" / "meta" / "dep.isolated.lib").exists(),
            "Workspace B should not be able to resolve dependency from workspace A",
        )

    def test_local_env_initializes_test_harness_templates(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.localenv.{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"local_env_test_project_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"local_env_test_project\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        local_env_home = self.repo_root / ".bake" / "local_env"
        if local_env_home.exists():
            shutil.rmtree(local_env_home)

        output = self.strip_ansi(self.bake(["--local-env", "run", str(project_dir)]))
        self.assertIn("PASS:", output)

        test_templates = local_env_home / "test"
        self.assertTrue(test_templates.is_dir(), f"Expected local test template dir: {test_templates}")
        self.assertTrue((test_templates / "bake_test.h").is_file())
        self.assertTrue((test_templates / "bake_test.c").is_file())
        self.assertTrue((test_templates / "bake_test_runtime.h").is_file())
        self.assertTrue((test_templates / "bake_test_runtime.c").is_file())

    def test_local_env_initializes_templates_from_global_bake_home(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = f"tmp.tests.localenv.global.{stamp}"
        workspace = self.repo_root / "test" / "tmp" / f"local_env_global_home_{stamp}"
        project_dir = workspace / "project"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"test\",\n"
            "    \"value\": {\n"
            "        \"output\": \"local_env_global_home_test\"\n"
            "    },\n"
            "    \"test\": {\n"
            "        \"testsuites\": [{\n"
            "            \"id\": \"Math\",\n"
            "            \"testcases\": [\"add\"]\n"
            "        }]\n"
            "    }\n"
            "}\n"
        )

        seed_home = workspace / "seed_bake_home"
        seed_test = seed_home / "test"
        source_templates = self.repo_root / "templates" / "test_harness"
        shutil.copytree(source_templates, seed_test)

        env = self.env.copy()
        env["BAKE_HOME"] = str(seed_home)
        env["BAKE_LOCAL_ENV"] = "0"
        env.pop("BAKE_GLOBAL_HOME", None)

        output = self.strip_ansi(self.bake(["--local-env", "run", "."], cwd=project_dir, env=env))
        self.assertIn("PASS:", output)

        local_templates = project_dir / ".bake" / "local_env" / "test"
        self.assertTrue(local_templates.is_dir(), f"Expected local template dir: {local_templates}")
        self.assertTrue((local_templates / "bake_test.h").is_file())
        self.assertTrue((local_templates / "bake_test.c").is_file())
        self.assertTrue((local_templates / "bake_test_runtime.h").is_file())
        self.assertTrue((local_templates / "bake_test_runtime.c").is_file())

    def test_build_nonexistent_target_fails(self) -> None:
        output = self.strip_ansi(
            self.bake_expect_failure(["build", "test/projects/c/does_not_exist"])
        )
        self.assertIn("not found", output.lower())

    def test_build_invalid_project_json_fails(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_dir = self.repo_root / "test" / "tmp" / f"invalid_json_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text("{invalid json")

        (src_dir / "main.c").write_text(
            "int main(void) {\n"
            "    return 0;\n"
            "}\n"
        )

        output = self.strip_ansi(
            self.bake_expect_failure(["build", str(project_dir)])
        )
        lower = output.lower()
        has_parse_message = (
            "failed to parse" in lower
            or "parse error" in lower
            or "json" in lower
            or "syntax" in lower
        )
        self.assertTrue(
            has_parse_message,
            f"Expected JSON parse error message, got:\n{output}",
        )
        self.assertIn(
            "project.json",
            output,
            f"Expected project.json reference in error, got:\n{output}",
        )

    def test_build_missing_dependency_fails(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_dir = self.repo_root / "test" / "tmp" / f"missing_dep_{stamp}"
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True, exist_ok=True)

        project_json = project_dir / "project.json"
        project_json.write_text(
            "{\n"
            f"    \"id\": \"tmp.missing_dep_test.{stamp}\",\n"
            "    \"type\": \"application\",\n"
            "    \"value\": {\n"
            "        \"use\": [\"nonexistent.package.xyz\"]\n"
            "    }\n"
            "}\n"
        )

        (src_dir / "main.c").write_text(
            "int main(void) {\n"
            "    return 0;\n"
            "}\n"
        )

        output = self.strip_ansi(
            self.bake_expect_failure(["build", str(project_dir)])
        )
        has_useful_message = (
            "unresolved dependency" in output.lower()
            or "nonexistent.package.xyz" in output
        )
        self.assertTrue(
            has_useful_message,
            f"Expected error about unresolved dependency or missing package name, got:\n{output}",
        )

    def test_run_nonexistent_target_fails(self) -> None:
        output = self.strip_ansi(
            self.bake_expect_failure(["run", "test/projects/c/does_not_exist"])
        )
        lower = output.lower()
        self.assertIn(
            "not found",
            lower,
            f"Expected 'not found' in error output, got:\n{output}",
        )
        self.assertIn(
            "does_not_exist",
            output,
            f"Expected missing target name in error output, got:\n{output}",
        )

    def test_circular_dependency_does_not_crash(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"circular_dep_{stamp}"
        proj_a = root / "a"
        proj_b = root / "b"
        for d in (proj_a / "src", proj_b / "src"):
            d.mkdir(parents=True, exist_ok=True)

        (proj_a / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.cycle.a.{stamp}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": {\n"
            f"        \"use\": [\"tmp.cycle.b.{stamp}\"]\n"
            "    }\n"
            "}\n"
        )
        (proj_a / "src" / "a.c").write_text("int a_value(void){return 1;}\n")

        (proj_b / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.cycle.b.{stamp}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": {\n"
            f"        \"use\": [\"tmp.cycle.a.{stamp}\"]\n"
            "    }\n"
            "}\n"
        )
        (proj_b / "src" / "b.c").write_text("int b_value(void){return 2;}\n")

        proc = subprocess.run(
            [str(self.bake_bin), "build", str(root)],
            cwd=str(self.repo_root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(
            proc.returncode,
            -11,
            "bake crashed (segfault) on circular dependency",
        )
        self.assertGreaterEqual(
            proc.returncode,
            0,
            f"bake terminated by signal on circular dependency: rc={proc.returncode}",
        )

    def test_duplicate_project_id_reports_error(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"dup_id_{stamp}"
        proj_a = root / "a"
        proj_b = root / "b"
        for d in (proj_a / "src", proj_b / "src"):
            d.mkdir(parents=True, exist_ok=True)

        proj_id = f"tmp.dup.id.{stamp}"
        for proj in (proj_a, proj_b):
            (proj / "project.json").write_text(
                "{\n"
                f"    \"id\": \"{proj_id}\",\n"
                "    \"type\": \"package\"\n"
                "}\n"
            )
            (proj / "src" / "src.c").write_text("int v(void){return 0;}\n")

        proc = subprocess.run(
            [str(self.bake_bin), "build", str(root)],
            cwd=str(self.repo_root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        self.assertGreaterEqual(
            proc.returncode,
            0,
            f"bake terminated by signal on duplicate id: rc={proc.returncode}\n{output}",
        )
        self.assertIn(
            "duplicate",
            output.lower(),
            f"expected duplicate-id error, got:\n{output}",
        )

    def test_unknown_conditional_kind_warns(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"unknown_cond_{stamp}"
        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.unknown.cond.{stamp}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": {\n"
            "        \"${oss linux}\": {\n"
            "            \"cflags\": [\"-DSHOULD_NOT_APPLY\"]\n"
            "        }\n"
            "    }\n"
            "}\n"
        )
        (root / "src" / "x.c").write_text("int x_value(void){return 0;}\n")
        proc = subprocess.run(
            [str(self.bake_bin), "build", str(root)],
            cwd=str(self.repo_root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        self.assertIn(
            "unknown conditional key",
            output,
            f"expected unknown-conditional warning in output:\n{output}",
        )

    @staticmethod
    def host_arch() -> str:
        machine = platform.machine().lower()
        if machine in {"arm64", "aarch64"}:
            return "arm64"
        if machine in {"x86_64", "amd64"}:
            return "x64"
        if machine in {"i386", "i686", "x86"}:
            return "x86"
        if machine.startswith("arm"):
            return "arm"
        return "unknown"

    def write_simple_app_project(
        self, name: str, main_c: str, lang_c: str | None = None
    ) -> tuple[Path, str]:
        stamp = int(time.time() * 1_000_000)
        app_id = f"{name}_{stamp}"
        project_dir = self.repo_root / "test" / "tmp" / f"{name}_{stamp}"
        self.addCleanup(shutil.rmtree, project_dir, ignore_errors=True)
        src_dir = project_dir / "src"
        src_dir.mkdir(parents=True)

        lang_c_section = f',\n    "lang.c": {lang_c}' if lang_c else ""
        (project_dir / "project.json").write_text(
            "{\n"
            f'    "id": "{app_id}",\n'
            f'    "type": "application"{lang_c_section}\n'
            "}\n"
        )
        (src_dir / "main.c").write_text(main_c)
        return project_dir, app_id

    def test_cfg_release_builds_separate_triplet(self) -> None:
        project_dir, app_id = self.write_simple_app_project(
            "cfg_modes",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("cfg_modes ok\\n");\n'
            "    return 0;\n"
            "}\n",
        )

        self.bake(["--cfg", "release", "build", str(project_dir)])

        triplet_base = f"{self.host_arch()}-{platform.system()}"
        release_dir = project_dir / ".bake" / f"{triplet_base}-release"
        debug_dir = project_dir / ".bake" / f"{triplet_base}-debug"
        release_artefact = release_dir / f"{app_id}{EXE_SUFFIX}"

        self.assertTrue(
            release_artefact.is_file(),
            f"Expected release artefact at {release_artefact}",
        )
        self.assertFalse(
            debug_dir.exists(),
            f"Release build should not create debug triplet dir: {debug_dir}",
        )

        output = self.run_cmd([str(release_artefact)])
        self.assertIn("cfg_modes ok", output)

        release_mtime = release_artefact.stat().st_mtime_ns

        self.bake(["build", str(project_dir)])

        debug_artefact = debug_dir / f"{app_id}{EXE_SUFFIX}"
        self.assertTrue(
            debug_artefact.is_file(),
            f"Expected debug artefact at {debug_artefact}",
        )
        self.assertTrue(
            release_artefact.is_file(),
            f"Debug build clobbered release artefact at {release_artefact}",
        )
        self.assertEqual(
            release_artefact.stat().st_mtime_ns,
            release_mtime,
            "Debug build modified the release artefact",
        )

        self.assertTrue((release_dir / "obj").is_dir(), f"Expected obj dir in {release_dir}")
        self.assertTrue((debug_dir / "obj").is_dir(), f"Expected obj dir in {debug_dir}")

        release_fingerprint = release_dir / ".bake_cmd"
        debug_fingerprint = debug_dir / ".bake_cmd"
        self.assertTrue(release_fingerprint.is_file(), f"Expected fingerprint at {release_fingerprint}")
        self.assertTrue(debug_fingerprint.is_file(), f"Expected fingerprint at {debug_fingerprint}")
        self.assertNotEqual(
            release_fingerprint.read_text(),
            debug_fingerprint.read_text(),
            "Expected release and debug builds to have distinct command fingerprints",
        )

    def test_conditional_os_and_cfg_blocks_apply(self) -> None:
        host_os = platform.system()
        other_os = "Linux" if host_os == "Darwin" else "Darwin"
        lang_c = (
            "{\n"
            f'        "${{os {host_os}}}": {{ "defines": ["HOST_OS_MATCHED"] }},\n'
            f'        "${{os {other_os}}}": {{ "defines": ["OTHER_OS_MATCHED"] }},\n'
            '        "${cfg debug}": { "defines": ["CFG_DEBUG_MATCHED"] },\n'
            '        "${cfg release}": { "defines": ["CFG_RELEASE_MATCHED"] }\n'
            "    }"
        )
        project_dir, _ = self.write_simple_app_project(
            "cond_match",
            "#ifndef HOST_OS_MATCHED\n"
            '#error "matching ${os} conditional was not applied"\n'
            "#endif\n"
            "#ifdef OTHER_OS_MATCHED\n"
            '#error "non-matching ${os} conditional was applied"\n'
            "#endif\n"
            "#ifndef CFG_DEBUG_MATCHED\n"
            '#error "matching ${cfg} conditional was not applied"\n'
            "#endif\n"
            "#ifdef CFG_RELEASE_MATCHED\n"
            '#error "non-matching ${cfg} conditional was applied"\n'
            "#endif\n"
            "int main(void) {\n"
            "    return 0;\n"
            "}\n",
            lang_c=lang_c,
        )

        self.bake(["build", str(project_dir)])

    @unittest.skipIf(platform.system() == "Windows", "run-prefix test uses a POSIX shell script")
    def test_run_prefix_wraps_executed_command(self) -> None:
        project_dir, app_id = self.write_simple_app_project(
            "run_prefix",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("run_prefix app output\\n");\n'
            "    return 0;\n"
            "}\n",
        )

        marker = f"run_prefix_marker_{app_id}"
        script = project_dir / "prefix.sh"
        script.write_text(
            "#!/bin/sh\n"
            f"echo {marker}\n"
            'exec "$@"\n'
        )
        script.chmod(0o755)

        output = self.strip_ansi(
            self.bake(["run", str(project_dir), "--run-prefix", str(script)])
        )
        self.assertIn(marker, output)
        self.assertIn("run_prefix app output", output)

    @staticmethod
    def emsdk_available() -> bool:
        if shutil.which("emcc"):
            return True
        candidates = [os.environ.get("EMSDK"), os.environ.get("EMSDK_DIR")]
        candidates.append(str(Path.home() / "GitHub" / "emsdk"))
        for candidate in candidates:
            if not candidate:
                continue
            root = Path(candidate)
            if (root / "emsdk_env.sh").is_file() and (
                root / "upstream" / "emscripten" / "emcc"
            ).is_file():
                return True
        return False

    @unittest.skipIf(platform.system() == "Windows", "emscripten target is not supported on Windows")
    def test_target_em_builds_wasm_artefacts(self) -> None:
        if not self.emsdk_available():
            self.skipTest("emscripten SDK not available")

        project_dir, app_id = self.write_simple_app_project(
            "em_target",
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("hello wasm\\n");\n'
            "    return 0;\n"
            "}\n",
        )

        self.bake(["--target", "em", "build", str(project_dir)])

        triplet_dir = project_dir / ".bake" / "wasm32-Emscripten-debug"
        self.assertTrue(
            triplet_dir.is_dir(),
            f"Expected emscripten triplet dir at {triplet_dir}",
        )

        js = triplet_dir / f"{app_id}.js"
        wasm = triplet_dir / f"{app_id}.wasm"
        self.assertTrue(js.is_file(), f"Expected .js artefact at {js}")
        self.assertTrue(wasm.is_file(), f"Expected sibling .wasm at {wasm}")

    def test_json_strlist_skips_nulls(self) -> None:
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"json_null_{stamp}"
        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.json.null.{stamp}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": {\n"
            "        \"use\": [null]\n"
            "    }\n"
            "}\n"
        )
        (root / "src" / "x.c").write_text("int x_value(void){return 0;}\n")
        proc = subprocess.run(
            [str(self.bake_bin), "info", str(root)],
            cwd=str(root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        text = self.strip_ansi(output)
        for line in text.splitlines():
            if line.startswith("use:") or line.lstrip().startswith("- "):
                self.assertNotIn(
                    "null",
                    line,
                    f"null entry leaked into use list:\n{text}",
                )

    def test_clean_nonexistent_target_succeeds(self) -> None:
        proc = subprocess.run(
            [str(self.bake_bin), "clean", "test/projects/c/does_not_exist"],
            cwd=str(self.repo_root),
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(
            proc.returncode,
            0,
            f"clean of nonexistent target should succeed silently, "
            f"rc={proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}",
        )
        stderr_clean = self.strip_ansi(proc.stderr or "").strip()
        self.assertEqual(
            stderr_clean,
            "",
            f"clean of nonexistent target should not produce stderr, got:\n{proc.stderr}",
        )

    def test_build_bundle_via_cmake(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available on PATH")
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        project_dir = self.repo_root / "test" / "tests" / "app_bundle"
        bundle_repo = project_dir / "bundle_repo"
        bake_dir = project_dir / ".bake"

        def _force_writable(path: Path) -> None:
            try:
                path.chmod(stat.S_IWUSR | stat.S_IRUSR | stat.S_IXUSR)
            except OSError:
                pass

        def _rmtree(path: Path) -> None:
            def onerror(func, target, _exc):
                _force_writable(Path(target))
                func(target)
            if path.exists():
                shutil.rmtree(path, onerror=onerror)

        _rmtree(bundle_repo)
        _rmtree(bake_dir)

        try:
            (bundle_repo / "src").mkdir(parents=True)
            (bundle_repo / "include").mkdir()
            (bundle_repo / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.10)\n"
                "project(bundle_test_lib C)\n"
                "add_library(bundle_test_lib STATIC src/bundle_test_lib.c)\n"
                "target_include_directories(bundle_test_lib PUBLIC\n"
                "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n"
                "    $<INSTALL_INTERFACE:include>)\n"
                "include(GNUInstallDirs)\n"
                "install(TARGETS bundle_test_lib\n"
                "    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}\n"
                "    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})\n"
                "install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\n"
            )
            (bundle_repo / "include" / "bundle_test_lib.h").write_text(
                "#ifndef BUNDLE_TEST_LIB_H\n"
                "#define BUNDLE_TEST_LIB_H\n"
                "int bundle_test_lib_answer(void);\n"
                "#endif\n"
            )
            (bundle_repo / "src" / "bundle_test_lib.c").write_text(
                "#include \"bundle_test_lib.h\"\n"
                "int bundle_test_lib_answer(void) { return 42; }\n"
            )

            git_args = [
                "git",
                "-c", "commit.gpgsign=false",
                "-c", "user.email=bundle@test",
                "-c", "user.name=bundle test",
            ]
            self.run_cmd(["git", "init", "-q"], cwd=bundle_repo)
            self.run_cmd([*git_args, "add", "-A"], cwd=bundle_repo)
            self.run_cmd([*git_args, "commit", "-q", "-m", "init"], cwd=bundle_repo)

            output = self.bake(["--local-env", "run", "."], cwd=project_dir)
            text = self.strip_ansi(output)
            self.assertIn("bundle_answer=42", text)
            self.assertIn("bundle_test_lib", text)

            install_root = (
                bake_dir / "bundles" / "bundle_test_lib" / "default" / "install"
            )
            self.assertTrue(
                install_root.exists() and any(install_root.iterdir()),
                f"Expected bundle install tree under {install_root}",
            )

            second_output = self.bake(["--local-env", "run", "."], cwd=project_dir)
            second_text = self.strip_ansi(second_output)
            self.assertIn("bundle_answer=42", second_text)
            self.assertNotIn("Cloning into", second_text)
            self.assertNotIn("Build files have been written to", second_text)
        finally:
            _rmtree(bundle_repo)
            _rmtree(bake_dir)

    @staticmethod
    def _rm_tree(path: Path) -> None:
        def _force_writable(target: Path) -> None:
            try:
                target.chmod(stat.S_IWUSR | stat.S_IRUSR | stat.S_IXUSR)
            except OSError:
                pass

        def onerror(func, target, _exc):
            _force_writable(Path(target))
            func(target)

        if path.exists():
            shutil.rmtree(path, onerror=onerror)

    @classmethod
    def _git_init_repo(cls, path: Path) -> None:
        git_args = [
            "git",
            "-c", "commit.gpgsign=false",
            "-c", "user.email=bundle@test",
            "-c", "user.name=bundle test",
        ]
        cls.run_cmd(["git", "init", "-q"], cwd=path)
        cls.run_cmd([*git_args, "add", "-A"], cwd=path)
        cls.run_cmd([*git_args, "commit", "-q", "-m", "init"], cwd=path)

    def test_cpp_project_uses_per_language_config_for_each_source(self) -> None:
        # A package/app whose language is c++ must still compile .c sources with
        # the lang.c configuration (and .cc sources with lang.cpp), rather than
        # forcing every unit through lang.cpp.
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"mixed_lang_{stamp}"
        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"tmp.mixed.lang.{stamp}\",\n"
            "    \"type\": \"application\",\n"
            "    \"value\": { \"language\": \"c++\" },\n"
            "    \"lang.c\": { \"defines\": [\"FROM_LANG_C\"] },\n"
            "    \"lang.cpp\": { \"defines\": [\"FROM_LANG_CPP\"], \"cpp-standard\": \"c++11\" }\n"
            "}\n"
        )
        (root / "src" / "main.c").write_text(
            "#ifndef FROM_LANG_C\n"
            "#error \"lang.c define not applied to .c source in a c++ project\"\n"
            "#endif\n"
            "#ifdef FROM_LANG_CPP\n"
            "#error \"lang.cpp define leaked into .c source\"\n"
            "#endif\n"
            "int cpp_helper(void);\n"
            "int main(void) { return cpp_helper(); }\n"
        )
        (root / "src" / "helper.cc").write_text(
            "#ifndef FROM_LANG_CPP\n"
            "#error \"lang.cpp define not applied to .cc source\"\n"
            "#endif\n"
            "extern \"C\" int cpp_helper(void) { return 0; }\n"
        )
        try:
            self.bake(["--local-env", "build", "."], cwd=root)
        finally:
            self._rm_tree(root)

    def test_c_app_links_cpp_package_dependency(self) -> None:
        # A C application that uses a C++ package must be linked with the C++
        # driver so the C++ runtime (libc++/libstdc++) is resolved.
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"c_links_cpp_{stamp}"
        pkg = root / "cpplib"
        app = root / "capp"
        pkg_id = f"tmp.cpplib.{stamp}"
        app_id = f"tmp.capp.{stamp}"

        (pkg / "src").mkdir(parents=True, exist_ok=True)
        (pkg / "include").mkdir(parents=True, exist_ok=True)
        (pkg / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{pkg_id}\",\n"
            "    \"type\": \"package\",\n"
            "    \"value\": { \"language\": \"c++\" }\n"
            "}\n"
        )
        (pkg / "include" / "cpplib.h").write_text(
            "#ifndef CPPLIB_H\n"
            "#define CPPLIB_H\n"
            "#ifdef __cplusplus\n"
            "extern \"C\" {\n"
            "#endif\n"
            "int cpplib_answer(void);\n"
            "#ifdef __cplusplus\n"
            "}\n"
            "#endif\n"
            "#endif\n"
        )
        (pkg / "src" / "lib.cc").write_text(
            "#include \"cpplib.h\"\n"
            "#include <stdexcept>\n"
            "extern \"C\" int cpplib_answer(void) {\n"
            "    try { throw std::runtime_error(\"boom\"); }\n"
            "    catch (const std::exception &) { return 42; }\n"
            "}\n"
        )

        (app / "src").mkdir(parents=True, exist_ok=True)
        (app / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{app_id}\",\n"
            "    \"type\": \"application\",\n"
            f"    \"value\": {{ \"use\": [\"{pkg_id}\"] }}\n"
            "}\n"
        )
        (app / "src" / "main.c").write_text(
            "#include \"cpplib.h\"\n"
            "#include <stdio.h>\n"
            "int main(void) { printf(\"answer=%d\\n\", cpplib_answer()); return 0; }\n"
        )
        try:
            output = self.bake(["--local-env", "run", "capp"], cwd=root)
            self.assertIn("answer=42", self.strip_ansi(output))
        finally:
            self._rm_tree(root)

    def test_header_only_bundle_exposes_root_and_propagates_includes(self) -> None:
        # A header-only bundle that lists extra include subdirs must still expose
        # its source root, and those include paths must propagate to dependents
        # of the package that declares the bundle.
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"hdr_bundle_{stamp}"
        bundle_repo = root / "hdrbundle_repo"
        pkg = root / "midpkg"
        app = root / "topapp"
        pkg_id = f"tmp.midpkg.{stamp}"
        app_id = f"tmp.topapp.{stamp}"

        (bundle_repo / "extra").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "hdrbundle.h").write_text(
            "#ifndef HDRBUNDLE_H\n#define HDRBUNDLE_H\n#define HDRBUNDLE_ROOT 1\n#endif\n"
        )
        (bundle_repo / "extra" / "hdrbundle_extra.h").write_text(
            "#ifndef HDRBUNDLE_EXTRA_H\n#define HDRBUNDLE_EXTRA_H\n#define HDRBUNDLE_EXTRA 2\n#endif\n"
        )
        self._git_init_repo(bundle_repo)

        (pkg / "src").mkdir(parents=True, exist_ok=True)
        (pkg / "include").mkdir(parents=True, exist_ok=True)
        (pkg / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{pkg_id}\",\n"
            "    \"type\": \"package\",\n"
            "    \"bundle\": {\n"
            "        \"hdrbundle\": {\n"
            f"            \"repository\": \"{bundle_repo.as_posix()}\",\n"
            "            \"header-only\": true,\n"
            "            \"include\": [\"extra\"]\n"
            "        }\n"
            "    }\n"
            "}\n"
        )
        (pkg / "include" / "midpkg.h").write_text(
            "#ifndef MIDPKG_H\n#define MIDPKG_H\nint midpkg_sum(void);\n#endif\n"
        )
        (pkg / "src" / "midpkg.c").write_text(
            "#include \"midpkg.h\"\n"
            "#include <hdrbundle.h>\n"
            "#include <hdrbundle_extra.h>\n"
            "int midpkg_sum(void) { return HDRBUNDLE_ROOT + HDRBUNDLE_EXTRA; }\n"
        )

        (app / "src").mkdir(parents=True, exist_ok=True)
        (app / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{app_id}\",\n"
            "    \"type\": \"application\",\n"
            f"    \"value\": {{ \"use\": [\"{pkg_id}\"] }}\n"
            "}\n"
        )
        # main.c relies on the bundle include paths reaching it via midpkg.
        (app / "src" / "main.c").write_text(
            "#include \"midpkg.h\"\n"
            "#include <hdrbundle.h>\n"
            "#include <hdrbundle_extra.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            "    printf(\"sum=%d direct=%d\\n\", midpkg_sum(), HDRBUNDLE_ROOT + HDRBUNDLE_EXTRA);\n"
            "    return 0;\n"
            "}\n"
        )
        try:
            output = self.bake(["--local-env", "run", "topapp"], cwd=root)
            text = self.strip_ansi(output)
            self.assertIn("sum=3", text)
            self.assertIn("direct=3", text)
        finally:
            self._rm_tree(root)

    def test_cmake_bundle_link_inputs_propagate_to_dependents(self) -> None:
        # The library produced by a cmake bundle must be linkable from dependents
        # of the package that declares it (the bundle's libpath/lib propagate).
        if shutil.which("cmake") is None:
            self.skipTest("cmake not available on PATH")
        if shutil.which("git") is None:
            self.skipTest("git not available on PATH")

        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"cmake_bundle_prop_{stamp}"
        bundle_repo = root / "calc_repo"
        pkg = root / "mathpkg"
        app = root / "mathapp"
        pkg_id = f"tmp.mathpkg.{stamp}"
        app_id = f"tmp.mathapp.{stamp}"

        (bundle_repo / "src").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "include").mkdir(parents=True, exist_ok=True)
        (bundle_repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(calc C)\n"
            "add_library(calc STATIC src/calc.c)\n"
            "target_include_directories(calc PUBLIC\n"
            "    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n"
            "    $<INSTALL_INTERFACE:include>)\n"
            "include(GNUInstallDirs)\n"
            "install(TARGETS calc\n"
            "    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}\n"
            "    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})\n"
            "install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\n"
        )
        (bundle_repo / "include" / "calc.h").write_text(
            "#ifndef CALC_H\n#define CALC_H\nint calc_add(int a, int b);\n#endif\n"
        )
        (bundle_repo / "src" / "calc.c").write_text(
            "#include \"calc.h\"\nint calc_add(int a, int b) { return a + b; }\n"
        )
        self._git_init_repo(bundle_repo)

        (pkg / "src").mkdir(parents=True, exist_ok=True)
        (pkg / "include").mkdir(parents=True, exist_ok=True)
        (pkg / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{pkg_id}\",\n"
            "    \"type\": \"package\",\n"
            "    \"bundle\": {\n"
            "        \"calc\": {\n"
            f"            \"repository\": \"{bundle_repo.as_posix()}\",\n"
            "            \"library\": \"calc\"\n"
            "        }\n"
            "    }\n"
            "}\n"
        )
        (pkg / "include" / "mathpkg.h").write_text(
            "#ifndef MATHPKG_H\n#define MATHPKG_H\nint mathpkg_compute(void);\n#endif\n"
        )
        # mathpkg.c references calc_add; the symbol must be resolved at the
        # final link of any dependent application.
        (pkg / "src" / "mathpkg.c").write_text(
            "#include \"mathpkg.h\"\n"
            "#include \"calc.h\"\n"
            "int mathpkg_compute(void) { return calc_add(40, 2); }\n"
        )

        (app / "src").mkdir(parents=True, exist_ok=True)
        (app / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{app_id}\",\n"
            "    \"type\": \"application\",\n"
            f"    \"value\": {{ \"use\": [\"{pkg_id}\"] }}\n"
            "}\n"
        )
        (app / "src" / "main.c").write_text(
            "#include \"mathpkg.h\"\n"
            "#include <stdio.h>\n"
            "int main(void) { printf(\"compute=%d\\n\", mathpkg_compute()); return 0; }\n"
        )
        try:
            output = self.bake(["--local-env", "run", "mathapp"], cwd=root)
            self.assertIn("compute=42", self.strip_ansi(output))
        finally:
            self._rm_tree(root)

    def test_discovery_skips_build_directories(self) -> None:
        # project.json files inside a build/ directory (e.g. nested CMake
        # build/_deps trees) must not be discovered as bake projects.
        stamp = int(time.time() * 1_000_000)
        root = self.repo_root / "test" / "tmp" / f"disc_skip_{stamp}"
        shared_id = f"tmp.disc.main.{stamp}"

        (root / "src").mkdir(parents=True, exist_ok=True)
        (root / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{shared_id}\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (root / "src" / "x.c").write_text("int x_value(void) { return 0; }\n")

        # Stray project nested under build/ that shares the main project's id.
        # If discovery descended into build/, this would raise a duplicate-id
        # error and fail the build.
        stray = root / "build" / "_deps" / "nested-src"
        (stray / "src").mkdir(parents=True, exist_ok=True)
        (stray / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{shared_id}\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (stray / "src" / "y.c").write_text("int y_value(void) { return 0; }\n")
        try:
            self.bake(["--local-env", "build", "."], cwd=root)
        finally:
            self._rm_tree(root)

    @unittest.skipIf(platform.system() == "Windows", "POSIX symlinks not supported on this Windows test setup")
    def test_bake_home_with_symlinked_include_does_not_delete_source(self) -> None:
        """When bake_home contains a symlink that points back into the project's
        source include tree (e.g. legacy bake2 ~/bake/include/<id> symlinks),
        bake's sync step must unlink the symlink rather than recurse through it
        and delete the source headers it points to."""
        stamp = int(time.time() * 1_000_000)
        project_id = "myproj_sym"
        tmp_root = self.repo_root / "test" / "tmp" / f"sync_symlink_{stamp}"
        project_dir = tmp_root / "project"
        bake_home = tmp_root / "fake_bake_home"
        include_dir = project_dir / "include"
        nested_include_dir = include_dir / project_id
        src_dir = project_dir / "src"

        nested_include_dir.mkdir(parents=True, exist_ok=True)
        src_dir.mkdir(parents=True, exist_ok=True)
        (bake_home / "include").mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (include_dir / f"{project_id}.h").write_text("extern int x;\n")
        (nested_include_dir / "important.h").write_text("/* important */\n")
        (nested_include_dir / "other.h").write_text("/* other */\n")
        (src_dir / f"{project_id}.c").write_text(f"#include <{project_id}.h>\nint x = 1;\n")

        symlink_path = bake_home / "include" / project_id
        os.symlink(str(nested_include_dir), str(symlink_path))

        before = sorted(p.relative_to(project_dir).as_posix() for p in nested_include_dir.rglob("*") if p.is_file())
        self.assertIn(f"include/{project_id}/important.h", before)

        env = self.env.copy()
        env["BAKE_HOME"] = str(bake_home)
        try:
            self.bake([], cwd=project_dir, env=env)
            after_source = sorted(p.relative_to(project_dir).as_posix() for p in nested_include_dir.rglob("*") if p.is_file())
        finally:
            shutil.rmtree(tmp_root, ignore_errors=True)

        missing = [path for path in before if path not in after_source]
        self.assertEqual(
            missing,
            [],
            "Source include files were deleted via a symlink in bake_home",
        )

    def test_bake_home_inside_project_does_not_delete_source_includes(self) -> None:
        stamp = int(time.time() * 1_000_000)
        project_id = "myproj"
        project_dir = self.repo_root / "test" / "tmp" / f"sync_overlap_{stamp}"
        include_dir = project_dir / "include"
        nested_include_dir = include_dir / project_id
        src_dir = project_dir / "src"

        nested_include_dir.mkdir(parents=True, exist_ok=True)
        src_dir.mkdir(parents=True, exist_ok=True)

        (project_dir / "project.json").write_text(
            "{\n"
            f"    \"id\": \"{project_id}\",\n"
            "    \"type\": \"package\"\n"
            "}\n"
        )
        (include_dir / f"{project_id}.h").write_text("extern int x;\n")
        (nested_include_dir / "important.h").write_text("/* important header */\n")
        (nested_include_dir / "other.h").write_text("/* another header */\n")
        (src_dir / f"{project_id}.c").write_text(f"#include <{project_id}.h>\nint x = 1;\n")

        before = sorted(p.relative_to(project_dir).as_posix() for p in include_dir.rglob("*") if p.is_file())

        env = self.env.copy()
        env["BAKE_HOME"] = str(project_dir)
        try:
            output = self.bake_expect_failure([], cwd=project_dir, env=env)
            after = sorted(p.relative_to(project_dir).as_posix() for p in include_dir.rglob("*") if p.is_file())
        finally:
            shutil.rmtree(project_dir, ignore_errors=True)

        self.assertIn("refusing to sync tree", self.strip_ansi(output))
        missing = [path for path in before if path not in after]
        self.assertEqual(
            missing,
            [],
            "Source include files were deleted by bake when bake_home overlaps the project path",
        )

    def test_worktree_unchanged_after_run(self) -> None:
        current_snapshot = self.git_snapshot()
        self.assertEqual(
            self.git_snapshot_before,
            current_snapshot,
            "Tracked files changed during run_tests.py execution",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
