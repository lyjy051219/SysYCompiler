#!/usr/bin/env python3
"""
后端用性能评分脚本。

使用决赛评分标准。
把当前前中端+clang o3 当作第一名。计算前中端加后端的分数。同时对比总运行时间。
后端部分pass可在脚本中开关

常用命令：

  ./performance_score.py --suite 26
      只跑 tests/testcases26/riscv-performance。

  ./performance_score.py --suite non26
      只跑 testcases26 以外的性能用例。

  ./performance_score.py --suite all
      合并前两类用例，默认方案。

  ./performance_score.py --suite 26 --baseline llc-ir
      只用 LLVM codegen 处理我方前中端 IR，并与我方链接同一个 runtime 对象。

  ./performance_score.py --suite 26 --disable-backend-pass cleanup-cfg \
      --work-root perf_score_work_cleanup_off
      关闭指定后端可选 pass 后评分。

  ./performance_score.py --single-case /path/to/case.sy --warmups 1 --runs 1 \
      --allow-under-standard
      快速冒烟；不要当正式成绩。

评分规则：

  1. 我方和 baseline 在所有预热、计时轮次都 AC，case 才计分。
  2. baseline 默认把我方前中端 IR 与性能 runtime IR 经 llvm-link 合并，再交给
     clang-18 -O3 -ffp-contract=off 生成 RISC-V 程序；llc-ir 跳过 clang 的
     IR 优化，只运行 LLVM codegen，并使用 static relocation 匹配我方后端。
     两种 baseline 的分数不可直接相减。
  3. 默认每个可执行文件预热 5 轮，再计时 5 轮。
  4. 单项分 = baseline_平均时间 / 我方平均时间 * 100，默认不封顶。
  5. 总分 = 所有单项分的几何平均。

总运行时间只统计计时轮次，不包含预热、编译、汇编、链接。
"""

import argparse
import csv
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from difflib import unified_diff
from pathlib import Path


class Color:
    GREEN = "\033[92m"
    RED = "\033[91m"
    YELLOW = "\033[93m"
    RESET = "\033[0m"


def discover_project() -> Path:
    cwd = Path.cwd().resolve()
    if (cwd / "CMakeLists.txt").exists() and (cwd / "runtime").exists():
        return cwd
    if (cwd / "Compiler" / "CMakeLists.txt").exists():
        return cwd / "Compiler"
    script_dir = Path(__file__).resolve().parent
    if (script_dir / "CMakeLists.txt").exists():
        return script_dir
    return Path("/home/lyjy/Compiler/Compiler").resolve()


PROJECT = discover_project()
WORKSPACE = PROJECT.parent
TESTS_ROOT = WORKSPACE.parent / "tests"
BUILD_DIR = PROJECT / "build-riscv"
COMPILER = BUILD_DIR / "compiler"
MIDDLE_BUILD_DIR = PROJECT / "build-middle"
MIDDLE_COMPILER = MIDDLE_BUILD_DIR / "compiler_middle_end"
OFFICIAL_RUNTIME_LL = PROJECT / "runtime" / "runtime.ll"
PERFORMANCE_RUNTIME_DIR = WORKSPACE / "性能替代版"
RUNTIME_C = PERFORMANCE_RUNTIME_DIR / "sylib.c"
RUNTIME_LL = PERFORMANCE_RUNTIME_DIR / "runtime.ll"
LLVM_RISCV_TRIPLE = "riscv64-linux-gnu"
LLVM_RISCV_CPU = "generic-rv64"
LLVM_RISCV_FEATURES = "+m,+a,+f,+d,+c,+zicsr,+zifencei,+relax"
LLVM_RISCV_ABI = "lp64d"
DEFAULT_LLC_RELOCATION_MODEL = "static"

# Only used when reading IR produced by the pre-29450d7 middle-end, which
# embedded runtime/runtime.ll before the application module.  Current IR
# already carries its own external declarations and never uses this fallback.
LEGACY_RUNTIME_DECLARATIONS = """\
declare i32 @getint()
declare i32 @getch()
declare float @getfloat()
declare i32 @getarray(i32*)
declare i32 @getfarray(float*)
declare void @putint(i32)
declare void @putch(i32)
declare void @putfloat(float)
declare void @putarray(i32, i32*)
declare void @putfarray(i32, float*)
declare void @putf(i8*, ...)
declare void @_sysy_starttime(i32)
declare void @_sysy_stoptime(i32)
declare void @llvm.memcpy.p0.p0.i32(ptr, ptr, i32, i1)
declare void @llvm.memmove.p0.p0.i32(ptr, ptr, i32, i1)
declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)

"""
RUNTIME_API_SYMBOLS = (
    "getint",
    "getch",
    "getfloat",
    "getarray",
    "getfarray",
    "putint",
    "putch",
    "putfloat",
    "putarray",
    "putfarray",
    "putf",
    "_sysy_starttime",
    "_sysy_stoptime",
)
RUNTIME_DEFINITION_RE = re.compile(
    r"(?m)^[ \t]*define\b[^\n@]*@(?:" +
    "|".join(re.escape(symbol) for symbol in RUNTIME_API_SYMBOLS) +
    r")\s*\("
)
IR_FUNCTION_NAME_RE = re.compile(r"@([-A-Za-z$._0-9]+)\s*\(")

ASSEMBLY_STAT_KEYS = (
    "instructions",
    "moves",
    "loads",
    "stores",
    "branches_jumps",
    "multiplies",
    "divides_remainders",
)
ASSEMBLY_LOAD_OPS = {
    "lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "flw", "fld",
    "lr.w", "lr.d",
}
ASSEMBLY_STORE_OPS = {
    "sb", "sh", "sw", "sd", "fsw", "fsd", "sc.w", "sc.d",
}
ASSEMBLY_CONTROL_OPS = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "beqz", "bnez",
    "ble", "bgt", "bleu", "bgtu", "blez", "bgez", "bltz", "bgtz",
    "j", "jal", "jalr", "jr", "ret", "call", "tail",
}
ASSEMBLY_MULTIPLY_OPS = {"mul", "mulh", "mulhsu", "mulhu", "mulw"}
ASSEMBLY_DIVIDE_REMAINDER_OPS = {
    "div", "divu", "divw", "divuw", "rem", "remu", "remw", "remuw",
}


@dataclass
class CommandResult:
    returncode: int
    stdout: bytes
    stderr: bytes
    elapsed: float


@dataclass
class RunResult:
    status: str
    runtime: float
    detail: str = ""


@dataclass
class CaseResult:
    case: Path
    baseline_avg: float
    ours_avg: float
    raw_score: float
    score: float
    baseline_runs: list
    ours_runs: list


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Score RISC-V backend performance against a selectable reference baseline.",
        add_help=False,
    )
    parser._optionals.title = "options"
    parser.add_argument("-h", "--help", action="help", help="show this help message and exit")
    parser.add_argument(
        "--suite",
        choices=["26", "non26", "all"],
        default="all",
        help="26: testcases26 only; non26: performance cases outside testcases26; all: both.",
    )
    parser.add_argument("--warmups", type=int, default=5, help="warmup runs per case")
    parser.add_argument("--runs", type=int, default=5, help="timed runs per case")
    parser.add_argument("--allow-under-standard", action="store_true",
                        help="allow fewer warmup/timed runs for smoke tests")
    parser.add_argument("--skip-build", action="store_true", help="do not rebuild the compiler")
    parser.add_argument("--max-cases", type=int, help="use only the first N sorted cases")
    parser.add_argument("--single-case", action="append", default=[],
                        help="run only the given case; repeat or use comma-separated values")
    parser.add_argument("--work-root", type=Path, default=PROJECT / "perf_score_work")
    parser.add_argument("--compile-timeout", type=int, default=600)
    parser.add_argument("--assemble-timeout", type=int, default=60)
    parser.add_argument("--link-timeout", type=int, default=60)
    parser.add_argument("--run-timeout", type=int, default=300)
    parser.add_argument("--build-timeout", type=int, default=900)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++-18"))
    parser.add_argument("--baseline", choices=["clang-ir", "llc-ir", "gcc-o3"], default="clang-ir",
                        help="baseline executable source; default: clang-ir")
    parser.add_argument("--clang", default=os.environ.get("CLANG", "clang-18"),
                        help="clang used for the clang-ir baseline")
    parser.add_argument("--llvm-link", default=os.environ.get("LLVM_LINK", "llvm-link-18"),
                        help="llvm-link used to combine application and runtime IR")
    parser.add_argument("--clang-opt-level", choices=["O0", "O1", "O2", "O3"],
                        default="O3", help="clang optimization level for clang-ir baseline")
    parser.add_argument("--clang-extra", action="append", default=[],
                        help="extra argument passed to clang baseline; repeatable")
    parser.add_argument("--clang-fp-contract", default="off",
                        choices=["off", "on", "fast"],
                        help="clang baseline -ffp-contract mode; default: off")
    parser.add_argument("--llc", default=os.environ.get("LLC", "llc-18"),
                        help="llc used for the llc-ir baseline")
    parser.add_argument("--llc-opt-level", choices=["O0", "O1", "O2", "O3"],
                        default="O3", help="llc codegen optimization level; default: O3")
    parser.add_argument("--llc-relocation-model", choices=["static", "pic"],
                        default=DEFAULT_LLC_RELOCATION_MODEL,
                        help="llc relocation model; default: static (matches our backend)")
    parser.add_argument("--llc-extra", action="append", default=[],
                        help="extra argument passed to llc baseline; repeatable")
    parser.add_argument("--llc-fp-contract", default="off",
                        choices=["off", "on", "fast"],
                        help="llc baseline -fp-contract mode; default: off")
    parser.add_argument("--gcc-extra", action="append", default=[],
                        help="extra argument passed to the gcc baseline; repeatable")
    parser.add_argument("--gcc-fp-contract", default="off",
                        choices=["off", "on", "fast"],
                        help="gcc baseline -ffp-contract mode; default: off")
    parser.add_argument("--disable-backend-pass", action="append", default=[],
                        help="disable optional backend pass for our compiler; repeat or comma-separate")
    parser.add_argument("--time-stat", choices=["mean", "median", "trimmed-mean"],
                        default="mean",
                        help="per-case runtime statistic; official default: mean")
    parser.add_argument(
        "--cpu-affinity",
        default=os.environ.get("PERF_CPU_AFFINITY"),
        metavar="CPU-LIST",
        help="pin each qemu timing run with taskset (for example: 0 or 2-3)",
    )
    parser.add_argument("--clock-jump-retries", type=int, default=5,
                        help="rerun an attempt when SysY wall-clock TOTAL is impossible")
    parser.add_argument("--no-clock-jump-filter", action="store_true",
                        help="use SysY TOTAL exactly as printed, without local clock-jump protection")
    parser.add_argument("--cap-score", action="store_true",
                        help="cap each case score at 100")
    parser.add_argument("--no-score-cap", action="store_true",
                        help="compatibility option; scores are uncapped by default")
    parser.add_argument("--keep-work", action="store_true", help="keep the previous work directory")
    return parser.parse_args()


def fail(message: str) -> None:
    print(f"{Color.RED}error:{Color.RESET} {message}", file=sys.stderr)
    raise SystemExit(2)


def run_command(args, cwd=WORKSPACE, timeout=60, stdin_path=None, env=None) -> CommandResult:
    stdin_file = stdin_path.open("rb") if stdin_path and stdin_path.exists() else None
    start = time.perf_counter()
    try:
        ret = subprocess.run(
            args,
            cwd=cwd,
            stdin=stdin_file,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            env=env,
        )
        elapsed = time.perf_counter() - start
        return CommandResult(ret.returncode, ret.stdout, ret.stderr, elapsed)
    finally:
        if stdin_file:
            stdin_file.close()


def probe_llc(args: argparse.Namespace) -> None:
    resolved = shutil.which(args.llc)
    if resolved is None:
        fail(f"llc executable not found: {args.llc}")
    try:
        ret = run_command([resolved, "--version"], timeout=10)
    except subprocess.TimeoutExpired:
        fail(f"timed out while querying llc version: {resolved}")
    version_text = (ret.stdout + ret.stderr).decode("utf-8", errors="replace")
    if ret.returncode != 0:
        fail(f"cannot query llc version: {args.llc}")
    if not re.search(r"(?m)^\s*riscv64\s+-", version_text):
        fail(f"llc has no registered RISC-V 64 target: {resolved}")
    version_lines = [line.strip() for line in version_text.splitlines() if line.strip()]
    args.llc_resolved = str(Path(resolved).resolve())
    args.llc_version = version_lines[0] if version_lines else "unknown"


def probe_llvm_link(args: argparse.Namespace) -> None:
    resolved = shutil.which(args.llvm_link)
    if resolved is None:
        fail(f"llvm-link executable not found: {args.llvm_link}")
    try:
        ret = run_command([resolved, "--version"], timeout=10)
    except subprocess.TimeoutExpired:
        fail(f"timed out while querying llvm-link version: {resolved}")
    if ret.returncode != 0:
        fail(f"cannot query llvm-link version: {args.llvm_link}")
    version_text = (ret.stdout + ret.stderr).decode("utf-8", errors="replace")
    version_lines = [line.strip() for line in version_text.splitlines() if line.strip()]
    args.llvm_link_resolved = str(Path(resolved).resolve())
    args.llvm_link_version = version_lines[0] if version_lines else "unknown"


def run_setup_command(args, stage: str, timeout: int) -> None:
    ret = subprocess.run(
        args,
        cwd=WORKSPACE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )
    if ret.returncode == 0:
        return
    print(f"{Color.RED}[{stage} failed]{Color.RESET} {' '.join(args)}")
    if ret.stdout:
        print(ret.stdout[-3000:])
    if ret.stderr:
        print(ret.stderr[-3000:])
    raise RuntimeError(f"{stage} failed with exit code {ret.returncode}")


def detect_riscv_prefix() -> str:
    env_prefix = os.environ.get("RISCV_PREFIX")
    if env_prefix and shutil.which(env_prefix + "gcc"):
        return env_prefix
    candidates = [
        "riscv64-linux-gnu-",
        "riscv64-unknown-linux-gnu-",
        "/home/r1c0/riscv/bin/riscv64-unknown-linux-gnu-",
    ]
    for prefix in candidates:
        if shutil.which(prefix + "gcc"):
            return prefix
    fail("RISC-V toolchain not found; install riscv64-linux-gnu-gcc or set RISCV_PREFIX")


RISCV_PREFIX = detect_riscv_prefix()
RISCV_GCC = RISCV_PREFIX + "gcc"
RISCV_AS = RISCV_PREFIX + "as"
QEMU = shutil.which("qemu-riscv64")
if not QEMU:
    fail("qemu-riscv64 not found")
TASKSET = shutil.which("taskset")


def build_cmake_target(build_dir: Path, target: str, configure_options: list,
                       args: argparse.Namespace, label: str) -> None:
    print(f"{Color.YELLOW}[SETUP]{Color.RESET} building {label} ...")
    configure_cmd = [
        "cmake",
        "-B",
        str(build_dir),
        "-S",
        str(PROJECT),
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_CXX_COMPILER={args.cxx}",
    ] + configure_options
    build_cmd = [
        "cmake",
        "--build",
        str(build_dir),
        "--config",
        "Debug",
        "--target",
        target,
        "-j",
        "4",
    ]
    run_setup_command(configure_cmd, f"configure {label}", args.build_timeout)
    run_setup_command(build_cmd, f"build {label}", args.build_timeout)


def build_compiler(args: argparse.Namespace) -> None:
    build_cmake_target(
        BUILD_DIR,
        "compiler",
        [
            "-DBUILD_FRONTEND_ONLY=OFF",
            "-DBUILD_MIDDLE_END=OFF",
            "-DBUILD_FRONTEND_RELEASE=OFF",
        ],
        args,
        "riscv backend compiler",
    )
    if args.baseline in ("clang-ir", "llc-ir"):
        build_cmake_target(
            MIDDLE_BUILD_DIR,
            "compiler_middle_end",
            [
                "-DBUILD_FRONTEND_ONLY=OFF",
                "-DBUILD_MIDDLE_END=ON",
                "-DBUILD_FRONTEND_RELEASE=OFF",
            ],
            args,
            "middle-end IR compiler",
        )


def is_bad_case(path: Path) -> bool:
    return "rv_bad_tests" in path.parts


def is_performance_case(path: Path) -> bool:
    return "performance" in str(path).lower()


def resolve_case_path(path_text: str) -> Path:
    raw = Path(path_text)
    if raw.is_absolute():
        return raw.resolve()
    for base in [PROJECT, WORKSPACE, WORKSPACE.parent, TESTS_ROOT]:
        candidate = (base / raw).resolve()
        if candidate.exists():
            return candidate
    return (PROJECT / raw).resolve()


def disabled_backend_passes(args: argparse.Namespace) -> list:
    disabled = []
    for item in args.disable_backend_pass:
        disabled.extend(part.strip() for part in item.split(",") if part.strip())
    return disabled


def effective_disabled_backend_passes(args: argparse.Namespace) -> list:
    disabled = [
        item.strip()
        for item in os.environ.get("BACKEND_DISABLE_PASSES", "").split(",")
        if item.strip()
    ]
    disabled.extend(disabled_backend_passes(args))
    return disabled


def collect_cases(args: argparse.Namespace) -> list:
    single_cases = []
    env_single = os.environ.get("SINGLE_CASES", "")
    for item in args.single_case:
        single_cases.extend(part for part in item.split(",") if part)
    single_cases.extend(part for part in env_single.split(",") if part)
    if single_cases:
        cases = [resolve_case_path(item) for item in single_cases]
    elif args.suite == "26":
        root = TESTS_ROOT / "testcases26" / "riscv-performance"
        cases = sorted(path.resolve() for path in root.glob("**/*.sy"))
    elif args.suite == "non26":
        cases = sorted(
            path.resolve()
            for path in TESTS_ROOT.glob("**/*.sy")
            if "testcases26" not in path.parts and is_performance_case(path)
        )
    else:
        cases = sorted(
            path.resolve()
            for path in TESTS_ROOT.glob("**/*.sy")
            if is_performance_case(path)
        )

    filtered = []
    for case in cases:
        if case.suffix != ".sy" or is_bad_case(case):
            continue
        filtered.append(case)
    if args.max_cases is not None:
        filtered = filtered[: args.max_cases]
    return filtered


def safe_case_id(case: Path) -> str:
    try:
        rel = case.relative_to(TESTS_ROOT)
    except ValueError:
        rel = case
    digest = hashlib.sha1(str(rel).encode("utf-8")).hexdigest()[:10]
    stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(rel.with_suffix("")))
    return f"{digest}_{stem[-80:]}"


def write_gcc_header(work_root: Path) -> Path:
    header = work_root / "sysy_score_runtime.h"
    header.write_text(
        "\n".join(
            [
                "#pragma once",
                "int getint(void);",
                "int getch(void);",
                "float getfloat(void);",
                "int getarray(int a[]);",
                "int getfarray(float a[]);",
                "void putint(int a);",
                "void putch(int a);",
                "void putarray(int n, int a[]);",
                "void putfloat(float a);",
                "void putfarray(int n, float a[]);",
                "void putf(char a[], ...);",
                "void _sysy_starttime(int lineno);",
                "void _sysy_stoptime(int lineno);",
                "#define starttime() _sysy_starttime(__LINE__)",
                "#define stoptime() _sysy_stoptime(__LINE__)",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return header


def strip_line_comment(line: str) -> str:
    pos = line.find("//")
    return line if pos < 0 else line[:pos]


def make_gcc_compatible_source(case: Path, case_dir: Path) -> Path:
    """生成给 gcc 用的临时兼容源码。

    SysY 的文件作用域 const int 可用于数组维度；C 不接受这种写法。
    这里把简单 const int 改成 enum 常量，不修改原测试文件。
    """

    source = case.read_text(encoding="utf-8")
    rewritten = []
    brace_depth = 0
    const_decl = re.compile(r"^(\s*)const\s+int\s+(.+);\s*$")
    item = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*$")

    for line in source.splitlines(keepends=True):
        newline = "\n" if line.endswith("\n") else ""
        body = line[:-1] if newline else line
        code = strip_line_comment(body)
        match = const_decl.match(code)
        if brace_depth == 0 and match:
            indent, rest = match.groups()
            entries = []
            convertible = True
            for part in rest.split(","):
                part_match = item.match(part)
                if not part_match:
                    convertible = False
                    break
                name, value = part_match.groups()
                entries.append(f"{name} = {value.strip()}")
            if convertible:
                rewritten.append(f"{indent}enum {{ {', '.join(entries)} }};{newline}")
            else:
                rewritten.append(line)
        else:
            rewritten.append(line)

        brace_depth += code.count("{") - code.count("}")
        if brace_depth < 0:
            brace_depth = 0

    compat = case_dir / "gcc_compat.sy"
    compat.write_text("".join(rewritten), encoding="utf-8")
    return compat


def prepare_work(args: argparse.Namespace) -> Path:
    work_root = args.work_root.resolve()
    if work_root.exists() and not args.keep_work:
        shutil.rmtree(work_root)
    (work_root / "cases").mkdir(parents=True, exist_ok=True)
    (work_root / "reports").mkdir(parents=True, exist_ok=True)
    return work_root


def compile_runtime_obj(work_root: Path, args: argparse.Namespace) -> Path:
    obj = work_root / "sylib.o"
    cmd = [RISCV_GCC, "-O2", "-w", "-c", str(RUNTIME_C), "-o", str(obj)]
    ret = run_command(cmd, timeout=args.compile_timeout)
    if ret.returncode != 0:
        raise RuntimeError(ret.stderr.decode("utf-8", errors="replace")[-3000:])
    return obj


def compile_ours(case: Path, case_dir: Path, runtime_obj: Path, args: argparse.Namespace) -> Path:
    asm_file = case_dir / "ours.s"
    obj_file = case_dir / "ours.o"
    exe_file = case_dir / "ours.exe"

    compile_env = os.environ.copy()
    disabled_passes = disabled_backend_passes(args)
    if disabled_passes:
        existing = compile_env.get("BACKEND_DISABLE_PASSES", "")
        requested = ",".join(disabled_passes)
        compile_env["BACKEND_DISABLE_PASSES"] = (
            f"{existing},{requested}" if existing else requested
        )

    ret = run_command([str(COMPILER), str(case), "-S", "-o", str(asm_file)],
                      timeout=args.compile_timeout, env=compile_env)
    if ret.returncode != 0:
        raise RuntimeError("compiler error\n" + ret.stderr.decode("utf-8", errors="replace")[-3000:])

    ret = run_command([RISCV_AS, str(asm_file), "-o", str(obj_file)], timeout=args.assemble_timeout)
    if ret.returncode != 0:
        raise RuntimeError("assembler error\n" + ret.stderr.decode("utf-8", errors="replace")[-3000:])

    ret = run_command([RISCV_GCC, "-static", str(obj_file), str(runtime_obj), "-o", str(exe_file)],
                      timeout=args.link_timeout)
    if ret.returncode != 0:
        raise RuntimeError("linker error\n" + ret.stderr.decode("utf-8", errors="replace")[-3000:])
    return exe_file


def compile_gcc_o3(case: Path, case_dir: Path, header: Path, args: argparse.Namespace) -> Path:
    exe_file = case_dir / "gcc_o3.exe"
    compat_source = make_gcc_compatible_source(case, case_dir)
    cmd = [
        RISCV_GCC,
        "-O3",
        "-static",
        "-w",
        "-include",
        str(header),
        "-x",
        "c",
        str(compat_source),
        str(RUNTIME_C),
        "-o",
        str(exe_file),
    ]
    baseline_extra = [f"-ffp-contract={args.gcc_fp_contract}"] + args.gcc_extra
    cmd[1:1] = baseline_extra
    ret = run_command(cmd, timeout=args.compile_timeout)
    if ret.returncode != 0:
        raise RuntimeError("gcc -O3 compile/link error\n" + ret.stderr.decode("utf-8", errors="replace")[-3000:])
    return exe_file


def append_missing_legacy_runtime_declarations(text: str) -> str:
    missing = []
    for declaration in LEGACY_RUNTIME_DECLARATIONS.splitlines():
        if not declaration:
            continue
        match = IR_FUNCTION_NAME_RE.search(declaration)
        if match is None:
            raise RuntimeError(f"invalid legacy runtime declaration: {declaration}")
        symbol = re.escape(match.group(1))
        if re.search(
            rf"(?m)^[ \t]*(?:declare|define)\b[^\n@]*@{symbol}[ \t]*\(",
            text,
        ):
            continue
        missing.append(declaration)

    if not missing:
        return text
    return text.rstrip() + "\n\n" + "\n".join(missing) + "\n"


def normalize_middle_ir(text: str) -> str:
    """Return an application module for both current and legacy emitters.

    Current middle-end IR is already a standalone application module with
    external runtime declarations.  Older builds prepended the exact contents
    of runtime/runtime.ll; strip only that known prefix and restore declarations
    needed to parse the remaining application IR.  Refuse unknown embedded
    runtimes instead of guessing where their module ends.
    """

    if OFFICIAL_RUNTIME_LL.exists():
        official_runtime = OFFICIAL_RUNTIME_LL.read_text(encoding="utf-8")
        if official_runtime and text.startswith(official_runtime):
            app_ir = text[len(official_runtime):].lstrip("\r\n")
            return append_missing_legacy_runtime_declarations(app_ir)

    if RUNTIME_DEFINITION_RE.search(text):
        raise RuntimeError(
            "middle-end IR embeds an unknown runtime implementation; expected "
            "external declarations or an exact runtime/runtime.ll prefix"
        )
    return text


def sanitize_ir_for_llvm_riscv(input_file: Path, output_file: Path,
                               *, normalize_middle_end: bool = False) -> None:
    text = input_file.read_text(encoding="utf-8")
    if normalize_middle_end:
        text = normalize_middle_ir(text)
    if not text.strip():
        raise RuntimeError(f"empty LLVM IR module: {input_file}")

    # Runtime IR can be generated on an x86 host.  The command line selects the
    # RISC-V target, so discard module and function attributes that would pin
    # LLVM to the host target while preserving every other module entity.
    text = re.sub(
        r'(?m)^[ \t]*target[ \t]+(?:datalayout|triple)[ \t]*=.*(?:\r?\n|\Z)',
        "",
        text,
    )
    text = re.sub(
        r'[ \t]*"target-(?:cpu|features)"[ \t]*=[ \t]*"[^"]*"',
        "",
        text,
    )
    # LLVM rejects an attribute group with no attributes.  Keep an empty CPU
    # marker only when removing host attributes emptied the whole group; an
    # empty value lets the command-line TargetMachine select the actual CPU.
    text = re.sub(
        r'(?m)^([ \t]*attributes[ \t]+#[0-9]+[ \t]*=[ \t]*\{)'
        r'[ \t]*(\}[ \t]*(?:;[^\r\n]*)?(?:\r?\n|\Z))',
        r'\1 "target-cpu"="" \2',
        text,
    )
    if not text.strip():
        raise RuntimeError(f"LLVM IR module is empty after target cleanup: {input_file}")
    output_file.write_text(text, encoding="utf-8")


def prepare_llvm_ir_baseline(case: Path, case_dir: Path,
                             args: argparse.Namespace) -> Path:
    ir_file = case_dir / "baseline.ll"
    llvm_ir_file = case_dir / "baseline_sanitized.ll"

    ret = run_command([str(MIDDLE_COMPILER), str(case), "-S", "-o", str(ir_file)],
                      timeout=args.compile_timeout)
    if ret.returncode != 0:
        raise RuntimeError(
            "middle-end IR compile error\n" +
            ret.stderr.decode("utf-8", errors="replace")[-3000:]
        )

    sanitize_ir_for_llvm_riscv(
        ir_file, llvm_ir_file, normalize_middle_end=True
    )
    return llvm_ir_file


def link_performance_runtime(app_ir_file: Path, case_dir: Path,
                             args: argparse.Namespace) -> Path:
    runtime_ir_file = case_dir / "performance_runtime_sanitized.ll"
    linked_ir_file = case_dir / "baseline_linked.ll"
    sanitize_ir_for_llvm_riscv(RUNTIME_LL, runtime_ir_file)

    # Keep runtime first to stay close to the historical runtime+application
    # module order used by clang-ir performance reports.
    ret = run_command(
        [
            args.llvm_link_resolved,
            "-S",
            str(runtime_ir_file),
            str(app_ir_file),
            "-o",
            str(linked_ir_file),
        ],
        timeout=args.compile_timeout,
    )
    if ret.returncode != 0:
        raise RuntimeError(
            "llvm-link application/runtime IR error\n" +
            ret.stderr.decode("utf-8", errors="replace")[-3000:]
        )
    return linked_ir_file


def compile_clang_ir_baseline(case: Path, case_dir: Path,
                              args: argparse.Namespace) -> Path:
    app_ir_file = prepare_llvm_ir_baseline(case, case_dir, args)
    llvm_ir_file = link_performance_runtime(app_ir_file, case_dir, args)
    asm_file = case_dir / "baseline_clang.s"
    obj_file = case_dir / "baseline_clang.o"
    exe_file = case_dir / "baseline_clang.exe"

    opt_flag = "-" + args.clang_opt_level
    clang_cmd = [
        args.clang,
        f"--target={LLVM_RISCV_TRIPLE}",
        "-march=rv64gc",
        f"-mabi={LLVM_RISCV_ABI}",
        opt_flag,
        "-fno-addrsig",
        f"-ffp-contract={args.clang_fp_contract}",
        "-S",
        str(llvm_ir_file),
        "-o",
        str(asm_file),
    ] + args.clang_extra
    ret = run_command(clang_cmd, timeout=args.compile_timeout)
    if ret.returncode != 0:
        raise RuntimeError(
            "clang IR baseline compile error\n" +
            ret.stderr.decode("utf-8", errors="replace")[-3000:]
        )

    ret = run_command([RISCV_AS, str(asm_file), "-o", str(obj_file)],
                      timeout=args.assemble_timeout)
    if ret.returncode != 0:
        raise RuntimeError(
            "clang IR baseline assembler error\n" +
            ret.stderr.decode("utf-8", errors="replace")[-3000:]
        )

    ret = run_command([RISCV_GCC, "-static", str(obj_file), "-o", str(exe_file)],
                      timeout=args.link_timeout)
    if ret.returncode != 0:
        raise RuntimeError(
            "clang IR baseline linker error\n" +
            ret.stderr.decode("utf-8", errors="replace")[-3000:]
        )
    return exe_file


def llc_codegen_flags(args: argparse.Namespace) -> list:
    return [
        f"-mtriple={LLVM_RISCV_TRIPLE}",
        "-march=riscv64",
        f"-mcpu={LLVM_RISCV_CPU}",
        f"-mattr={LLVM_RISCV_FEATURES}",
        f"-target-abi={LLVM_RISCV_ABI}",
        f"-relocation-model={args.llc_relocation_model}",
        f"-O={args.llc_opt_level[1:]}",
        f"-fp-contract={args.llc_fp_contract}",
    ]


def compile_llc_ir_baseline(case: Path, case_dir: Path, runtime_obj: Path,
                            args: argparse.Namespace) -> Path:
    llvm_ir_file = prepare_llvm_ir_baseline(case, case_dir, args)
    asm_file = case_dir / "baseline_llc.s"
    obj_file = case_dir / "baseline_llc.o"
    exe_file = case_dir / "baseline_llc.exe"

    llc_cmd = [
        args.llc_resolved,
        *llc_codegen_flags(args),
        *args.llc_extra,
        "-filetype=asm",
        str(llvm_ir_file),
        "-o",
        str(asm_file),
    ]
    ret = run_command(llc_cmd, timeout=args.compile_timeout)
    if ret.returncode != 0:
        raise RuntimeError(
            "llc IR baseline compile error\n" +
            ret.stderr.decode("utf-8", errors="replace")[-3000:]
        )

    ret = run_command([RISCV_AS, str(asm_file), "-o", str(obj_file)],
                      timeout=args.assemble_timeout)
    if ret.returncode != 0:
        raise RuntimeError(
            "llc IR baseline assembler error\n" +
            ret.stderr.decode("utf-8", errors="replace")[-3000:]
        )

    ret = run_command([RISCV_GCC, "-static", str(obj_file), str(runtime_obj),
                       "-o", str(exe_file)],
                      timeout=args.link_timeout)
    if ret.returncode != 0:
        raise RuntimeError(
            "llc IR baseline linker error\n" +
            ret.stderr.decode("utf-8", errors="replace")[-3000:]
        )
    return exe_file


def compile_baseline(case: Path, case_dir: Path, header: Path | None,
                     runtime_obj: Path,
                     args: argparse.Namespace) -> Path:
    if args.baseline == "gcc-o3":
        if header is None:
            raise RuntimeError("gcc baseline header was not prepared")
        return compile_gcc_o3(case, case_dir, header, args)
    if args.baseline == "clang-ir":
        return compile_clang_ir_baseline(case, case_dir, args)
    if args.baseline == "llc-ir":
        return compile_llc_ir_baseline(case, case_dir, runtime_obj, args)
    raise RuntimeError(f"unsupported baseline: {args.baseline}")


def parse_sysy_total_seconds(stderr_bytes: bytes) -> float | None:
    text = stderr_bytes.decode("utf-8", errors="replace")
    match = re.search(r"TOTAL:\s*(-?\d+)H-(-?\d+)M-(-?\d+)S-(-?\d+)us", text)
    if not match:
        return None
    hours, minutes, seconds, microseconds = map(int, match.groups())
    return hours * 3600 + minutes * 60 + seconds + microseconds / 1_000_000


def clock_jump_detected(sysy_runtime: float, elapsed: float) -> bool:
    if sysy_runtime < 0:
        return True
    # SysY TOTAL measures a section inside the process, so it cannot exceed
    # the monotonic whole-process elapsed time by a large margin.
    tolerance = max(1.0, elapsed * 0.20)
    return sysy_runtime > elapsed + tolerance


def expected_output(expected_file: Path) -> str:
    if not expected_file.exists():
        raise RuntimeError(f"expected output not found: {expected_file}")
    return expected_file.read_text(encoding="utf-8")


def run_and_compare(exe: Path, input_file: Path, expected_text: str,
                    args: argparse.Namespace, label: str, case: Path) -> RunResult:
    command = [QEMU, str(exe)]
    if args.cpu_affinity:
        command = [TASKSET, "-c", args.cpu_affinity, *command]
    try:
        ret = run_command(command, timeout=args.run_timeout, stdin_path=input_file)
    except subprocess.TimeoutExpired:
        return RunResult("TLE", float(args.run_timeout), f"{label} qemu timeout after {args.run_timeout}s")

    sysy_runtime = parse_sysy_total_seconds(ret.stderr)
    if sysy_runtime is None:
        runtime = ret.elapsed
    elif not args.no_clock_jump_filter and clock_jump_detected(sysy_runtime, ret.elapsed):
        detail = (
            f"{label} clock jump: SysY TOTAL={sysy_runtime:.6f}s, "
            f"monotonic elapsed={ret.elapsed:.6f}s"
        )
        return RunResult("CLOCK_JUMP", sysy_runtime, detail)
    else:
        runtime = sysy_runtime if sysy_runtime >= 0 else ret.elapsed

    actual = ret.stdout.decode("utf-8", errors="replace")
    if actual and not actual.endswith("\n"):
        actual += "\n"
    actual += str(ret.returncode) + "\n"

    diff = list(unified_diff(actual.splitlines(), expected_text.splitlines(),
                             fromfile=f"{label}-actual", tofile="expected", lineterm=""))
    if diff:
        detail = "\n".join(diff[:120])
        return RunResult("WA", runtime, f"{label} wrong answer: {case}\n{detail}")
    return RunResult("AC", max(runtime, 1e-9))


def run_and_compare_stable(exe: Path, input_file: Path, expected_text: str,
                           args: argparse.Namespace, label: str, case: Path) -> RunResult:
    max_attempts = 1 if args.no_clock_jump_filter else args.clock_jump_retries + 1
    for attempt in range(1, max_attempts + 1):
        result = run_and_compare(exe, input_file, expected_text, args, label, case)
        if result.status != "CLOCK_JUMP":
            return result
        if attempt < max_attempts:
            print(
                f"{Color.YELLOW}[CLOCK]{Color.RESET} {format_case(case)} "
                f"{result.detail}; retry {attempt}/{max_attempts - 1}"
            )
    return result


def baseline_label(args: argparse.Namespace) -> str:
    return args.baseline


def measure_pair(ours_exe: Path, baseline_exe: Path, case: Path,
                 args: argparse.Namespace) -> tuple:
    input_file = case.with_suffix(".in")
    expected_text = expected_output(case.with_suffix(".out"))
    label_base = baseline_label(args)

    for i in range(args.warmups):
        for label, exe in [(label_base, baseline_exe), ("ours", ours_exe)]:
            result = run_and_compare_stable(exe, input_file, expected_text, args, label, case)
            if result.status != "AC":
                raise RuntimeError(f"{label} warmup run {i + 1} failed: {result.detail}")

    ours_runs = []
    baseline_runs = []
    for i in range(args.runs):
        order = [(label_base, baseline_exe), ("ours", ours_exe)]
        if i % 2 == 1:
            order.reverse()
        for label, exe in order:
            result = run_and_compare_stable(exe, input_file, expected_text, args, label, case)
            if result.status != "AC":
                raise RuntimeError(f"{label} timed run {i + 1} failed: {result.detail}")
            if label == label_base:
                baseline_runs.append(result.runtime)
            else:
                ours_runs.append(result.runtime)

    return baseline_runs, ours_runs


def runtime_statistic(values: list, mode: str) -> float:
    # 正式评分默认使用算术平均；median/trimmed-mean 只用于本机稳定性诊断。
    if mode == "mean" or len(values) <= 2:
        return sum(values) / len(values)
    ordered = sorted(values)
    if mode == "median":
        mid = len(ordered) // 2
        if len(ordered) % 2:
            return ordered[mid]
        return (ordered[mid - 1] + ordered[mid]) / 2.0
    if len(ordered) >= 5:
        ordered = ordered[1:-1]
    return sum(ordered) / len(ordered)


def geometric_mean(values: list) -> float:
    if not values:
        return 0.0
    return math.exp(sum(math.log(max(value, 1e-300)) for value in values) / len(values))


def format_case(case: Path, max_len=42) -> str:
    try:
        text = str(case.relative_to(TESTS_ROOT))
    except ValueError:
        text = str(case)
    if len(text) > max_len:
        text = "..." + text[-(max_len - 3):]
    return text.ljust(max_len)


def file_sha256(path: Path) -> str | None:
    if not path.exists():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def baseline_assembly_path(case_dir: Path, args: argparse.Namespace) -> Path | None:
    if args.baseline == "clang-ir":
        return case_dir / "baseline_clang.s"
    if args.baseline == "llc-ir":
        return case_dir / "baseline_llc.s"
    # The gcc-o3 path currently compiles and links in one driver invocation.
    return None


def assembly_opcode_counts(path: Path | None) -> dict:
    counts = {key: None for key in ASSEMBLY_STAT_KEYS}
    if path is None or not path.exists():
        return counts

    counts = {key: 0 for key in ASSEMBLY_STAT_KEYS}
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        if ":" in line:
            possible_label, remainder = line.split(":", 1)
            if re.fullmatch(r"[-A-Za-z$._0-9]+", possible_label.strip()):
                line = remainder.strip()
        if not line or line.startswith("."):
            continue
        opcode = line.split(None, 1)[0].lower()
        if not re.fullmatch(r"[a-z][a-z0-9_.]*", opcode):
            continue

        counts["instructions"] += 1
        if opcode == "mv" or opcode.startswith("fmv."):
            counts["moves"] += 1
        if opcode in ASSEMBLY_LOAD_OPS:
            counts["loads"] += 1
        if opcode in ASSEMBLY_STORE_OPS:
            counts["stores"] += 1
        if opcode in ASSEMBLY_CONTROL_OPS:
            counts["branches_jumps"] += 1
        if opcode in ASSEMBLY_MULTIPLY_OPS or opcode.startswith(
            ("fmul.", "fmadd.", "fmsub.", "fnmadd.", "fnmsub.")
        ):
            counts["multiplies"] += 1
        if opcode in ASSEMBLY_DIVIDE_REMAINDER_OPS or opcode.startswith("fdiv."):
            counts["divides_remainders"] += 1
    return counts


def write_hotspot_report(work_root: Path, results: list,
                         args: argparse.Namespace) -> Path:
    hotspot_path = work_root / "reports" / "hotspots.csv"
    relative_rank = {
        id(result): rank
        for rank, result in enumerate(
            sorted(results, key=lambda item: 100.0 - item.raw_score, reverse=True), 1
        )
    }
    absolute_rank = {
        id(result): rank
        for rank, result in enumerate(
            sorted(results, key=lambda item: item.ours_avg - item.baseline_avg,
                   reverse=True), 1
        )
    }
    ordered_results = sorted(
        results, key=lambda item: item.ours_avg - item.baseline_avg, reverse=True
    )

    stat_columns = []
    for key in ASSEMBLY_STAT_KEYS:
        stat_columns.extend([f"ours_{key}", f"baseline_{key}"])

    with hotspot_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "case",
            "relative_gap_rank",
            "relative_gap_percent",
            "absolute_gap_rank",
            "absolute_gap_seconds",
            "baseline_time",
            "ours_time",
            "raw_score",
            "case_dir",
            "ours_asm",
            "baseline_asm",
            *stat_columns,
        ])
        for result in ordered_results:
            try:
                case_name = str(result.case.relative_to(TESTS_ROOT))
            except ValueError:
                case_name = str(result.case)
            case_dir = work_root / "cases" / safe_case_id(result.case)
            ours_asm = case_dir / "ours.s"
            baseline_asm = baseline_assembly_path(case_dir, args)
            ours_stats = assembly_opcode_counts(ours_asm)
            baseline_stats = assembly_opcode_counts(baseline_asm)
            stat_values = []
            for key in ASSEMBLY_STAT_KEYS:
                stat_values.extend([ours_stats[key], baseline_stats[key]])
            writer.writerow([
                case_name,
                relative_rank[id(result)],
                f"{100.0 - result.raw_score:.6f}",
                absolute_rank[id(result)],
                f"{result.ours_avg - result.baseline_avg:.9f}",
                f"{result.baseline_avg:.9f}",
                f"{result.ours_avg:.9f}",
                f"{result.raw_score:.6f}",
                str(case_dir),
                str(ours_asm),
                str(baseline_asm) if baseline_asm is not None else "",
                *stat_values,
            ])
    return hotspot_path


def write_reports(work_root: Path, results: list, failures: list, args: argparse.Namespace) -> None:
    reports = work_root / "reports"
    csv_path = reports / "score_cases.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "case",
            "time_stat",
            "baseline_time",
            "ours_time",
            "baseline_mean",
            "ours_mean",
            "raw_score",
            "score",
            "baseline_runs",
            "ours_runs",
        ])
        for result in results:
            try:
                case_name = str(result.case.relative_to(TESTS_ROOT))
            except ValueError:
                case_name = str(result.case)
            baseline_mean = sum(result.baseline_runs) / len(result.baseline_runs)
            ours_mean = sum(result.ours_runs) / len(result.ours_runs)
            writer.writerow([
                case_name,
                args.time_stat,
                f"{result.baseline_avg:.9f}",
                f"{result.ours_avg:.9f}",
                f"{baseline_mean:.9f}",
                f"{ours_mean:.9f}",
                f"{result.raw_score:.6f}",
                f"{result.score:.6f}",
                " ".join(f"{x:.9f}" for x in result.baseline_runs),
                " ".join(f"{x:.9f}" for x in result.ours_runs),
            ])

    hotspot_path = write_hotspot_report(work_root, results, args)

    baseline_total_runtime = sum(sum(result.baseline_runs) for result in results)
    ours_total_runtime = sum(sum(result.ours_runs) for result in results)
    timed_run_count = sum(len(result.ours_runs) for result in results)
    summary = {
        "suite": args.suite,
        "baseline": args.baseline,
        "warmups": args.warmups,
        "runs": args.runs,
        "under_standard": args.warmups < 5 or args.runs < 2,
        "time_stat": args.time_stat,
        "cpu_affinity": args.cpu_affinity,
        "case_count": len(results),
        "failure_count": len(failures),
        "score": geometric_mean([result.score for result in results]) if not failures else None,
        "raw_score": geometric_mean([result.raw_score for result in results]) if not failures else None,
        "score_cap": args.cap_score,
        "compiler": str(COMPILER),
        "compiler_sha256": file_sha256(COMPILER),
        "gpr_virtual_copy_coalescing":
            os.environ.get("BACKEND_DISABLE_GPR_VREG_COALESCING") is None,
        "effective_disabled_backend_passes": effective_disabled_backend_passes(args),
        "clang": args.clang if args.baseline == "clang-ir" else None,
        "clang_opt_level": args.clang_opt_level if args.baseline == "clang-ir" else None,
        "clang_fp_contract": args.clang_fp_contract if args.baseline == "clang-ir" else None,
        "clang_extra": args.clang_extra if args.baseline == "clang-ir" else None,
        "clang_relocation_model": "driver-default" if args.baseline == "clang-ir" else None,
        "llvm_link": args.llvm_link if args.baseline == "clang-ir" else None,
        "llvm_link_resolved": args.llvm_link_resolved if args.baseline == "clang-ir" else None,
        "llvm_link_version": args.llvm_link_version if args.baseline == "clang-ir" else None,
        "llc": args.llc if args.baseline == "llc-ir" else None,
        "llc_resolved": args.llc_resolved if args.baseline == "llc-ir" else None,
        "llc_version": args.llc_version if args.baseline == "llc-ir" else None,
        "llc_opt_level": args.llc_opt_level if args.baseline == "llc-ir" else None,
        "llc_fp_contract": args.llc_fp_contract if args.baseline == "llc-ir" else None,
        "llc_extra": args.llc_extra if args.baseline == "llc-ir" else None,
        "llc_triple": LLVM_RISCV_TRIPLE if args.baseline == "llc-ir" else None,
        "llc_cpu": LLVM_RISCV_CPU if args.baseline == "llc-ir" else None,
        "llc_features": LLVM_RISCV_FEATURES if args.baseline == "llc-ir" else None,
        "llc_abi": LLVM_RISCV_ABI if args.baseline == "llc-ir" else None,
        "llc_relocation_model": args.llc_relocation_model if args.baseline == "llc-ir" else None,
        "llc_shared_runtime_object": args.baseline == "llc-ir",
        "gcc_fp_contract": args.gcc_fp_contract if args.baseline == "gcc-o3" else None,
        "gcc_extra": args.gcc_extra if args.baseline == "gcc-o3" else None,
        "baseline_total_timed_runtime": baseline_total_runtime,
        "ours_total_timed_runtime": ours_total_runtime,
        "baseline_average_timed_runtime": baseline_total_runtime / timed_run_count if timed_run_count else None,
        "ours_average_timed_runtime": ours_total_runtime / timed_run_count if timed_run_count else None,
        "timed_run_count": timed_run_count,
        "csv": str(csv_path),
        "hotspots_csv": str(hotspot_path),
    }
    (reports / "score_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

    failure_path = reports / "failure_report.txt"
    if failures:
        failure_path.write_text("\n\n".join(failures) + "\n", encoding="utf-8")
    else:
        failure_path.write_text("No failures.\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    if not args.allow_under_standard and (args.warmups < 5 or args.runs < 2):
        fail("official scoring requires --warmups >= 5 and --runs >= 2; add --allow-under-standard for smoke tests")
    if args.warmups < 0 or args.runs <= 0:
        fail("warmups must be >= 0 and runs must be > 0")
    if args.cpu_affinity and TASKSET is None:
        fail("--cpu-affinity requires taskset")
    if not TESTS_ROOT.exists():
        fail(f"tests directory not found: {TESTS_ROOT}")
    if not RUNTIME_C.exists():
        fail(f"runtime library not found: {RUNTIME_C}")
    if args.baseline == "clang-ir" and not RUNTIME_LL.exists():
        fail(f"runtime IR not found: {RUNTIME_LL}")
    if args.baseline == "clang-ir" and shutil.which(args.clang) is None:
        fail(f"clang executable not found: {args.clang}")
    if args.baseline == "clang-ir":
        probe_llvm_link(args)
    if args.baseline == "llc-ir":
        probe_llc(args)

    if not args.skip_build:
        build_compiler(args)
    if not COMPILER.exists():
        fail(f"compiler not found: {COMPILER}")
    if args.baseline in ("clang-ir", "llc-ir") and not MIDDLE_COMPILER.exists():
        fail(f"middle-end compiler not found: {MIDDLE_COMPILER}")

    cases = collect_cases(args)
    if not cases:
        fail("no performance cases selected")

    work_root = prepare_work(args)
    gcc_header = write_gcc_header(work_root) if args.baseline == "gcc-o3" else None
    runtime_obj = compile_runtime_obj(work_root, args)

    print(
        f"{Color.YELLOW}[SETUP]{Color.RESET} "
        f"suite={args.suite}, cases={len(cases)}, warmups={args.warmups}, "
        f"timed-runs={args.runs}, time-stat={args.time_stat}"
    )
    print(
        f"{Color.YELLOW}[SETUP]{Color.RESET} runtime: "
        f"{PERFORMANCE_RUNTIME_DIR} (CLOCK_MONOTONIC)"
    )
    if args.cpu_affinity:
        print(
            f"{Color.YELLOW}[SETUP]{Color.RESET} qemu cpu affinity: "
            f"{args.cpu_affinity}"
        )
    if args.baseline == "gcc-o3":
        baseline_flags = [f"-ffp-contract={args.gcc_fp_contract}"] + args.gcc_extra
        print(f"{Color.YELLOW}[SETUP]{Color.RESET} baseline: {RISCV_GCC} -O3 {' '.join(baseline_flags)}")
    elif args.baseline == "clang-ir":
        clang_flags = [
            "-" + args.clang_opt_level,
            f"-ffp-contract={args.clang_fp_contract}",
        ] + args.clang_extra
        print(
            f"{Color.YELLOW}[SETUP]{Color.RESET} baseline: "
            f"{MIDDLE_COMPILER} app IR + runtime IR -> "
            f"{args.llvm_link_resolved} -> {args.clang} {' '.join(clang_flags)}"
        )
    else:
        llc_flags = llc_codegen_flags(args) + args.llc_extra
        print(
            f"{Color.YELLOW}[SETUP]{Color.RESET} baseline: "
            f"{MIDDLE_COMPILER} app IR -> {args.llc_resolved} {' '.join(llc_flags)}; "
            f"shared runtime object={runtime_obj}"
        )
    disabled_passes = disabled_backend_passes(args)
    if disabled_passes:
        print(f"{Color.YELLOW}[SETUP]{Color.RESET} disabled backend passes: {','.join(disabled_passes)}")
    print(f"{Color.YELLOW}[SETUP]{Color.RESET} work root: {work_root}")

    results = []
    failures = []
    for index, case in enumerate(cases, 1):
        case_dir = work_root / "cases" / safe_case_id(case)
        case_dir.mkdir(parents=True, exist_ok=True)
        try:
            ours_exe = compile_ours(case, case_dir, runtime_obj, args)
            base_exe = compile_baseline(case, case_dir, gcc_header, runtime_obj, args)
            baseline_runs, ours_runs = measure_pair(ours_exe, base_exe, case, args)
            baseline_avg = runtime_statistic(baseline_runs, args.time_stat)
            ours_avg = runtime_statistic(ours_runs, args.time_stat)
            raw_score = (baseline_avg / max(ours_avg, 1e-9)) * 100.0
            score = min(100.0, raw_score) if args.cap_score else raw_score
            result = CaseResult(case, baseline_avg, ours_avg, raw_score, score,
                                baseline_runs, ours_runs)
            results.append(result)
            print(
                f"{Color.GREEN}[AC]{Color.RESET} {format_case(case)} "
                f"{baseline_label(args)}={baseline_avg:.6f}s "
                f"ours={ours_avg:.6f}s score={score:8.4f} "
                f"[{index}/{len(cases)}]"
            )
        except (RuntimeError, subprocess.TimeoutExpired) as exc:
            try:
                rel = str(case.relative_to(TESTS_ROOT))
            except ValueError:
                rel = str(case)
            detail = f"[FAIL] {rel}\n{str(exc).strip()}"
            failures.append(detail)
            print(f"{Color.RED}[FAIL]{Color.RESET} {format_case(case)} [{index}/{len(cases)}]")
            print(str(exc).strip()[-2000:])

    write_reports(work_root, results, failures, args)

    print()
    print("=" * 72)
    print(f"Selected cases: {len(cases)}")
    print(f"Accepted cases: {len(results)}")
    print(f"Failures:       {len(failures)}")
    if failures:
        print(f"{Color.RED}Non-AC cases exist; final score is not reported.{Color.RESET}")
        print(f"Failure report: {work_root / 'reports' / 'failure_report.txt'}")
        return 1

    raw_final = geometric_mean([result.raw_score for result in results])
    final = geometric_mean([result.score for result in results])
    baseline_total_runtime = sum(sum(result.baseline_runs) for result in results)
    ours_total_runtime = sum(sum(result.ours_runs) for result in results)
    timed_run_count = sum(len(result.ours_runs) for result in results)
    print(f"Raw geometric mean score: {raw_final:.6f}")
    if args.cap_score:
        print(f"Final performance score:  {final:.6f} (case scores capped at 100)")
    else:
        print(f"Final performance score:  {final:.6f}")
    print(f"{baseline_label(args)} timed total runtime: {baseline_total_runtime:.6f}s")
    print(f"ours timed total runtime:      {ours_total_runtime:.6f}s")
    if timed_run_count:
        print(f"{baseline_label(args)} average timed run:   {baseline_total_runtime / timed_run_count:.6f}s")
        print(f"ours average timed run:        {ours_total_runtime / timed_run_count:.6f}s")
    print(f"Case CSV: {work_root / 'reports' / 'score_cases.csv'}")
    print(f"Summary JSON: {work_root / 'reports' / 'score_summary.json'}")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
