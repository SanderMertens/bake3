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

        cls.run_cmd(["make", "clean"])
        cls.run_cmd(["make", "-j", "8"])
        cls.bake(["setup", "--local"])

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

    def test_worktree_unchanged_after_run(self) -> None:
        current_snapshot = self.git_snapshot()
        self.assertEqual(
            self.git_snapshot_before,
            current_snapshot,
            "Tracked files changed during run_tests.py execution",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
