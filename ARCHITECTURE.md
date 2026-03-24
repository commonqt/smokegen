# Smokegen Architecture

> **SMOKE** = **S**cripting **M**eta **O**bject **K**ompiler **E**ngine

Smokegen is the code-generator component of the SMOKE toolkit. It parses C++ header files (primarily Qt headers) using **LLVM/Clang** and produces SMOKE data tables — compact, introspectable descriptions of an entire C++ API that language bindings (Perl, Ruby, C#, Common Lisp, etc.) can consume at runtime.

---

## High-Level Pipeline

```
                    ┌────────────────────────────┐
                    │       Header files         │
                    │  (e.g. Qt public headers)  │
                    └────────────┬───────────────┘
                                 │
                    ┌────────────▼───────────────┐
                    │         main.cpp           │
                    │  CLI parsing, config XML   │
                    │  Clang ToolInvocation      │
                    └────────────┬───────────────┘
                                 │
           ┌─────────────────────▼──────────────────────┐
           │          Clang Frontend Pipeline           │
           │                                            │
           │  SmokegenFrontendAction                    │
           │    └─► SmokegenASTConsumer                 │
           │          ├─► SmokegenPPCallbacks (pre-     │
           │          │    processor: Qt macro inject)  │
           │          └─► SmokegenASTVisitor            │
           │               (visits decls, fills         │
           │                global registries)          │
           └─────────────────────┬──────────────────────┘
                                 │
                    ┌────────────▼───────────────┐
                    │    Global Type Registries  │
                    │  classes, enums, typedefs, │
                    │  functions, types, globals │
                    └────────────┬───────────────┘
                                 │
                    ┌────────────▼───────────────┐
                    │   Generator Plugin (.dll   │
                    │   / .so loaded at runtime) │
                    │   e.g. generator_smoke     │
                    │   or generator_dump        │
                    └────────────┬───────────────┘
                                 │
                    ┌────────────▼───────────────┐
                    │   Output files             │
                    │  smokedata.cpp, x_*.cpp,   │
                    │  _smoke.h                  │
                    └────────────────────────────┘
```

The process has **two phases**:

1. **Parsing** — Clang parses the C++ headers and the AST visitor populates in-memory registries of classes, methods, enums, typedefs, functions, and types.
2. **Generation** — A dynamically-loaded generator plugin reads those registries and writes output files.

---

## Directory Layout

```
smokegen/
├── main.cpp                  # Entry point, CLI, Clang invocation
├── frontendaction.h/.cpp     # Clang FrontendAction (creates the ASTConsumer)
├── astconsumer.h/.cpp        # Clang ASTConsumer (hooks preprocessor + visitor)
├── astvisitor.h/.cpp         # RecursiveASTVisitor — core parsing logic
├── ppcallbacks.h/.cpp        # Preprocessor callbacks (Qt macro injection)
├── defaultargvisitor.h/.cpp  # Resolves default argument expressions
├── type.h/.cpp               # In-memory type model + global registries
├── options.h/.cpp            # ParserOptions (shared between parser & generators)
├── smoke.h                   # Runtime Smoke class (consumed by bindings)
├── generator_export.h        # DLL export/import macros
├── config.h.in               # CMake-configured header (LIB_SUFFIX)
├── embedded_includes.h.in    # Embedded clang built-in headers (optional)
├── qobjectdefs-injected.h    # Custom Qt macro definitions injected into parse
├── CMakeLists.txt            # Top-level build file
│
├── generators/
│   ├── smoke/                # ★ The main SMOKE generator plugin
│   │   ├── generator_smoke.cpp      # Plugin entry point (generate())
│   │   ├── globals.h                # Options, Util, SmokeDataFile, SmokeClassFiles
│   │   ├── helpers.cpp              # Utility functions (type analysis, inheritance)
│   │   ├── writeSmokeDataFile.cpp   # Writes smokedata.cpp (tables)
│   │   └── writeClasses.cpp         # Writes x_*.cpp (method dispatch code)
│   └── dump/                 # Debug generator — dumps parsed info to stdout
│       └── generator_dump.cpp
│
├── smokebase/                # Tiny static library: Smoke class statics
│   └── smokebase.cpp
│
├── smokeapi/                 # CLI introspection tool for compiled SMOKE libs
│   └── main.cpp
│
├── deptool/                  # Dependency analysis tool for SMOKE libs
│   └── main.cpp
│
└── cmake/                    # CMake helper modules
```

---

## Core Components

### 1. Entry Point — `main.cpp`

Responsibilities:

- **Command-line parsing** — Accepts `-I` (include dirs), `-g` (generator name), `-qt` (Qt mode), `-t` (resolve typedefs), `-config` (XML config file), `-clangOptions`, and header files after `--`.
- **XML config file** — An optional XML file can set include dirs, defines lists, drop macros, generator name, etc.
- **Generator plugin loading** — Loads a shared library named `generator_<name>` (e.g. `generator_smoke.dll`) using `QLibrary` and resolves a `generate()` function symbol from it.
- **Clang ToolInvocation** — For each header file, constructs a Clang `ToolInvocation` with the appropriate compiler flags (`-I`, `-D`, `-x c++`, `-fsyntax-only`). An overlay file system injects embedded built-in headers.
- **Calling the generator** — After all headers are parsed, calls `generate()` from the loaded plugin, which reads the populated global registries and writes output.

### 2. Clang Frontend Pipeline

The parsing pipeline uses standard Clang LibTooling abstractions:

#### `SmokegenFrontendAction` (frontendaction.h/.cpp)
A `clang::ASTFrontendAction` subclass. For each translation unit, it creates a `SmokegenASTConsumer`. It configures the compiler to skip function bodies (they are irrelevant for binding generation) and suppresses certain warnings.

#### `SmokegenASTConsumer` (astconsumer.h/.cpp)
A `clang::ASTConsumer` that does two things during `Initialize()`:
1. Installs `SmokegenPPCallbacks` on the preprocessor.
2. For every top-level declaration group (`HandleTopLevelDecl`), calls `SmokegenASTVisitor::TraverseDecl`.

#### `SmokegenPPCallbacks` (ppcallbacks.h/.cpp)
Monitors file changes during preprocessing. When clang finishes processing `qobjectdefs.h`, this callback injects custom macro definitions from `qobjectdefs-injected.h`. These redefined macros (like `Q_SIGNALS`, `Q_SLOTS`, `Q_PROPERTY`, etc.) use `QT_ANNOTATE_ACCESS_SPECIFIER` and `QT_ANNOTATE_FUNCTION` to add Clang annotation attributes. This lets the AST visitor later detect which methods are signals, slots, or property accessors.

#### `SmokegenASTVisitor` (astvisitor.h/.cpp)
A `clang::RecursiveASTVisitor` — **the heart of the parser**. It visits:

| Visitor Method | What It Does |
|---|---|
| `VisitCXXRecordDecl` | Registers classes/structs/unions with their methods, fields, base classes, template status |
| `VisitEnumDecl` | Registers enums and their members |
| `VisitFunctionDecl` | Registers free (non-member) functions |
| `VisitTypedefNameDecl` | Registers typedefs and type aliases |

Each `register*` method converts the Clang AST node into the smokegen in-memory model (see §3) and stores it in the global registries. Key details:

- **Classes**: Records name, namespace, parent class, kind (class/struct/union), forward-declaration status, base classes, methods (with parameters, const, virtual, static, signal/slot annotations), and fields.
- **Methods**: Extracts return types, parameter types and default values, access specifiers, and flags (virtual, pure virtual, static, explicit, deleted, signal, slot, Q_PROPERTY accessor).
- **Default arguments**: Uses `DefaultArgVisitor` to fully qualify enum constants used in default argument expressions (e.g. `DefaultConversion` → `QTextCodec::DefaultConversion`).
- **Type resolution**: `registerType()` decomposes Clang `QualType` into the smokegen `Type` model — tracking pointer depth, references, const/volatile qualifiers, array dimensions, function pointers, and template arguments.
- **Typedef resolution**: When `ParserOptions::resolveTypedefs` is enabled, typedefs are resolved to their underlying types, preserving additional pointer depth from usage sites.

### 3. In-Memory Type Model — `type.h` / `type.cpp`

The parsed C++ API is stored in six **global registries** (all `StableHashMap` instances defined in `type.cpp`):

| Registry | Key | Value | Description |
|---|---|---|---|
| `classes` | Qualified name (e.g. `QWidget`) | `Class` | All C++ classes, structs, unions, namespaces |
| `enums` | Qualified name | `Enum` | Enumerations and their members |
| `typedefs` | Qualified name | `Typedef` | Type aliases |
| `functions` | Mangled signature | `Function` | Free (non-member) functions |
| `globals` | Name | `GlobalVar` | Global variables |
| `types` | String representation | `Type` | Deduplicated type descriptors |

`StableHashMap<T>` is a `QHash<QString, std::shared_ptr<T>>` wrapper that provides pointer stability — pointers to values remain valid even when the hash is modified. This is critical because types, classes, and methods cross-reference each other via raw pointers.

**Class hierarchy:**

```
BasicTypeDeclaration          — name, namespace, parent class, access, file
  ├── Class                   — kind, methods[], fields[], baseClasses[], children[], isTemplate
  ├── Enum                    — members[] (EnumMember), isScoped
  └── Typedef                 — aliased Type*, resolve()

Member                        — declaring type, name, Type*, access, flags (virtual/static/...)
  ├── Method                  — parameters[], isConstructor/Destructor/Const/Signal/Slot/Deleted
  ├── Field                   — (no additions beyond Member)
  └── EnumMember              — string value

Type                          — class/typedef/enum/name, const, volatile, pointerDepth,
                                isRef, isIntegral, isFunctionPointer, templateArguments[],
                                arrayDimensions[], parameters[] (for fn ptrs)

Parameter                     — name, Type*, defaultValue

Function (inherits GlobalVar) — parameters[]
GlobalVar                     — name, namespace, Type*, fileName
```

### 4. Parser Options — `options.h` / `options.cpp`

`ParserOptions` is a simple struct of static fields shared between the parser and generator plugins (via the export macro `GENERATOR_EXPORT`):

| Field | Purpose |
|---|---|
| `headerList` | Header files to parse |
| `includeDirs` | `-I` directories |
| `frameworkDirs` | macOS framework directories |
| `definesList` | File containing `#define` lines to pass to clang |
| `dropMacros` | Macros to ignore |
| `resolveTypedefs` | Whether to resolve typedefs to underlying types |
| `qtMode` | Enables special handling of QFlags, signals/slots |
| `notToBeResolved` | Typedefs that should never be resolved (e.g. `FILE`) |

### 5. Qt Macro Injection — `qobjectdefs-injected.h`

Qt's `qobjectdefs.h` defines macros like `Q_OBJECT`, `Q_PROPERTY`, `signals`, `slots` in ways that are meaningful to `moc` but not to a regular C++ compiler. Smokegen re-defines these macros so that they produce **Clang annotation attributes** (`__attribute__((annotate("qt_signal")))`, etc.). This allows the AST visitor to detect signals, slots, and properties directly from the AST.

The injection happens via `SmokegenPPCallbacks::InjectQObjectDefs()`, which creates an in-memory source buffer and pushes it into Clang's preprocessor right after `qobjectdefs.h` has been included.

---

## Generator Plugins

Generator plugins are shared libraries loaded at runtime via `QLibrary`. They must export a C function:

```c
extern "C" int generate();
```

The plugin reads the global registries (`classes`, `enums`, `functions`, `typedefs`, `types`) populated by the parser, and produces output files.

### `generator_smoke` (generators/smoke/)

The primary generator. It produces the SMOKE data tables that language bindings use at runtime.

**Entry point**: `generator_smoke.cpp` → `generate()`:
1. Parses its own command-line options (`-m` module name, `-p` parts, `-pm` parent modules, `-st` scalar types, `-smokeconfig` config file, `-o` output dir, etc.).
2. Reads an optional XML smoke config file for additional settings.
3. Calls `SmokeDataFile::write()` and `SmokeClassFiles::write()`.

**Output files:**

| File | Content |
|---|---|
| `smokedata.cpp` | The main SMOKE data file containing arrays for classes, methods, method maps, types, inheritance lists, argument lists, method names, and the cast function. |
| `x_1.cpp` … `x_N.cpp` | Method dispatch code — one file per "part" (default 20 parts). Each contains `xcall_<ClassName>` functions that translate SMOKE stack-based calls into actual C++ method calls. |
| `<module>_smoke.h` | Header declaring the Smoke module pointer and init function. |

**Key structures in `globals.h`:**

- **`SmokeDataFile`** — Builds class/method/type indices, determines which classes are "external" (from parent modules), computes virtual method tables, and writes `smokedata.cpp`.
- **`SmokeClassFiles`** — Generates the `x_*.cpp` files. For each class, it writes:
  - `xcall_<Class>(short index, void* obj, Smoke::Stack args)` — a switch statement dispatching to the actual C++ method based on index.
  - Virtual method override stubs for the `x_<Class>` wrapper class.
  - Enum accessor functions.
  - Field getter/setter functions.
- **`Util`** — Static helper functions for type analysis, inheritance traversal, method munging (SMOKE's `$#?` encoding for argument types), and determining if classes can be instantiated/copied/destroyed.

**Method Munging:** SMOKE encodes method signatures into compact "munged" names where:
- `$` = scalar type (int, bool, enum, etc.)
- `#` = object type (class/struct by value or reference)
- `?` = non-scalar (pointer, array, etc.)

This enables efficient method lookup by language bindings.

### `generator_dump` (generators/dump/)

A trivial debug generator that simply prints all parsed class names and type names to stdout. Useful for verifying that parsing works correctly:

```
smokegen -g dump -- header.h
```

---

## The Smoke Runtime — `smoke.h` / `smokebase/`

The `Smoke` class (defined in `smoke.h`) is the runtime data structure consumed by language bindings. It contains:

| Array | Content |
|---|---|
| `classes[]` | Class descriptors (name, parents index, classFn, enumFn, flags, size) |
| `methods[]` | Method descriptors (class, name index, args index, numArgs, flags, return type, dispatch index) |
| `methodMaps[]` | Maps munged method names to method indices |
| `methodNames[]` | String table of method names |
| `types[]` | Type descriptors (name, class index, flags for stack/ptr/ref/const) |
| `inheritanceList[]` | Groups of parent class indices |
| `argumentList[]` | Groups of argument type indices |
| `ambiguousMethodList[]` | Overloaded method resolution |

The `Smoke` class provides lookup methods: `idClass()`, `idMethod()`, `idType()`, `findClass()`, `findMethod()` — all using binary search over sorted arrays for efficiency.

`smokebase.cpp` defines the static members `Smoke::classMap` and `Smoke::NullModuleIndex`.

---

## Auxiliary Tools

### `smokeapi` (smokeapi/)
A command-line introspection tool for compiled SMOKE libraries. It loads a SMOKE `.so`/`.dll`, resolves its `init_<module>_Smoke` function, and prints classes, methods, enums, and other information. Useful for debugging and verifying generated bindings.

### `deptool` (deptool/)
Analyzes dependencies between SMOKE modules. It loads multiple SMOKE libraries and determines which modules depend on which by checking which classes are marked as `external` (defined in a parent module). Can output results as XML.

---

## Build System

The project uses **CMake** and requires:
- **Qt 6** (Core and Xml modules) — for string handling, containers, XML config parsing, dynamic library loading
- **LLVM/Clang** (tested with LLVM 19) — for C++ parsing
- **C++17** compiler

Key build targets:

| Target | Type | Description |
|---|---|---|
| `smokegen` | Executable | The main parser/generator tool |
| `generator_smoke` | Shared module | SMOKE output generator plugin |
| `generator_dump` | Shared module | Debug dump generator plugin |
| `smokebase` | Shared library | Smoke runtime statics |
| `smokeapi` | Executable | SMOKE library introspection tool |
| `smokegen_deptool` | Executable | SMOKE module dependency analyzer |

Generator plugins are installed to `<prefix>/lib/smokegen/` and smokegen searches for them there at runtime.

---

## Data Flow Summary

```
1.  CLI args + XML config
         │
2.  Clang parses each header file:
         │
         ├── Preprocessor ──► SmokegenPPCallbacks
         │                    (injects Qt macro annotations)
         │
         └── AST ──► SmokegenASTVisitor
                      ├── registerClass()  ──► classes registry
                      ├── registerEnum()   ──► enums registry
                      ├── registerFunction() ──► functions registry
                      ├── registerTypedef() ──► typedefs registry
                      └── registerType()   ──► types registry
         │
3.  Generator plugin loaded (QLibrary)
         │
4.  generate() called:
         ├── SmokeDataFile: builds indices, writes smokedata.cpp
         └── SmokeClassFiles: writes x_*.cpp dispatch code
         │
5.  Output files compiled into a SMOKE shared library
    (done externally, e.g. by smokeqt's CMake build)
```

---

## Key Design Decisions

- **Plugin architecture for generators**: The parser and generator are decoupled. Different generators can be written without modifying the parser. Plugins are loaded at runtime via `QLibrary`.
- **Pointer stability via StableHashMap**: Since the in-memory model uses raw pointers extensively (types reference classes, methods reference types, etc.), the hash map guarantees that pointers remain valid after insertions by storing values in `std::shared_ptr`.
- **Qt macro injection**: Instead of requiring moc or a custom preprocessor, smokegen re-defines Qt macros to produce Clang annotation attributes, letting the standard Clang parser capture signal/slot/property information.
- **Method munging**: SMOKE's `$#?` encoding compresses method signatures for fast runtime lookup by language bindings.
- **Skipping function bodies**: Since only the API surface matters (declarations, not implementations), the parser tells clang to skip function bodies for performance.
- **Embedded built-in headers**: Clang built-in headers can be embedded via an overlay filesystem, making the tool self-contained without requiring a full clang installation at runtime.
