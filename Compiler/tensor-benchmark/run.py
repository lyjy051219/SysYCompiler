#!/usr/bin/env python3
# Copyright (c) 2026 Compiler contributors
# SPDX-License-Identifier: MIT


from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
CASE_DIR = SCRIPT_DIR / "cases"
MANIFEST_FILE = SCRIPT_DIR / "manifest.json"
DEFAULT_RESULT_DIR = SCRIPT_DIR / "results"
DEFAULT_WORK_DIR = SCRIPT_DIR / "_work"
RUNTIME_SOURCE = PROJECT_DIR / "runtime" / "sylib.c"
RISCV_MARCH = "rv64gc"
RISCV_ABI = "lp64d"
TIMER_PATTERN = re.compile(
    rb"Timer@\d+-\d+:\s*(\d+)H-(\d+)M-(\d+)S-(\d+)us"
)


def default_compiler() -> Path:
    current = PROJECT_DIR / "build-riscv" / "compiler"
    if current.is_file():
        return current
    return PROJECT_DIR / "build-riscv-release" / "compiler"


def positive_integer(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("必须是正整数")
    return value


def nonnegative_integer(text: str) -> int:
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("必须是非负整数")
    return value


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="编译并重复运行 tensor RISC-V 性能基准"
    )
    parser.add_argument(
        "--compiler", type=Path, default=default_compiler(),
        help="待测编译器路径",
    )
    parser.add_argument(
        "--case", action="append", default=[],
        help="只运行名称中包含该文本的用例，可重复指定",
    )
    parser.add_argument(
        "--opt-level", choices=("O0", "O1", "O2"), default="O1",
        help="传给编译器的优化等级，默认 O1",
    )
    parser.add_argument(
        "--warmup", type=nonnegative_integer, default=1,
        help="每个用例正式计时前的预热次数，默认 1",
    )
    parser.add_argument(
        "--repeat", type=positive_integer, default=5,
        help="每个用例的正式计时次数，默认 5",
    )
    parser.add_argument(
        "--compile-timeout", type=positive_integer, default=600,
        help="单个用例编译超时秒数，默认 600",
    )
    parser.add_argument(
        "--tool-timeout", type=positive_integer, default=120,
        help="汇编、链接超时秒数，默认 120",
    )
    parser.add_argument(
        "--run-timeout", type=positive_integer, default=120,
        help="单次 QEMU 运行超时秒数，默认 120",
    )
    parser.add_argument(
        "--riscv-prefix", default=os.environ.get("RISCV_PREFIX", ""),
        help="RISC-V 工具链前缀，例如 riscv64-linux-gnu-",
    )
    parser.add_argument(
        "--qemu", type=Path,
        help="qemu-riscv64 路径，默认从 PATH 查找",
    )
    parser.add_argument(
        "--qemu-cpu", default="rv64",
        help="传给 qemu-riscv64 -cpu 的型号，默认 rv64",
    )
    parser.add_argument(
        "--work-dir", type=Path, default=DEFAULT_WORK_DIR,
        help="中间产物目录",
    )
    parser.add_argument(
        "--output-dir", type=Path, default=DEFAULT_RESULT_DIR,
        help="未显式指定 JSON/CSV 时的结果目录",
    )
    parser.add_argument(
        "--json", dest="json_path", type=Path,
        help="JSON 报告路径",
    )
    parser.add_argument(
        "--csv", dest="csv_path", type=Path,
        help="CSV 报告路径",
    )
    parser.add_argument(
        "--label", default="",
        help="写入报告的实验标签，例如 baseline 或 tensor-loop",
    )
    parser.add_argument(
        "--compare-json", type=Path,
        help="读取另一份本 runner 生成的 JSON，并报告逐用例加速比",
    )
    parser.add_argument(
        "--keep-work", action="store_true",
        help="保留已有工作目录；默认运行前清空",
    )
    return parser.parse_args()


def decode(data: bytes) -> str:
    return data.decode("utf-8", errors="replace")


def normalize_text(text: str) -> str:
    return text.replace("\r\n", "\n")


def first_line(command: list[str]) -> str:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, check=False)
    text = result.stdout.strip() or result.stderr.strip()
    return text.splitlines()[0] if text else "unknown"


def command_result(
    command: list[str], timeout: int, cwd: Path = PROJECT_DIR,
    stdin_path: Optional[Path] = None,
) -> tuple[subprocess.CompletedProcess[bytes], float]:
    input_file = stdin_path.open("rb") if stdin_path else None
    started = time.perf_counter()
    try:
        result = subprocess.run(
            command, cwd=cwd, stdin=input_file,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, check=False,
        )
    finally:
        if input_file:
            input_file.close()
    return result, time.perf_counter() - started


def find_riscv_gcc(prefix: str) -> str:
    candidates: list[str] = []
    if prefix:
        candidates.append(prefix + "gcc")
    candidates.extend((
        "riscv64-linux-gnu-gcc",
        "riscv64-unknown-linux-gnu-gcc",
        "riscv64-conda-linux-gnu-gcc",
    ))
    for candidate in candidates:
        path = shutil.which(candidate)
        if path:
            return path
    raise RuntimeError("找不到 RISC-V GCC；请设置 --riscv-prefix 或 RISCV_PREFIX")


def find_qemu(path: Optional[Path]) -> str:
    if path:
        resolved = path.expanduser().resolve()
        if resolved.is_file() and os.access(resolved, os.X_OK):
            return str(resolved)
        raise RuntimeError(f"qemu 不可执行：{resolved}")
    discovered = shutil.which("qemu-riscv64")
    if not discovered:
        raise RuntimeError("找不到 qemu-riscv64")
    return discovered


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def git_text(arguments: list[str]) -> str:
    result = subprocess.run(
        ["git", *arguments], cwd=PROJECT_DIR,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, check=False,
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def load_manifest() -> dict[str, dict[str, Any]]:
    entries = json.loads(MANIFEST_FILE.read_text(encoding="utf-8"))
    return {entry["name"]: entry for entry in entries}


def select_cases(filters: list[str]) -> list[Path]:
    cases = sorted(CASE_DIR.glob("*.sy"))
    if filters:
        cases = [case for case in cases
                 if any(text in case.stem for text in filters)]
    if not cases:
        raise RuntimeError("没有匹配到 tensor 性能用例")
    for case in cases:
        if not case.with_suffix(".in").is_file():
            raise RuntimeError(f"缺少输入文件：{case.with_suffix('.in')}")
        if not case.with_suffix(".out").is_file():
            raise RuntimeError(f"缺少期望输出：{case.with_suffix('.out')}")
    return cases


def prepare_work_directory(path: Path, keep: bool) -> None:
    resolved = path.expanduser().resolve()
    protected = {
        Path("/").resolve(), Path.home().resolve(), PROJECT_DIR.resolve(),
        PROJECT_DIR.parent.resolve(), SCRIPT_DIR.resolve(),
    }
    if resolved in protected:
        raise RuntimeError(f"拒绝把过宽路径作为工作目录：{resolved}")
    if not keep and resolved.exists():
        shutil.rmtree(resolved)
    resolved.mkdir(parents=True, exist_ok=True)


def compile_runtime(
    gcc: str, work_dir: Path, timeout: int,
) -> Path:
    runtime_object = work_dir / "sylib.o"
    command = [
        gcc, f"-march={RISCV_MARCH}", f"-mabi={RISCV_ABI}",
        "-O2", "-w", "-c", str(RUNTIME_SOURCE), "-o", str(runtime_object),
    ]
    result, _ = command_result(command, timeout)
    if result.returncode != 0:
        raise RuntimeError("编译 SysY 运行库失败：\n" + decode(result.stderr)[-4000:])
    return runtime_object


def timer_seconds(stderr: bytes) -> Optional[float]:
    records = TIMER_PATTERN.findall(stderr)
    if not records:
        return None
    microseconds = 0
    for hours, minutes, seconds, micros in records:
        microseconds += (
            ((int(hours) * 60 + int(minutes)) * 60 + int(seconds)) * 1_000_000
            + int(micros)
        )
    return microseconds / 1_000_000.0


def expected_program_output(case: Path) -> str:
    return normalize_text(case.with_suffix(".out").read_text(encoding="utf-8"))


def actual_program_output(result: subprocess.CompletedProcess[bytes]) -> str:
    output = normalize_text(decode(result.stdout))
    if output and not output.endswith("\n"):
        output += "\n"
    return output + f"{result.returncode}\n"


def validate_execution(
    case: Path, result: subprocess.CompletedProcess[bytes],
) -> tuple[bool, str, Optional[float]]:
    if result.returncode < 0:
        return False, f"QEMU 被信号 {-result.returncode} 终止", None
    expected = expected_program_output(case)
    actual = actual_program_output(result)
    if actual.splitlines() != expected.splitlines():
        return False, f"输出不一致：expected={expected!r}, actual={actual!r}", None
    target_time = timer_seconds(result.stderr)
    if target_time is None:
        return False, "stderr 中没有 Timer@ 记录，拒绝以宿主机墙钟代替目标计时", None
    return True, "", target_time


def failed_case(name: str, stage: str, detail: str, metadata: dict[str, Any]) -> dict[str, Any]:
    return {
        "name": name,
        "status": "FAIL",
        "stage": stage,
        "detail": detail,
        "metadata": metadata,
    }


def test_case(
    case: Path, metadata: dict[str, Any], compiler: Path,
    gcc: str, qemu: str, runtime_object: Path,
    work_dir: Path, arguments: argparse.Namespace,
) -> dict[str, Any]:
    case_work = work_dir / case.stem
    if case_work.exists():
        shutil.rmtree(case_work)
    case_work.mkdir(parents=True)
    assembly = case_work / "test.s"
    llvm_ir = case_work / "test.ll"
    object_file = case_work / "test.o"
    executable = case_work / "test"

    compile_command = [
        str(compiler), str(case), "-S", "-o", str(assembly),
        "-" + arguments.opt_level,
    ]
    try:
        compiled, compile_seconds = command_result(
            compile_command, arguments.compile_timeout
        )
    except subprocess.TimeoutExpired:
        return failed_case(case.stem, "compile", "编译超时", metadata)
    if compiled.returncode != 0 or not assembly.is_file():
        detail = decode(compiled.stderr) or decode(compiled.stdout)
        return failed_case(case.stem, "compile", detail[-6000:], metadata)

    assemble_command = [
        gcc, f"-march={RISCV_MARCH}", f"-mabi={RISCV_ABI}",
        "-c", str(assembly), "-o", str(object_file),
    ]
    try:
        assembled, assemble_seconds = command_result(
            assemble_command, arguments.tool_timeout
        )
    except subprocess.TimeoutExpired:
        return failed_case(case.stem, "assemble", "汇编超时", metadata)
    if assembled.returncode != 0:
        return failed_case(case.stem, "assemble", decode(assembled.stderr)[-6000:], metadata)

    link_command = [
        gcc, f"-march={RISCV_MARCH}", f"-mabi={RISCV_ABI}", "-static",
        str(object_file), str(runtime_object), "-lm", "-o", str(executable),
    ]
    try:
        linked, link_seconds = command_result(link_command, arguments.tool_timeout)
    except subprocess.TimeoutExpired:
        return failed_case(case.stem, "link", "链接超时", metadata)
    if linked.returncode != 0:
        return failed_case(case.stem, "link", decode(linked.stderr)[-6000:], metadata)

    target_samples: list[float] = []
    wall_samples: list[float] = []
    total_runs = arguments.warmup + arguments.repeat
    input_path = case.with_suffix(".in")
    qemu_command = [qemu, "-cpu", arguments.qemu_cpu, str(executable)]
    for run_index in range(total_runs):
        try:
            executed, wall_seconds = command_result(
                qemu_command, arguments.run_timeout, stdin_path=input_path
            )
        except subprocess.TimeoutExpired:
            return failed_case(case.stem, "run", "QEMU 运行超时", metadata)
        valid, detail, target_seconds = validate_execution(case, executed)
        if not valid or target_seconds is None:
            stderr = decode(executed.stderr)[-3000:]
            return failed_case(case.stem, "verify", detail + "\n" + stderr, metadata)
        if run_index >= arguments.warmup:
            target_samples.append(target_seconds)
            wall_samples.append(wall_seconds)

    target_median = statistics.median(target_samples)
    wall_median = statistics.median(wall_samples)
    result: dict[str, Any] = {
        "name": case.stem,
        "status": "PASS",
        "metadata": metadata,
        "compile_seconds": compile_seconds,
        "assemble_seconds": assemble_seconds,
        "link_seconds": link_seconds,
        "assembly_bytes": assembly.stat().st_size,
        "assembly_lines": sum(1 for _ in assembly.open("rb")),
        "object_bytes": object_file.stat().st_size,
        "executable_bytes": executable.stat().st_size,
        "ir_bytes": llvm_ir.stat().st_size if llvm_ir.is_file() else None,
        "target_samples_seconds": target_samples,
        "target_median_seconds": target_median,
        "target_min_seconds": min(target_samples),
        "target_max_seconds": max(target_samples),
        "wall_samples_seconds": wall_samples,
        "wall_median_seconds": wall_median,
    }
    if len(target_samples) > 1 and target_median > 0.0:
        result["target_relative_stdev"] = (
            statistics.stdev(target_samples) / target_median
        )
    else:
        result["target_relative_stdev"] = 0.0
    return result


def safe_label(text: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z_.-]+", "-", text.strip())
    return cleaned.strip("-") or "run"


def report_paths(arguments: argparse.Namespace) -> tuple[Path, Path]:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    label = safe_label(arguments.label or arguments.compiler.stem)
    output_dir = arguments.output_dir.expanduser().resolve()
    json_path = (
        arguments.json_path.expanduser().resolve()
        if arguments.json_path else output_dir / f"{stamp}-{label}.json"
    )
    csv_path = (
        arguments.csv_path.expanduser().resolve()
        if arguments.csv_path else output_dir / f"{stamp}-{label}.csv"
    )
    return json_path, csv_path


def write_json(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8")


def write_csv(path: Path, cases: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    columns = [
        "name", "status", "category", "element_type", "shape", "rounds",
        "compile_seconds", "target_median_seconds", "target_min_seconds",
        "target_max_seconds", "target_relative_stdev", "wall_median_seconds",
        "assembly_bytes", "assembly_lines", "ir_bytes", "stage", "detail",
    ]
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=columns, lineterminator="\n"
        )
        writer.writeheader()
        for result in cases:
            metadata = result.get("metadata", {})
            row = {column: result.get(column, "") for column in columns}
            row["category"] = metadata.get("category", "")
            row["element_type"] = metadata.get("element_type", "")
            row["shape"] = "x".join(str(value) for value in metadata.get("shape", []))
            row["rounds"] = metadata.get("rounds", "")
            writer.writerow(row)


def compare_results(
    current: list[dict[str, Any]], reference_path: Path,
) -> dict[str, Any]:
    reference_report = json.loads(reference_path.read_text(encoding="utf-8"))
    reference = {entry["name"]: entry for entry in reference_report["cases"]}
    speedups: dict[str, float] = {}
    for entry in current:
        old = reference.get(entry["name"])
        if entry.get("status") != "PASS" or not old or old.get("status") != "PASS":
            continue
        old_time = float(old["target_median_seconds"])
        new_time = float(entry["target_median_seconds"])
        if old_time > 0.0 and new_time > 0.0:
            speedups[entry["name"]] = old_time / new_time
    geometric_mean = None
    if speedups:
        geometric_mean = math.exp(
            sum(math.log(value) for value in speedups.values()) / len(speedups)
        )
    return {
        "reference_json": str(reference_path.resolve()),
        "speedup_by_case": speedups,
        "geometric_mean_speedup": geometric_mean,
    }


def print_case_result(index: int, total: int, result: dict[str, Any]) -> None:
    if result["status"] == "PASS":
        print(
            f"[PASS] [{index}/{total}] {result['name']:<34} "
            f"target median={result['target_median_seconds']:.6f}s "
            f"compile={result['compile_seconds']:.3f}s",
            flush=True,
        )
    else:
        detail = result.get("detail", "").splitlines()
        summary = detail[0] if detail else "未知错误"
        print(
            f"[FAIL] [{index}/{total}] {result['name']:<34} "
            f"stage={result.get('stage', 'unknown')} {summary}",
            flush=True,
        )


def main() -> int:
    arguments = parse_arguments()
    compiler = arguments.compiler.expanduser().resolve()
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        raise RuntimeError(f"编译器不可执行：{compiler}")
    gcc = find_riscv_gcc(arguments.riscv_prefix)
    qemu = find_qemu(arguments.qemu)
    work_dir = arguments.work_dir.expanduser().resolve()
    prepare_work_directory(work_dir, arguments.keep_work)
    runtime_object = compile_runtime(gcc, work_dir, arguments.tool_timeout)

    manifest = load_manifest()
    cases = select_cases(arguments.case)
    unknown = [case.stem for case in cases if case.stem not in manifest]
    if unknown:
        raise RuntimeError("manifest.json 缺少用例：" + ", ".join(unknown))

    print(f"[环境] compiler={compiler}")
    print(f"[环境] compiler_sha256={sha256_file(compiler)}")
    print(f"[环境] gcc={first_line([gcc, '--version'])}")
    print(f"[环境] qemu={first_line([qemu, '--version'])}, cpu={arguments.qemu_cpu}")
    print(
        "[提示] QEMU 计时用于同机相对比较，不模拟目标板缓存、乱序、主频和内存系统。"
    )

    results: list[dict[str, Any]] = []
    for index, case in enumerate(cases, 1):
        result = test_case(
            case, manifest[case.stem], compiler, gcc, qemu,
            runtime_object, work_dir, arguments,
        )
        results.append(result)
        print_case_result(index, len(cases), result)

    report: dict[str, Any] = {
        "schema_version": 1,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "label": arguments.label,
        "configuration": {
            "compiler": str(compiler),
            "compiler_sha256": sha256_file(compiler),
            "optimization_level": arguments.opt_level,
            "warmup": arguments.warmup,
            "repeat": arguments.repeat,
            "riscv_march": RISCV_MARCH,
            "riscv_abi": RISCV_ABI,
            "qemu_cpu": arguments.qemu_cpu,
        },
        "environment": {
            "git_commit": git_text(["rev-parse", "HEAD"]),
            "git_branch": git_text(["branch", "--show-current"]),
            "git_dirty": bool(git_text(["status", "--porcelain"])),
            "gcc_version": first_line([gcc, "--version"]),
            "qemu_version": first_line([qemu, "--version"]),
            "python_version": sys.version.splitlines()[0],
        },
        "cases": results,
    }
    if arguments.compare_json:
        comparison = compare_results(results, arguments.compare_json.expanduser().resolve())
        report["comparison"] = comparison
        print("\n[对比]")
        for name, speedup in comparison["speedup_by_case"].items():
            print(f"  {name:<34} {speedup:.4f}x")
        geometric_mean = comparison["geometric_mean_speedup"]
        if geometric_mean is not None:
            print(f"  几何平均加速比{'':<22} {geometric_mean:.4f}x")

    json_path, csv_path = report_paths(arguments)
    write_json(json_path, report)
    write_csv(csv_path, results)
    print(f"\n[报告] JSON: {json_path}")
    print(f"[报告] CSV:  {csv_path}")

    failures = sum(result["status"] != "PASS" for result in results)
    print(f"[汇总] 总计 {len(results)}，通过 {len(results) - failures}，失败 {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        sys.stderr.write(f"[错误] {error}\n")
        sys.exit(2)
