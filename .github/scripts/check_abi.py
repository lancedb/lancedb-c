#!/usr/bin/env python3
"""Compare the public C declarations with declarations generated from Rust."""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

CBINDGEN_VERSION = "0.29.4"


@dataclasses.dataclass(frozen=True)
class CType:
    kind: str  # base, pointer, function
    name: str = ""
    const: bool = False
    inner: Optional["CType"] = None
    ret: Optional["CType"] = None
    params: Tuple["CType", ...] = ()
    variadic: bool = False


@dataclasses.dataclass(frozen=True)
class Function:
    type: CType
    param_names: Tuple[str, ...]


def parse_type(text: str) -> CType:
    text = re.sub(r"\s+", " ", text.strip())
    text = re.sub(r"\b(?:struct|enum|union)\s+", "", text)
    # Function type: RET (PARAMS)
    open_paren = text.find("(")
    if open_paren != -1 and text.endswith(")"):
        # Ensure this is the outermost parameter list, not a parenthesized base.
        depth = 0
        close = -1
        for i, ch in enumerate(text):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    close = i
                    break
        if close == len(text) - 1:
            return CType("function", ret=parse_type(text[:open_paren]), params=parse_params(text[open_paren + 1:-1]))
    # Function pointer: RET (*)(PARAMS)
    match = re.match(r"^(.*?)\s*\(\s*\*\s*\)\s*\((.*)\)$", text)
    if match:
        return CType(
            "pointer",
            inner=CType(
                "function",
                ret=parse_type(match.group(1)),
                params=parse_params(match.group(2)),
            ),
        )
    # Pointer declarator with optional const after each '*'.
    tokens = text.replace("*", " * ").split()
    base_tokens: List[str] = []
    pointers: List[bool] = []
    base_const = False
    for token in tokens:
        if token == "*":
            pointers.append(False)
        elif token == "const":
            if pointers:
                pointers[-1] = True
            else:
                base_const = True
        else:
            base_tokens.append(token)
    base = normalize_base(" ".join(base_tokens), base_const)
    for const in pointers:
        base = CType("pointer", const=const, inner=base)
    return base


def parse_params(text: str) -> Tuple[CType, ...]:
    text = text.strip()
    if not text or text == "void":
        return ()
    parts: List[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
    parts.append(text[start:].strip())
    params: List[CType] = []
    for part in parts:
        if part == "...":
            params.append(CType("base", name="..."))
        else:
            params.append(parse_type(part))
    return tuple(params)


TARGET_SIZES: Dict[str, int] = {}


def integer_aliases() -> Dict[str, str]:
    int_bits = TARGET_SIZES.get("int", 4) * 8
    short_bits = TARGET_SIZES.get("short", 2) * 8
    long_bits = TARGET_SIZES.get("long", 8) * 8
    long_long_bits = TARGET_SIZES.get("long long", 8) * 8
    pointer_bits = TARGET_SIZES.get("pointer", 8) * 8
    return {
        "bool": "bool",
        "_Bool": "bool",
        "void": "void",
        "char": "char",
        "signed char": "int:8",
        "unsigned char": "uint:8",
        "int8_t": "int:8",
        "uint8_t": "uint:8",
        "int16_t": "int:16",
        "uint16_t": "uint:16",
        "int32_t": "int:32",
        "uint32_t": "uint:32",
        "int64_t": "int:64",
        "uint64_t": "uint:64",
        "short": f"int:{short_bits}",
        "short int": f"int:{short_bits}",
        "signed short": f"int:{short_bits}",
        "signed short int": f"int:{short_bits}",
        "unsigned short": f"uint:{short_bits}",
        "unsigned short int": f"uint:{short_bits}",
        "int": f"int:{int_bits}",
        "signed": f"int:{int_bits}",
        "signed int": f"int:{int_bits}",
        "unsigned": f"uint:{int_bits}",
        "unsigned int": f"uint:{int_bits}",
        "long": f"int:{long_bits}",
        "long int": f"int:{long_bits}",
        "signed long": f"int:{long_bits}",
        "signed long int": f"int:{long_bits}",
        "unsigned long": f"uint:{long_bits}",
        "unsigned long int": f"uint:{long_bits}",
        "long long": f"int:{long_long_bits}",
        "long long int": f"int:{long_long_bits}",
        "signed long long": f"int:{long_long_bits}",
        "signed long long int": f"int:{long_long_bits}",
        "unsigned long long": f"uint:{long_long_bits}",
        "unsigned long long int": f"uint:{long_long_bits}",
        "size_t": f"uint:{pointer_bits}",
        "uintptr_t": f"uint:{pointer_bits}",
        "ssize_t": f"int:{pointer_bits}",
        "intptr_t": f"int:{pointer_bits}",
        "ptrdiff_t": f"int:{pointer_bits}",
        "float": f"float:{TARGET_SIZES.get('float', 4) * 8}",
        "double": f"float:{TARGET_SIZES.get('double', 8) * 8}",
        "long double": f"float:{TARGET_SIZES.get('long double', 16) * 8}",
    }


def normalize_base(text: str, const: bool) -> CType:
    text = " ".join(text.split())
    aliases = integer_aliases()
    if text in aliases:
        return CType("base", name=aliases[text], const=const)
    return CType("base", name=text, const=const)


@dataclasses.dataclass
class Declarations:
    functions: Dict[str, Function]
    structs: Dict[str, List[Tuple[str, CType]]]
    enums: Dict[str, List[int]]
    struct_locations: Dict[str, int]


def _main_location(loc: dict) -> bool:
    return bool(loc) and "file" not in loc and "includedFrom" not in loc


def _constant_value(node: dict) -> Optional[int]:
    for child in node.get("inner", []):
        if child.get("kind") == "ConstantExpr" and "value" in child:
            return int(child["value"], 0)
    return None


def collect_declarations(ast_path: Path) -> Declarations:
    ast = json.loads(ast_path.read_text())
    by_id: Dict[str, List[dict]] = {}
    typedefs: Dict[str, str] = {}
    functions: Dict[str, Function] = {}

    def walk(node):
        if isinstance(node, dict):
            node_id = node.get("id")
            if node_id:
                by_id.setdefault(node_id, []).append(node)
            if node.get("kind") == "TypedefDecl" and node.get("name") and _main_location(node.get("loc", {})):
                owned = None
                for child in node.get("inner", []):
                    if child.get("kind") == "ElaboratedType" and child.get("ownedTagDecl"):
                        owned = child["ownedTagDecl"]["id"]
                typedefs[node["name"]] = owned
            if (
                node.get("kind") == "FunctionDecl"
                and node.get("name", "").startswith("lancedb_")
                and node.get("storageClass") != "static"
            ):
                functions[node["name"]] = Function(
                    parse_type(node["type"]["qualType"]),
                    tuple(
                        child.get("name", "")
                        for child in node.get("inner", [])
                        if child.get("kind") == "ParmVarDecl"
                    ),
                )
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(ast)

    structs: Dict[str, List[Tuple[str, CType]]] = {}
    enums: Dict[str, List[int]] = {}
    struct_locations: Dict[str, int] = {}
    for name, tag_id in typedefs.items():
        if not tag_id:
            continue
        candidates = by_id.get(tag_id, [])
        for node in candidates:
            if node.get("kind") == "RecordDecl" and node.get("completeDefinition"):
                structs[name] = [
                    (field["name"], parse_type(field["type"]["qualType"]))
                    for field in node.get("inner", [])
                    if field.get("kind") == "FieldDecl"
                ]
                struct_locations[name] = int(node.get("loc", {}).get("line", 0))
                break
        for node in candidates:
            if node.get("kind") == "EnumDecl":
                constants = [c for c in node.get("inner", []) if c.get("kind") == "EnumConstantDecl"]
                if constants:
                    enums[name] = [v for v in (_constant_value(c) for c in constants) if v is not None]
                    break
    return Declarations(functions, structs, enums, struct_locations)


@dataclasses.dataclass(frozen=True)
class Allow:
    allow_const: bool = False
    allow_enum_int: bool = False


# Deliberate differences between the C declaration and the Rust FFI signature.
# Keys are function parameter names, or (struct, field) pairs.
FUNCTION_EXCEPTIONS: Dict[Tuple[str, str], Allow] = {
    # The C enum documents the accepted values, while Rust takes a checked
    # c_int and rejects values outside the enumerators at the FFI boundary.
    ("lancedb_expr_binary", "op"): Allow(allow_enum_int=True),
    ("lancedb_table_create_vector_index", "index_type"): Allow(allow_enum_int=True),
    ("lancedb_table_create_scalar_index", "index_type"): Allow(allow_enum_int=True),
    ("lancedb_table_optimize", "optimize_type"): Allow(allow_enum_int=True),
    ("lancedb_vector_query_distance_type", "distance_type"): Allow(allow_enum_int=True),
    # Rust reads the Arrow C structures without mutating them; the public
    # header leaves the caller's pointers mutable.
    ("lancedb_json_matches", "arrays"): Allow(allow_const=True),
    ("lancedb_json_matches", "schema"): Allow(allow_const=True),
}
FIELD_EXCEPTIONS: Dict[Tuple[str, str], Allow] = {
    # The header exposes a read-only view of result data. Rust owns the
    # pointers privately so it can free them with Drop.
    ("LanceDBIndexInfo", "name"): Allow(allow_const=True),
    ("LanceDBIndexInfo", "columns"): Allow(allow_const=True),
    ("LanceDBIndexInfo", "num_columns"): Allow(allow_const=True),
    ("LanceDBIndexInfo", "index_type"): Allow(allow_const=True),
    ("LanceDBIndexStatistics", "index_type"): Allow(allow_const=True),
    ("LanceDBIndexStatistics", "distance_type"): Allow(allow_const=True),
    # Rust stores the raw integer and validates it before converting to an
    # enum, avoiding undefined behavior for an out-of-range C value.
    ("LanceDBVectorIndexConfig", "distance_type"): Allow(allow_enum_int=True),
}


def compare_type(path: str, header: CType, generated: CType, enum_names: Set[str], allow: Allow = Allow()) -> List[str]:
    errors: List[str] = []
    if allow.allow_const:
        header = strip_const(header)
        generated = strip_const(generated)
    if header.kind != generated.kind:
        return [f"{path}: type kind differs: {header.kind} vs {generated.kind}"]
    if header.kind == "base":
        if header.name != generated.name:
            c_int = "int:{}".format(TARGET_SIZES.get("int", 4) * 8)
            if allow.allow_enum_int and (
                (header.name in enum_names and generated.name in ("int", "int32", c_int))
                or (generated.name in enum_names and header.name in ("int", "int32", c_int))
            ):
                return []
            return [f"{path}: base type differs: {header.name!r} vs {generated.name!r}"]
        if header.const != generated.const:
            return [f"{path}: const differs on base type"]
        return []
    if header.kind == "pointer":
        if header.const != generated.const:
            errors.append(f"{path}: pointer const differs: {header.const} vs {generated.const}")
        errors.extend(compare_type(path, header.inner, generated.inner, enum_names, allow))
        return errors
    # function
    errors.extend(compare_type(path + " return", header.ret, generated.ret, enum_names, allow))
    if len(header.params) != len(generated.params):
        return [f"{path}: parameter count differs: {len(header.params)} vs {len(generated.params)}"]
    for index, (left, right) in enumerate(zip(header.params, generated.params)):
        errors.extend(compare_type(f"{path} argument {index + 1}", left, right, enum_names, allow))
    if header.variadic != generated.variadic:
        errors.append(f"{path}: variadic differs")
    return errors


def strip_const(value: CType) -> CType:
    if value.kind == "base":
        return dataclasses.replace(value, const=False)
    if value.kind == "pointer":
        return dataclasses.replace(value, const=False, inner=strip_const(value.inner))
    return dataclasses.replace(
        value,
        ret=strip_const(value.ret),
        params=tuple(strip_const(p) for p in value.params),
    )


def rust_exports(repo: Path) -> Set[str]:
    pattern = re.compile(
        r'#\[no_mangle\]\s*(?:#\[[^\]]*\]\s*)*(?:pub\s+)?(?:unsafe\s+)?'
        r'extern\s+"C"\s+fn\s+([A-Za-z_]\w*)'
    )
    names: Set[str] = set()
    for path in sorted((repo / "src").rglob("*.rs")):
        names.update(pattern.findall(path.read_text()))
    return names


def target_sizes(clang: str) -> Dict[str, int]:
    output = subprocess.run(
        [clang, "-dM", "-E", "-x", "c", os.devnull],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    macros = {}
    for line in output.splitlines():
        match = re.match(r"#define __SIZEOF_([A-Z_]+)__ (\d+)", line)
        if match:
            macros[match.group(1)] = int(match.group(2))
    names = {
        "INT": "int",
        "SHORT": "short",
        "LONG": "long",
        "LONG_LONG": "long long",
        "POINTER": "pointer",
        "FLOAT": "float",
        "DOUBLE": "double",
        "LONG_DOUBLE": "long double",
    }
    return {names[key]: value for key, value in macros.items() if key in names}


def record_layouts(clang: str, header: Path, include_dir: Optional[Path]) -> Dict[int, Tuple[int, int, Dict[str, int]]]:
    command = [clang, "-std=c17", "-Xclang", "-fdump-record-layouts-complete", "-c", str(header), "-o", os.devnull]
    if include_dir is not None:
        command[2:2] = ["-I", str(include_dir)]
    output = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    layouts: Dict[int, Tuple[int, int, Dict[str, int]]] = {}
    current_line: Optional[int] = None
    current_fields: Dict[str, int] = {}
    current_size: Optional[int] = None
    current_align: Optional[int] = None
    wanted = header.resolve()
    for line in output.splitlines():
        match = re.search(r"struct \(unnamed at (.*):(\d+):\d+\)", line)
        if match:
            if current_line is not None and current_size is not None and current_align is not None:
                layouts[current_line] = (current_size, current_align, current_fields)
            current_fields = {}
            current_size = None
            current_align = None
            current_line = None
            if Path(match.group(1)).resolve() == wanted:
                current_line = int(match.group(2))
            continue
        if current_line is None:
            continue
        match = re.match(r"\s*(\d+) \|\s+(.+?)\s*$", line)
        if match and not match.group(2).startswith("["):
            # Field lines end in the declared field name; the type and any
            # nested record label precede it.
            current_fields[match.group(2).split()[-1]] = int(match.group(1))
            continue
        match = re.search(r"\[sizeof=(\d+), align=(\d+)\]", line)
        if match:
            current_size = int(match.group(1))
            current_align = int(match.group(2))
    if current_line is not None and current_size is not None and current_align is not None:
        layouts[current_line] = (current_size, current_align, current_fields)
    return layouts


def compare_declarations(
    header: Declarations,
    generated: Declarations,
    rust_exports: Set[str],
    header_layouts: Dict[int, Tuple[int, int, Dict[str, int]]],
    generated_layouts: Dict[int, Tuple[int, int, Dict[str, int]]],
) -> List[str]:
    errors: List[str] = []
    enum_names = set(header.enums)

    header_functions = set(header.functions)
    generated_functions = set(generated.functions)
    for name in sorted(header_functions - generated_functions):
        errors.append(f"function {name}: declared in the public header but not generated from Rust")
    for name in sorted(generated_functions - header_functions):
        errors.append(f"function {name}: generated from Rust but not declared in the public header")
    for name in sorted(rust_exports - generated_functions):
        errors.append(f"function {name}: exported by Rust source but missing from cbindgen output")
    for name in sorted(generated_functions - rust_exports):
        errors.append(f"function {name}: generated by cbindgen but not an extern \"C\" Rust export")
    for name in sorted(header_functions & generated_functions):
        left_function = header.functions[name]
        right_function = generated.functions[name]
        if left_function.type.kind != "function" or right_function.type.kind != "function":
            errors.append(f"function {name}: declaration is not a function type")
            continue
        # Return type has no exception; arguments are checked individually below.
        errors.extend(
            compare_type(
                f"function {name} return",
                left_function.type.ret,
                right_function.type.ret,
                enum_names,
            )
        )
        left_params = left_function.type.params
        right_params = right_function.type.params
        if len(left_params) != len(right_params):
            errors.append(f"function {name}: parameter count differs: {len(left_params)} vs {len(right_params)}")
            continue
        for index, (left, right) in enumerate(zip(left_params, right_params)):
            parameter_name = (
                left_function.param_names[index] if index < len(left_function.param_names) else ""
            )
            errors.extend(
                compare_type(
                    f"function {name} argument {index + 1}",
                    left,
                    right,
                    enum_names,
                    FUNCTION_EXCEPTIONS.get((name, parameter_name), Allow()),
                )
            )

    for name in sorted(set(header.structs) | set(generated.structs)):
        left_fields = header.structs.get(name)
        right_fields = generated.structs.get(name)
        if left_fields is None:
            errors.append(f"struct {name}: generated from Rust but not defined in the public header")
            continue
        if right_fields is None:
            errors.append(f"struct {name}: defined in the public header but not generated from Rust")
            continue
        if [field[0] for field in left_fields] != [field[0] for field in right_fields]:
            errors.append(
                f"struct {name}: field order differs: "
                f"{[field[0] for field in left_fields]} vs {[field[0] for field in right_fields]}"
            )
            continue
        for (field_name, left_type), (_, right_type) in zip(left_fields, right_fields):
            errors.extend(
                compare_type(
                    f"struct {name} field {field_name}",
                    left_type,
                    right_type,
                    enum_names,
                    FIELD_EXCEPTIONS.get((name, field_name), Allow()),
                )
            )
        left_line = header.struct_locations[name]
        right_line = generated.struct_locations[name]
        left_layout = header_layouts.get(left_line)
        right_layout = generated_layouts.get(right_line)
        if left_layout is None or right_layout is None:
            errors.append(f"struct {name}: record layout was not emitted by clang")
            continue
        left_size, left_align, left_offsets = left_layout
        right_size, right_align, right_offsets = right_layout
        if (left_size, left_align) != (right_size, right_align):
            errors.append(
                f"struct {name}: size/alignment differs: "
                f"{left_size}/{left_align} vs {right_size}/{right_align}"
            )
        for field_name, _ in left_fields:
            if left_offsets.get(field_name) != right_offsets.get(field_name):
                errors.append(
                    f"struct {name} field {field_name}: offset differs: "
                    f"{left_offsets.get(field_name)} vs {right_offsets.get(field_name)}"
                )

    for name in sorted(header.enums):
        left_values = header.enums[name]
        right_values = generated.enums.get(name)
        if right_values is None:
            # These are header-only bit masks, not Rust enums. Their values are
            # covered by the C++ optimize option tests rather than cbindgen.
            if name in ("LanceDBCompactionField", "LanceDBPruneField"):
                continue
            errors.append(f"enum {name}: defined in the public header but not generated from Rust")
            continue
        if sorted(left_values) != sorted(right_values):
            errors.append(f"enum {name}: values differ: {sorted(left_values)} vs {sorted(right_values)}")
    for name in sorted(set(generated.enums) - set(header.enums)):
        errors.append(f"enum {name}: generated from Rust but not defined in the public header")

    return errors


def run_checked(command: Sequence[str], **kwargs) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        raise RuntimeError(
            "command failed: {}\nstdout:\n{}\nstderr:\n{}".format(
                " ".join(command), result.stdout, result.stderr
            )
        )
    return result


def clang_ast(clang: str, header: Path, include_dir: Optional[Path], output: Path) -> None:
    command = [clang, "-std=c17", "-Xclang", "-ast-dump=json", "-fsyntax-only", str(header)]
    if include_dir is not None:
        command[2:2] = ["-I", str(include_dir)]
    output.write_text(run_checked(command).stdout)


def generate_bindings(cbindgen: str, repo: Path, output: Path, header_text: str) -> None:
    opaque = re.findall(r"typedef\s+struct\s+(\w+)\s+\1\s*;", header_text)
    # Enums used only through checked raw c_int parameters do not otherwise
    # appear in cbindgen's reachable declarations; ask for every public header
    # enum explicitly so their values are compared too.
    enums = re.findall(r"typedef\s+enum\s*\{.*?\}\s*(\w+)\s*;", header_text, re.DOTALL)
    config = """language = "C"
include_guard = "LANCEDB_CBINDGEN_H"
style = "type"
documentation = false

[export]
item_types = ["enums", "structs", "opaque", "functions"]
exclude = [{exclude}]
include = [{include}]
""".format(
        exclude=", ".join('"{}"'.format(name) for name in opaque),
        include=", ".join('"{}"'.format(name) for name in enums),
    )
    with tempfile.TemporaryDirectory(prefix="lancedb-cbindgen-") as directory:
        config_path = Path(directory) / "cbindgen.toml"
        body_path = Path(directory) / "bindings.h"
        config_path.write_text(config)
        run_checked(
            [
                cbindgen,
                "--config",
                str(config_path),
                "--crate",
                "lancedb-c",
                "--output",
                str(body_path),
            ],
            cwd=repo,
        )
        body = body_path.read_text()
        prefix = "\n".join("typedef struct {0} {0};".format(name) for name in opaque)
        # Keep the generated system includes, then declare the opaque handles
        # that the public header deliberately keeps incomplete.
        split = body.find("\n\n")
        if split == -1:
            raise RuntimeError("cbindgen output has no include block")
        output.write_text(body[: split + 2] + prefix + "\n\n" + body[split + 2 :])


def check_header(
    header_path: Path,
    include_dir: Path,
    clang: str,
    generated: Declarations,
    rust_export_names: Set[str],
    generated_layouts: Dict[int, Tuple[int, int, Dict[str, int]]],
    work: Path,
    name: str,
) -> List[str]:
    header_ast_path = work / (name + ".json")
    clang_ast(clang, header_path, include_dir, header_ast_path)
    header = collect_declarations(header_ast_path)
    header_layouts = record_layouts(clang, header_path, include_dir)
    return compare_declarations(
        header, generated, rust_export_names, header_layouts, generated_layouts
    )


def prepare_generated(
    repo: Path, cbindgen: str, clang: str, work: Path
) -> Tuple[Declarations, Dict[int, Tuple[int, int, Dict[str, int]]]]:
    header_path = repo / "include" / "lancedb.h"
    generated_path = work / "generated.h"
    generate_bindings(cbindgen, repo, generated_path, header_path.read_text())
    generated_ast_path = work / "generated.json"
    clang_ast(clang, generated_path, None, generated_ast_path)
    generated = collect_declarations(generated_ast_path)
    generated_layouts = record_layouts(clang, generated_path, None)
    return generated, generated_layouts


def mutate_signature(text: str) -> str:
    return re.sub(
        r"(LanceDBConnectBuilder\*\s*lancedb_connect\s*\(\s*const char\s*\*)(\s+uri\s*\)\s*;)",
        r"\1*\2",
        text,
        count=1,
    )


def mutate_layout(text: str) -> str:
    return re.sub(r"(\n\s*)size_t\s+(p25|p50)\s*;", r"\1uint32_t \2;", text, count=2)


def run_self_test(
    repo: Path,
    header_path: Path,
    clang: str,
    generated: Declarations,
    rust_export_names: Set[str],
    generated_layouts: Dict[int, Tuple[int, int, Dict[str, int]]],
    work: Path,
) -> List[str]:
    original = header_path.read_text()
    fixtures = [
        ("signature", mutate_signature, "function lancedb_connect argument 1"),
        ("layout", mutate_layout, "struct LanceDBFragmentSummaryStats"),
    ]
    failures: List[str] = []
    for name, mutate, expected in fixtures:
        mutated = mutate(original)
        if mutated == original:
            failures.append("self-test {} fixture did not match the current header".format(name))
            continue
        fixture_path = work / (name + ".h")
        fixture_path.write_text(mutated)
        errors = check_header(
            fixture_path,
            repo / "include",
            clang,
            generated,
            rust_export_names,
            generated_layouts,
            work,
            name,
        )
        if not any(expected in error for error in errors):
            failures.append(
                "self-test {} fixture was not detected as expected; errors: {}".format(name, errors)
            )
    return failures


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root containing include/lancedb.h and the Rust crate",
    )
    parser.add_argument(
        "--header",
        type=Path,
        help="public header to check (default: <repo>/include/lancedb.h)",
    )
    parser.add_argument("--cbindgen", default=shutil.which("cbindgen"))
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="also check that temporary signature and layout drift fixtures are detected",
    )
    args = parser.parse_args(argv)

    repo = args.repo.resolve()
    header_path = (args.header or repo / "include" / "lancedb.h").resolve()
    if not args.cbindgen:
        print(
            "error: cbindgen not found; install pinned cbindgen {} with:\n"
            "  cargo install cbindgen --version ={} --locked".format(
                CBINDGEN_VERSION, CBINDGEN_VERSION
            ),
            file=sys.stderr,
        )
        return 2
    version = run_checked([args.cbindgen, "--version"]).stdout.strip()
    if version != "cbindgen " + CBINDGEN_VERSION:
        print(
            "error: expected cbindgen {} but found {}; install the pinned version with:\n"
            "  cargo install cbindgen --version ={} --locked".format(
                CBINDGEN_VERSION, version, CBINDGEN_VERSION
            ),
            file=sys.stderr,
        )
        return 2
    if not args.clang:
        print("error: clang not found; the declaration check uses clang's AST", file=sys.stderr)
        return 2

    global TARGET_SIZES
    TARGET_SIZES = target_sizes(args.clang)
    with tempfile.TemporaryDirectory(prefix="lancedb-c-abi-") as directory:
        work = Path(directory)
        generated, generated_layouts = prepare_generated(repo, args.cbindgen, args.clang, work)
        rust_export_names = rust_exports(repo)
        errors = check_header(
            header_path,
            repo / "include",
            args.clang,
            generated,
            rust_export_names,
            generated_layouts,
            work,
            "header",
        )
        if errors:
            print("C ABI declaration mismatch:", file=sys.stderr)
            for error in errors:
                print("  " + error, file=sys.stderr)
            return 1
        if args.self_test:
            failures = run_self_test(
                repo,
                header_path,
                args.clang,
                generated,
                rust_export_names,
                generated_layouts,
                work,
            )
            if failures:
                print("C ABI declaration check self-test failed:", file=sys.stderr)
                for failure in failures:
                    print("  " + failure, file=sys.stderr)
                return 1
            print("C ABI drift fixtures detected: signature and layout")

    print(
        "C ABI declarations match: {} extern functions, {} public structs, {} shared enums".format(
            len(generated.functions), len(generated.structs), len(generated.enums)
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
