#!/usr/bin/env python3
"""RISC-V 后端性能评分脚本。

常用命令：

  ./performance_score.py --suite 26
      只跑 tests/testcases26/riscv-performance。

  ./performance_score.py --suite non26
      只跑 testcases26 以外的性能用例。

  ./performance_score.py --suite all
      合并前两类用例，默认方案。

  ./performance_score.py --suite 26 --disable-backend-pass cleanup-cfg \
      --work-root perf_score_work_cleanup_off
      关闭指定后端可选 pass 后评分。

  ./performance_score.py --single-case /path/to/case.sy --warmups 1 --runs 1 \
      --allow-under-standard
      快速冒烟；不要当正式成绩。

评分规则：

  1. 我方和 gcc 基线在所有预热、计时轮次都 AC，case 才计分。
  2. 基线默认是 riscv64 gcc -O3 -ffp-contract=off，避免 FMA 改变严格浮点输出。
  3. 默认每个可执行文件预热 5 轮，再计时 5 轮。
  4. 单项分 = gcc_O3_平均时间 / 我方平均时间 * 100，默认不封顶。
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
RUNTIME_C = PROJECT / "runtime" / "sylib.c"


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
    gcc_avg: float
    ours_avg: float
    raw_score: float
    score: float
    gcc_runs: list
    ours_runs: list


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="按 gcc -O3 基线给 RISC-V 后端性能评分。",
        add_help=False,
    )
    parser._optionals.title = "选项"
    parser.add_argument("-h", "--help", action="help", help="显示帮助并退出")
    parser.add_argument(
        "--suite",
        choices=["26", "non26", "all"],
        default="all",
        help="26 只跑 26 年性能用例；non26 只跑非 26 年性能用例；all 合并两者。",
    )
    parser.add_argument("--warmups", type=int, default=5, help="每个 case 的预热轮数")
    parser.add_argument("--runs", type=int, default=5, help="每个 case 的计时轮数")
    parser.add_argument("--allow-under-standard", action="store_true",
                        help="允许低于正式轮数，用于冒烟测试")
    parser.add_argument("--skip-build", action="store_true", help="评分前不重新构建编译器")
    parser.add_argument("--max-cases", type=int, help="排序后只取前 N 个 case")
    parser.add_argument("--single-case", action="append", default=[],
                        help="只跑指定 case；可重复，也可逗号分隔")
    parser.add_argument("--work-root", type=Path, default=PROJECT / "perf_score_work")
    parser.add_argument("--compile-timeout", type=int, default=600)
    parser.add_argument("--assemble-timeout", type=int, default=60)
    parser.add_argument("--link-timeout", type=int, default=60)
    parser.add_argument("--run-timeout", type=int, default=300)
    parser.add_argument("--build-timeout", type=int, default=900)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++-18"))
    parser.add_argument("--gcc-extra", action="append", default=[],
                        help="额外传给 gcc 基线的参数；可重复")
    parser.add_argument("--gcc-fp-contract", default="off",
                        choices=["off", "on", "fast"],
                        help="gcc 基线的 -ffp-contract 模式；默认 off")
    parser.add_argument("--disable-backend-pass", action="append", default=[],
                        help="关闭我方后端可选 pass；可重复，也可逗号分隔")
    parser.add_argument("--cap-score", action="store_true",
                        help="把每个 case 的分数封顶为 100")
    parser.add_argument("--no-score-cap", action="store_true",
                        help="兼容旧参数；现在默认不封顶")
    parser.add_argument("--keep-work", action="store_true", help="保留旧工作目录内容")
    return parser.parse_args()


def fail(message: str) -> None:
    print(f"{Color.RED}错误:{Color.RESET} {message}", file=sys.stderr)
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
    fail("找不到 RISC-V 工具链；请安装 riscv64-linux-gnu-gcc 或设置 RISCV_PREFIX")


RISCV_PREFIX = detect_riscv_prefix()
RISCV_GCC = RISCV_PREFIX + "gcc"
RISCV_AS = RISCV_PREFIX + "as"
QEMU = shutil.which("qemu-riscv64")
if not QEMU:
    fail("找不到 qemu-riscv64")


def build_compiler(args: argparse.Namespace) -> None:
    print(f"{Color.YELLOW}[SETUP]{Color.RESET} 构建编译器 ...")
    configure_cmd = [
        "cmake",
        "-B",
        str(BUILD_DIR),
        "-S",
        str(PROJECT),
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DBUILD_FRONTEND_ONLY=OFF",
        "-DBUILD_MIDDLE_END=OFF",
        "-DBUILD_FRONTEND_RELEASE=OFF",
        f"-DCMAKE_CXX_COMPILER={args.cxx}",
    ]
    build_cmd = [
        "cmake",
        "--build",
        str(BUILD_DIR),
        "--config",
        "Debug",
        "--target",
        "compiler",
        "-j",
        "4",
    ]
    run_setup_command(configure_cmd, "configure", args.build_timeout)
    run_setup_command(build_cmd, "build", args.build_timeout)


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

    ret = run_command([str(COMPILER), "-S", "-o", str(asm_file), str(case)],
                      timeout=args.compile_timeout, env=compile_env)
    if ret.returncode != 0:
        raise RuntimeError("编译器错误\n" + ret.stderr.decode("utf-8", errors="replace")[-3000:])

    ret = run_command([RISCV_AS, str(asm_file), "-o", str(obj_file)], timeout=args.assemble_timeout)
    if ret.returncode != 0:
        raise RuntimeError("汇编错误\n" + ret.stderr.decode("utf-8", errors="replace")[-3000:])

    ret = run_command([RISCV_GCC, "-static", str(obj_file), str(runtime_obj), "-o", str(exe_file)],
                      timeout=args.link_timeout)
    if ret.returncode != 0:
        raise RuntimeError("链接错误\n" + ret.stderr.decode("utf-8", errors="replace")[-3000:])
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
        raise RuntimeError("gcc -O3 编译/链接错误\n" + ret.stderr.decode("utf-8", errors="replace")[-3000:])
    return exe_file


def parse_runtime_seconds(stderr_bytes: bytes, fallback: float) -> float:
    text = stderr_bytes.decode("utf-8", errors="replace")
    match = re.search(r"TOTAL:\s*(\d+)H-(\d+)M-(\d+)S-(\d+)us", text)
    if not match:
        return fallback
    hours, minutes, seconds, microseconds = map(int, match.groups())
    return hours * 3600 + minutes * 60 + seconds + microseconds / 1_000_000


def expected_output(expected_file: Path) -> str:
    if not expected_file.exists():
        raise RuntimeError(f"找不到期望输出: {expected_file}")
    return expected_file.read_text(encoding="utf-8")


def run_and_compare(exe: Path, input_file: Path, expected_text: str,
                    timeout: int, label: str, case: Path) -> RunResult:
    try:
        ret = run_command([QEMU, str(exe)], timeout=timeout, stdin_path=input_file)
    except subprocess.TimeoutExpired:
        return RunResult("TLE", float(timeout), f"{label} qemu 超时 {timeout}s")

    runtime = parse_runtime_seconds(ret.stderr, ret.elapsed)
    actual = ret.stdout.decode("utf-8", errors="replace")
    if actual and not actual.endswith("\n"):
        actual += "\n"
    actual += str(ret.returncode) + "\n"

    diff = list(unified_diff(actual.splitlines(), expected_text.splitlines(),
                             fromfile=f"{label}-actual", tofile="expected", lineterm=""))
    if diff:
        detail = "\n".join(diff[:120])
        return RunResult("WA", runtime, f"{label} 输出错误: {case}\n{detail}")
    return RunResult("AC", max(runtime, 1e-9))


def measure_pair(ours_exe: Path, gcc_exe: Path, case: Path, args: argparse.Namespace) -> tuple:
    input_file = case.with_suffix(".in")
    expected_text = expected_output(case.with_suffix(".out"))

    for i in range(args.warmups):
        for label, exe in [("gcc-o3", gcc_exe), ("ours", ours_exe)]:
            result = run_and_compare(exe, input_file, expected_text, args.run_timeout, label, case)
            if result.status != "AC":
                raise RuntimeError(f"{label} 预热第 {i + 1} 轮失败: {result.detail}")

    ours_runs = []
    gcc_runs = []
    for i in range(args.runs):
        order = [("gcc-o3", gcc_exe), ("ours", ours_exe)]
        if i % 2 == 1:
            order.reverse()
        for label, exe in order:
            result = run_and_compare(exe, input_file, expected_text, args.run_timeout, label, case)
            if result.status != "AC":
                raise RuntimeError(f"{label} 计时第 {i + 1} 轮失败: {result.detail}")
            if label == "gcc-o3":
                gcc_runs.append(result.runtime)
            else:
                ours_runs.append(result.runtime)

    return gcc_runs, ours_runs


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


def write_reports(work_root: Path, results: list, failures: list, args: argparse.Namespace) -> None:
    reports = work_root / "reports"
    csv_path = reports / "score_cases.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "case",
            "gcc_o3_avg",
            "ours_avg",
            "raw_score",
            "score",
            "gcc_o3_runs",
            "ours_runs",
        ])
        for result in results:
            try:
                case_name = str(result.case.relative_to(TESTS_ROOT))
            except ValueError:
                case_name = str(result.case)
            writer.writerow([
                case_name,
                f"{result.gcc_avg:.9f}",
                f"{result.ours_avg:.9f}",
                f"{result.raw_score:.6f}",
                f"{result.score:.6f}",
                " ".join(f"{x:.9f}" for x in result.gcc_runs),
                " ".join(f"{x:.9f}" for x in result.ours_runs),
            ])

    gcc_total_runtime = sum(sum(result.gcc_runs) for result in results)
    ours_total_runtime = sum(sum(result.ours_runs) for result in results)
    timed_run_count = sum(len(result.ours_runs) for result in results)
    summary = {
        "suite": args.suite,
        "warmups": args.warmups,
        "runs": args.runs,
        "case_count": len(results),
        "failure_count": len(failures),
        "score": geometric_mean([result.score for result in results]) if not failures else None,
        "raw_score": geometric_mean([result.raw_score for result in results]) if not failures else None,
        "score_cap": args.cap_score,
        "gcc_fp_contract": args.gcc_fp_contract,
        "gcc_extra": args.gcc_extra,
        "gcc_total_timed_runtime": gcc_total_runtime,
        "ours_total_timed_runtime": ours_total_runtime,
        "gcc_average_timed_runtime": gcc_total_runtime / timed_run_count if timed_run_count else None,
        "ours_average_timed_runtime": ours_total_runtime / timed_run_count if timed_run_count else None,
        "timed_run_count": timed_run_count,
        "csv": str(csv_path),
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
        fail("正式评分要求 --warmups >= 5 且 --runs >= 2；冒烟测试请加 --allow-under-standard")
    if args.warmups < 0 or args.runs <= 0:
        fail("warmups 必须 >= 0，runs 必须 > 0")
    if not TESTS_ROOT.exists():
        fail(f"找不到 tests 目录: {TESTS_ROOT}")
    if not RUNTIME_C.exists():
        fail(f"找不到运行库: {RUNTIME_C}")

    if not args.skip_build:
        build_compiler(args)
    if not COMPILER.exists():
        fail(f"找不到编译器: {COMPILER}")

    cases = collect_cases(args)
    if not cases:
        fail("没有选中性能用例")

    work_root = prepare_work(args)
    gcc_header = write_gcc_header(work_root)
    runtime_obj = compile_runtime_obj(work_root, args)

    print(f"{Color.YELLOW}[SETUP]{Color.RESET} 方案={args.suite}, 用例={len(cases)}, 预热={args.warmups}, 计时={args.runs}")
    baseline_flags = [f"-ffp-contract={args.gcc_fp_contract}"] + args.gcc_extra
    print(f"{Color.YELLOW}[SETUP]{Color.RESET} gcc 基线: {RISCV_GCC} -O3 {' '.join(baseline_flags)}")
    disabled_passes = disabled_backend_passes(args)
    if disabled_passes:
        print(f"{Color.YELLOW}[SETUP]{Color.RESET} 已关闭后端 pass: {','.join(disabled_passes)}")
    print(f"{Color.YELLOW}[SETUP]{Color.RESET} 工作目录: {work_root}")

    results = []
    failures = []
    for index, case in enumerate(cases, 1):
        case_dir = work_root / "cases" / safe_case_id(case)
        case_dir.mkdir(parents=True, exist_ok=True)
        try:
            ours_exe = compile_ours(case, case_dir, runtime_obj, args)
            gcc_exe = compile_gcc_o3(case, case_dir, gcc_header, args)
            gcc_runs, ours_runs = measure_pair(ours_exe, gcc_exe, case, args)
            gcc_avg = sum(gcc_runs) / len(gcc_runs)
            ours_avg = sum(ours_runs) / len(ours_runs)
            raw_score = (gcc_avg / max(ours_avg, 1e-9)) * 100.0
            score = min(100.0, raw_score) if args.cap_score else raw_score
            result = CaseResult(case, gcc_avg, ours_avg, raw_score, score, gcc_runs, ours_runs)
            results.append(result)
            print(
                f"{Color.GREEN}[AC]{Color.RESET} {format_case(case)} "
                f"gcc={gcc_avg:.6f}s ours={ours_avg:.6f}s score={score:8.4f} "
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
    print(f"选中用例数: {len(cases)}")
    print(f"AC 用例数:   {len(results)}")
    print(f"失败数:      {len(failures)}")
    if failures:
        print(f"{Color.RED}存在非 AC 用例，不输出最终分数。{Color.RESET}")
        print(f"失败报告: {work_root / 'reports' / 'failure_report.txt'}")
        return 1

    raw_final = geometric_mean([result.raw_score for result in results])
    final = geometric_mean([result.score for result in results])
    gcc_total_runtime = sum(sum(result.gcc_runs) for result in results)
    ours_total_runtime = sum(sum(result.ours_runs) for result in results)
    timed_run_count = sum(len(result.ours_runs) for result in results)
    print(f"原始几何平均分: {raw_final:.6f}")
    if args.cap_score:
        print(f"最终性能分:     {final:.6f} (单项封顶 100)")
    else:
        print(f"最终性能分:     {final:.6f}")
    print(f"gcc 计时总运行时间:  {gcc_total_runtime:.6f}s")
    print(f"我方计时总运行时间: {ours_total_runtime:.6f}s")
    if timed_run_count:
        print(f"gcc 单次计时平均:    {gcc_total_runtime / timed_run_count:.6f}s")
        print(f"我方单次计时平均:   {ours_total_runtime / timed_run_count:.6f}s")
    print(f"用例 CSV: {work_root / 'reports' / 'score_cases.csv'}")
    print(f"汇总 JSON: {work_root / 'reports' / 'score_summary.json'}")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
