# Bake
Bake is a build system that provides a simple JSON configuration and fast builds. It provides simplified dependency management by letting projects refer to dependencies using logical names. This decouples projects from what is installed on a system, and prevents having to specify absolute or relative paths to other dependency code.

Bake accomplishes this by creating entries for projects in a bake environment (see below), which allows bake to find projects and build artefacts by logical name.

Bake is not yet-another-cmake-or-make wrapper. Instead, it emits compiler commands directly which speeds up builds and simplifies installation. Bake supports clang, gcc and msvc on MacOS, Linux and Windows.

**WARNING: Heavy work in progress. If you are looking for a stable build system, do not use this!! Take a look instead at https://github.com/SanderMertens/bake**

## Getting started
To install bake, run the following command:

```
./setup.sh
```

This might prompt you for a password once as the script has to to install a script to `/usr/local/bin`. Once this script is installed, providing a password during setup is no longer needed.

## Commands
Build project in current directory:
```
bake
bake .
bake build
bake build .
```

Clean project in current directory:
```
bake clean
```

Rebuild project in current directory:
```
bake rebuild
```

Build project in specified directory:
```
bake my_app
bake build my_app
bake build projects/my_app
```

Clean project in specified directory:
```
bake clean my_app
bake clean projects/my_app
```

Rebuild project in specified directory:
```
bake rebuild my_app
bake rebuild projects/my_app
```

Run project in current directory (will recursively build dependencies):
```
bake run
bake run .
```

Run project in specified directory:
```
bake run my_app
bake run projects/my_app
```

The project is run with its own directory as working directory, so a project can
load files relative to its own location.

Pass arguments to project:
```
bake run -- --key=value
bake run my_app -- --key=value
```

## Usage
```
Usage: bake [options] [command] [target]

Commands:
  build [target]      Build target project and dependencies (default)
  run [target]        Build and run executable target
  test [target]       Build and run test target
  clean [target]      Remove build artifacts
  rebuild [target]    Clean and build
  list                List projects in bake environment
  info <target>       Show project info
  cleanup             Remove stale projects from bake environment
  reset               Reset bake environment metadata
  setup               Install bake executable into bake environment

Options:
  --cfg <mode>        Build mode: sanitize|debug|profile|release
  --cc <compiler>     Override C compiler
  --cxx <compiler>    Override C++ compiler
  --target <name>     Cross-compile target (em = emscripten/wasm)
  --run-prefix <cmd>  Prefix command when running binaries
  --local-env[=<name>] Use ./.bake/local_env (or ./.bake/local_env/<name>) as isolated BAKE_HOME and build root
  --local             Setup only: install into BAKE_HOME (skip /usr/local/bin)
  --standalone        Use amalgamated dependency sources in deps/
  --strict            Enable strict compiler warnings and checks
  --trace             Enable trace logging (Flecs log level 0)
  -j <count>          Number of parallel jobs for build/test execution
  -r                  Apply command recursively to project and project dependencies
  -h, --help          Show this help
```

## Project structure
Bake projects store files in well known locations, to keep build configuration simple. Those locations are:

- `src`: Directory that stores the source files. May contain other directories that store source files.
- `include`: Directory that stores public include files. Files in this directory will be accessible to the project as well as dependees.
- `etc`: Project assets. These will be accessible from the project binary when the project is ran.

When a project is built, the build artefacts will be stored in:

- `.bake/arch-os-config`: Stores the executable or library binary
- `.bake/arch-os-config/obj`: Stores the generated object files
- `.bake/arch-os-config/generated`: Stores generated files (such as header dependencies)

For example:

`.bake/arm64-Darwin-debug/libflecs.a`

When `--local-env` is used, build artefacts are isolated to:

- `.bake/local_env/build/<project-id>/arch-os-config`: Stores the executable or library binary
- `.bake/local_env/build/<project-id>/arch-os-config/obj`: Stores generated object files
- `.bake/local_env/build/<project-id>/arch-os-config/generated`: Stores generated files (such as header dependencies)

When `--local-env=<name>` is used, build artefacts are isolated to:

- `.bake/local_env/<name>/build/<project-id>/arch-os-config`: Stores the executable or library binary
- `.bake/local_env/<name>/build/<project-id>/arch-os-config/obj`: Stores generated object files
- `.bake/local_env/<name>/build/<project-id>/arch-os-config/generated`: Stores generated files (such as header dependencies)

## Project configuration
Bake projects are configured with a `project.json` file in the root of the project directory. The simplest configuration looks like this:

```json
{
    "id": "my_app",
    "type": "application"
}
```

The type of a project may be either `application` or `package`, which respectively builds an executable or library.

Project settings can be configured in a `value` property. For example:

```json
{
    "id": "my_app",
    "type": "application",
    "value": {
        "use": ["flecs", "cglm"]
    }
}
```

The following options are supported:
- `use`: Specify a list of dependencies. Dependencies always use logical names which match exactly the name provided in the `id` field.
- `use-private`: Same as `use`, but headers of dependencies will not be visible to dependees.
- `language`: Specify the language. May be `c` (default), `c++` or `cpp` (same as `c++`).
- `public`: When false, the project will not be copied to the bake environment (see below). Default is true.
- `amalgamate`: Specify whether the project should amalgamated the source files.
- `amalgamate-path`: Destination path for the output of the amalgamation process.
- `output`: Name of the build artefact. Defaults to the project id.
- `standalone`: When true, this will copy all amalgamated sources from dependencies to a `deps` folder in the project, and include those in the project build rather than relying on linking with dependency binaries. This allows for the project to be easily shared, without having to also share the dependencies. The sources in `deps` are refreshed automatically when a dependency changes. When the dependency sources are not available (for example on a machine that only has the standalone project), the existing sources in `deps` are used as is.

## Language configuration
Projects can configure options that are specific to the programming language of the project by adding a `lang.c` or `lang.cpp` section to the project configuration. For example:

```json
{
    "id": "my_app",
    "type": "application",
    "value": {
        "use": ["flecs", "cglm"]
    },
    "lang.c": {
        "lib": ["m", "pthreads"]
    }
}
```

The following configuration options are available:

- `cflags`: list of arguments to pass to the C compiler
- `cxxflags`: list of arguments to pass to the C++ compiler
- `ldflags`: list of preprocessor defines to add to the linker
- `lib`: list libraries to link with
- `libpath`: list of paths to use for resolving  libraries
- `defines`: list of preprocessor defines to add to the compiler
- `include`: list of additional include paths
- `embed`: list of files and directories to embed in the artefact (Emscripten only, see below)
- `shell`: html page emcc uses as the template for the artefact (Emscripten only, see below)
- `c-standard`: Specify the C standard to use for C files
- `cpp-standard`: Specify the C++ standard to use for C++ files

Each configuration key has a single supported spelling. When bake finds an
unknown key that resembles a supported one, it says so instead of ignoring it:

```
[warning] unknown project key 'libs', did you mean 'lib'?
```

Keys that resemble nothing bake knows (`author`, `description`) stay silent, so
project metadata does not produce warnings.

## Dependee configuration
Projects may add a `dependee` section to their project configuration which contains configuration that will be applied to dependee projects. The structure of a dependee object mirrors that of the project configuration. The following example makes sure that any project that uses `my_library` will also have `flecs` as a dependency and link with `libm`.

```json
{
    "id": "my_libary",
    "type": "package",
    "value": {
        "use": ["flecs", "cglm"]
    },
    "dependee": {
        "value": {
            "use": ["flecs"]
        },
        "lang.c": {
            "lib": ["m"]
        }
    }
}
```

When a dependee configuration specifies a property that accepts a list, the list will be appended to the dependees own list if it existts. For example:

```json
{
    "id": "my_app",
    "type": "package",
    "value": {
        "use": ["my_library", "cglm"]
    }
}
```

This project will have `my_library`, `cglm` and `flecs` as dependencies (in addition to linking with `libm`).

## Bundles
A project can declare external dependencies that bake fetches and builds automatically. These are useful for pulling in third-party libraries that are not bake projects themselves. Each entry under `bundle` maps a logical name (which can be referenced from `use`) to a git repository. Bake clones the repository, runs CMake to configure, build and install it, and registers the result as a dependency.

```json
{
    "id": "my_app",
    "type": "application",
    "value": {
        "use": ["glfw"]
    },
    "bundle": {
        "glfw": {
            "repository": "https://github.com/glfw/glfw.git",
            "tag": "3.4",
            "library": "glfw3",
            "cmake-args": ["-DGLFW_BUILD_DOCS=OFF", "-DGLFW_BUILD_TESTS=OFF"]
        }
    }
}
```

The following options are supported per bundle entry:
- `repository`: Git URL or local path to clone (required).
- `branch` / `tag`: Optional ref to check out at clone time.
- `commit`: Optional specific commit hash to check out (will fetch full history if needed).
- `subdir`: Optional path inside the cloned repository where the build manifest lives.
- `library`: Name of the library produced (without `lib` prefix or extension). Defaults to the bundle id.
- `build-system`: `"cmake"` (default) or `"cargo"`. With `cargo`, bake runs `cargo build --release --target-dir <build>` instead of cmake; cross-compilation targets are forwarded to Cargo and the artefact is resolved from the corresponding target directory.
- `header-only`: When `true`, skip the build step entirely and only expose the cloned source tree as include paths (useful for header-only libraries).
- `include`: List of subdirectories of the bundle source to add to the consuming project's include path (in addition to the default `<install>/include` for built bundles or the bundle root for header-only bundles).
- `sources`: List of source files (relative to the bundle source) to compile alongside the consuming project's own sources. Useful for "drop-in" `.c` files like miniz.
- `cmake-args`: List of extra arguments passed to `cmake` during configuration.
- `lib`: System libraries the bundle depends on at link time.
- `ldflags`: Extra link flags to apply when linking the consuming project.

Bundles are project-scoped: each project's bundles are fetched and built under that project's own `.bake/bundles/<id>/<ref>/{src,build/<triplet>,install/<triplet>}` tree, where `<ref>` is `commits/<hash>`, `tags/<tag>`, `branches/<branch>`, or `default` (when no ref is pinned). The most specific ref wins (`commit` > `tag` > `branch`). Two projects in the same workspace pinning different versions of the same bundle do not interfere with each other. Bundles are only fetched and built once per ref-scoped path; subsequent builds reuse them.

What a bundle contributes does propagate transitively: the include paths, library paths, libraries and `ldflags` a bundle adds to its own project are also applied to every project that uses it, so a dependee links against the bundle without redeclaring it. The exception is `sources`, which are compiled into the declaring project only.

## Conditional configuration
Sometimes a project may want to apply a configuration only on a specific operating system or for a specific build target. This can be accomplished by surrounding the conditional configuration like so:

```json
{
    "id": "my_app",
    "type": "application",
    "value": {
        "use": ["flecs", "cglm"]
    },
    "lang.c": {
        "${os linux}": {
            "lib": ["m", "pthreads"]
        }
    }
}
```

The following conditional kinds are supported:
- `${os <name>}`: matches the build target OS (`Darwin`, `Linux`, `Windows`, `Emscripten`, ...). This equals the host OS unless cross-compiling with `--target`.
- `${arch <name>}`: matches the build target architecture (`arm64`, `x64`, `wasm32`, ...).
- `${cfg <mode>}`: matches the build mode (`debug`, `release`, ...).
- `${target <name>}`: matches the `--target` value (e.g. `em`).

Conditional blocks may appear at the top level (where they can introduce e.g. `bundle` entries) as well as inside `lang.c`, `lang.cpp` and `dependee` sections.

## Cross-compilation
Bake can cross-compile a project to a non-host target with `--target <name>`. The only target currently supported is `em` (Emscripten / WebAssembly):

```sh
bake build --target em
bake build my_app --target em
```

When `--target em` is used, bake:
- defaults the compiler to `emcc` / `em++` (still overridable with `--cc` / `--cxx`),
- archives static libraries with `emar`,
- configures `bundle` dependencies with `emcmake cmake`,
- builds Cargo bundles with `--target wasm32-unknown-emscripten` and `--crate-type staticlib`,
- emits a `wasm32-Emscripten-<cfg>` triplet so wasm artefacts never clash with native ones,
- gives application targets a `.js` artefact, or an `.html` one when a `shell` is configured (emscripten also emits the sibling `.wasm` file next to it),
- passes the configuration's optimisation level to the link as well as the compile, because for emcc the link-time `-O` level selects which variant of the system libraries is linked,
- omits `-pg` from `--cfg profile`, which emcc does not implement.

### Finding the SDK
If `emcc` is not already on `PATH`, bake sources `emsdk_env.sh` from the first of `$EMSDK`, `$EMSDK_DIR` and `~/GitHub/emsdk` that has one, and imports the resulting `PATH` and `EM*` variables for the build. Set `EMSDK` to use a specific installation:

```sh
EMSDK=~/GitHub/emsdk-latest bake build my_app --target em
```

### Target-specific flags
Use `${os Emscripten}` / `${target em}` conditionals in `project.json` to select target-specific dependencies and flags:

```json
"lang.c": {
    "${target em}": {
        "cflags": ["--use-port=emdawnwebgpu"],
        "ldflags": ["-sUSE_GLFW=3", "--use-port=emdawnwebgpu", "-sDEFAULT_TO_CXX", "-Wl,-u,ntohs"],
        "embed": ["etc"],
        "shell": "etc/index.html"
    }
}
```

Bake adds `-s ALLOW_MEMORY_GROWTH=1`, `-s EXPORTED_RUNTIME_METHODS=cwrap`, `-s MODULARIZE=1` and `-s EXPORT_NAME="<output>"` to the link. These are emitted *before* the project's own `ldflags`, so a project that wants different values simply sets them (`"ldflags": ["-sMODULARIZE=0"]`). With `MODULARIZE=1` the `.js` file defines a factory function named after the project output rather than starting the module, so the page has to call it.

### Embedding assets
`embed` entries become `--embed-file` arguments, which place the bytes in the wasm data segment. Relative entries are resolved against the project directory, and are mounted in MEMFS under the path as written, so the same relative path works on both targets:

```json
"lang.c": {
    "${target em}": { "embed": ["etc"] }
}
```

`<project>/etc` is embedded and `fopen("etc/config.json")` finds it, because emscripten's working directory is `/` and bake runs native builds from the project directory. Two escape hatches are passed through untouched: an absolute path, and an explicit `src@dst` mapping that names the MEMFS destination.

### The page
`shell` names an html file, relative to the project directory, that emcc uses as the template for the artefact (`--shell-file`). Setting it makes the artefact an `.html` file instead of a `.js` one; the `.js` and `.wasm` are still written next to it. The template must contain the `{{{ SCRIPT }}}` placeholder, which emcc replaces with a `<script>` tag for the `.js` file.

`bake run --target em` cannot execute a wasm artefact, so instead it serves the directory the artefact was linked into over a local http server and opens the artefact (or an `index.html` next to it) in a browser. Set `BROWSER` to a command that does nothing (`BROWSER=true`) to keep it from opening a window. The equivalent by hand is:

```sh
python3 -m http.server 8080 -d <project>/.bake/wasm32-Emscripten-<cfg>
```

Syncing an emscripten artefact into the bake environment copies the sibling `.wasm`, `.data` and `.js` files along with it, so an installed artefact is loadable.

## Project discovery
When bake is called on a directory, it will recursively discover all other bake projects in that directory. A bake project is identified as a project with a `project.json`. The command specified on the bake command line will then be executed for all discovered projects.

When projects have dependencies on each other, bake will ensure that they are built in the correct dependency order.

If one of the discovered projects have a dependency on a project that is not discovered in the specified directory, the binary of the project will be looked up in the bake environment (see below).

If the project also cannot be found in the bake environment, the build cannot proceed, and an error will be thrown.

## Bake Environment
When a project is built with bake, an entry for it will be stored in the bake environment. The location of the bake environment is read from the `BAKE_HOME` environment variable. If the variable is not set, `~/bake3` is used.

When `--local-env` is used, bake overrides `BAKE_HOME` for that invocation to `./.bake/local_env`. When `--local-env=<name>` is used, bake overrides it to `./.bake/local_env/<name>`.

The bake environment has the following directories:

- `<arch-os>/<config>/bin`: stores application binaries
- `<arch-os>/<config>/lib`: stores library binaries
- `include/<project>`: stores the `include` folder of a project
- `meta/<project>`: stores project metadata

A project meta folder stores:
- `project.json`: Copy of the bake configuration for the project
- `LICENSE`: Copy of the license file of the project
- `source.txt`: File with the location of the last location from which the project was built
- `dependee.json`: Project configuration to apply to dependees of the project (copy of the `dependee` section in the project's project.json)

## Building bake
To build bake, run the following command in the repository root:

```
make -j 8
```

Then run this command to initialize the bake environment:

```
./build/bake setup
```

## Testing
To test bake, run `python3 run_tests.py` in the root folder of the project.

Each test removes the workspace it created under `test/tmp`. Set
`BAKE_TEST_KEEP_TMP=1` to keep those trees for debugging.

The suite also builds and runs the C unit tests that cover bake's path, string
and list helpers. To run them on their own:

```
make unit
./build/bake_unit_tests
```
