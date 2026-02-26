# Bake
Bake is a build system that provides a simple JSON configuration and fast builds. It provides simplified dependency management by letting projects refer to dependencies using logical names. This decouples projects from what is installed on a system, and prevents having to specify absolute or relative paths to other dependency code.

Bake accomplishes this by creating entries for projects in a bake environment (see below), which allows bake to find projects and build artefacts by logical name.

Bake is not yet-another-cmake-or-make wrapper. Instead, it emits compiler commands directly which speeds up builds and simplifies installation. Bake supports clang, gcc and msvc on MacOS, Linux and Windows.

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
  --run-prefix <cmd>  Prefix command when running binaries
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
- `standalone`: When true, this will copy all amalgamated sources from dependencies to a `deps` folder in the project, and include those in the project build rather than relying on linking with dependency binaries. This allows for the project to be easily shared, without having to also share the dependencies.

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
- `c-standard`: Specify the C standard to use for C files
- `cpp-standard`: Specify the C++ standard to use for C++ files
- `export-symbols`: Export symbols if true (default is false)

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

## Conditional configuration
Sometimes a project may want to apply a configuration only on a specific operation system or for a specific compiler. This can be accomplished by surrounding the conditional configuration like so:

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

## Project discovery
When bake is called on a directory, it will recursively discover all other bake projects in that directory. A bake project is identified as a project with a `project.json`. The command specified on the bake command line will then be executed for all discovered projects.

When projects have dependencies on each other, bake will ensure that they are built in the correct dependency order.

If one of the discovered projects have a dependency on a project that is not discovered in the specified directory, the binary of the project will be looked up in the bake environment (see below).

If the project also cannot be found in the bake environment, the build cannot proceed, and an error will be thrown.

## Bake Environment
When a project is built with bake, an entry for it will be stored in the bake environment. The location of the bake environment is read from the `BAKE_HOME` environment variable. If the variable is not set, `~/bake3` is used.

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

## Testing
To test bake, run `run_tests.sh` in the root folder of the project.
