# Copyright (c) 2026 Compiler contributors
# SPDX-License-Identifier: MIT

# 由 backendtest.ipynb 提取的独立后端测试脚本，配置可用环境变量 TEST_ROOT MAX_CASES SINGLE_CASES SKIP_BUILD 覆盖。
# 后端专用测试脚本：完整编译器 -> RISC-V 汇编 -> 静态链接 -> qemu-riscv64 运行 -> 对比 .out。
# 这一版放在一个单元格里，日志格式尽量和 test.ipynb 保持一致。

# 导入 os：读取环境变量，例如 RISCV_PREFIX。
import os
# 导入 re：解析运行库 stderr 中的 TOTAL 时间。
import re
# 导入 shutil：查找工具链、复制用例、清理临时目录。
import shutil
# 导入 subprocess：调用 cmake、编译器、汇编器、链接器和 qemu。
import subprocess
# 导入 time：统计单个用例和整轮测试耗时。
import time
# 导入 hashlib：给失败用例生成稳定的短标识。
import hashlib
# 从 pathlib 导入 Path：用稳定方式拼接路径。
from pathlib import Path
# 从 difflib 导入 unified_diff：生成 WA 时的输出差异。
from difflib import unified_diff

# Color 保存终端颜色，和 test.ipynb 的颜色风格一致。
class Color:
    # 绿色用于 AC。
    GREEN = '\033[92m'
    # 红色用于 CE/AE/LE/RE/WA/BAD。
    RED = '\033[91m'
    # 黄色用于 TLE 和提示。
    YELLOW = '\033[93m'
    # RESET 用于恢复默认颜色。
    RESET = '\033[0m'

# NOTEBOOK_CWD 是当前 Jupyter 内核的工作目录。
NOTEBOOK_CWD = Path.cwd().resolve()
# 如果当前工作目录就是 Compiler 目录，就直接把它当作项目目录。
if (NOTEBOOK_CWD / 'CMakeLists.txt').exists() and (NOTEBOOK_CWD / 'runtime').exists():
    # PROJECT 指向包含 CMakeLists.txt、runtime、testcases 的目录。
    PROJECT = NOTEBOOK_CWD
# 如果当前工作目录是仓库根目录，就使用下面的 Compiler 子目录。
elif (NOTEBOOK_CWD / 'Compiler' / 'CMakeLists.txt').exists():
    # PROJECT 指向仓库根目录下的 Compiler 子目录。
    PROJECT = NOTEBOOK_CWD / 'Compiler'
# 如果前两种都不是，就使用当前机器上的默认项目位置。
else:
    # PROJECT 使用绝对路径兜底，避免 notebook 从别的目录启动时找不到项目。
    PROJECT = Path('/home/lyjy/Compiler/Compiler').resolve()
# WORKSPACE 是仓库根目录，也就是 Compiler 项目的父目录。
WORKSPACE = PROJECT.parent
# BUILD_DIR 是完整编译器的 CMake 构建目录。
BUILD_DIR = PROJECT / 'build-riscv'
# COMPILER 是完整编译器可执行文件。
COMPILER = BUILD_DIR / 'compiler'
# TEST_ROOT 默认根目录，包含 functional + performance 两套用例
DEFAULT_TEST_BASE = WORKSPACE.parent / 'tests' / 'testcases26'
# 两套测试子目录
TEST_FUNC_DIR = DEFAULT_TEST_BASE / 'riscv-functional'
TEST_PERF_DIR = DEFAULT_TEST_BASE / 'riscv-performance'
# TEST_ROOT 环境变量优先级最高，一旦设置则只使用该目录，不再自动加载两套用例
TEST_ROOT = Path(os.environ['TEST_ROOT']).resolve() if os.environ.get('TEST_ROOT') else None
# RUNTIME_C 是 SysY 运行库源文件。
RUNTIME_C = PROJECT / 'runtime' / 'sylib.c'
# WORK_ROOT 是后端测试产生的临时目录。允许每个并发测试轮次使用独立目录，
# 避免固定的 current/test.s、test.in 和 test.out 相互覆盖。
WORK_ROOT = (Path(os.environ['BACKEND_TEST_WORK_ROOT']).resolve()
             if os.environ.get('BACKEND_TEST_WORK_ROOT')
             else PROJECT / 'backend_test_work')
# CASE_DIR 保存当前用例的临时源文件和产物。
CASE_DIR = WORK_ROOT / 'current'
# FAILURE_DIR 保存失败用例的报告和必要信息。
FAILURE_DIR = WORK_ROOT / 'failures'
# REPORT_FILE 保存整轮测试的失败详情。
REPORT_FILE = WORK_ROOT / 'failure_report.txt'
# SYLIB_OBJ 是运行库编译后的 RISC-V 目标文件。
SYLIB_OBJ = WORK_ROOT / 'sylib.o'

# BUILD_BEFORE_TEST 控制运行测试前是否自动构建完整编译器。
BUILD_BEFORE_TEST = os.environ.get('SKIP_BUILD') is None
# QUIET_SETUP 控制是否隐藏 CMake 和 sylib.c 编译 warning；失败时仍会打印完整错误摘要。
QUIET_SETUP = True
# CASE_PATTERNS 控制测试范围；默认递归测试所有 .sy。
CASE_PATTERNS = ['**/*.sy']
# SINGLE_CASES 非空时只运行这里列出的用例，支持绝对路径、相对 PROJECT、相对 WORKSPACE。
SINGLE_CASES = [p for p in os.environ.get('SINGLE_CASES', '').split(',') if p]
# MAX_CASES 用于快速冒烟测试；None 表示不限制数量。
MAX_CASES = int(os.environ['MAX_CASES']) if os.environ.get('MAX_CASES') else None
# STOP_ON_FIRST_FAIL 为 True 时遇到第一个失败用例就停止。
STOP_ON_FIRST_FAIL = False
# ========== 修改点1：编译超时改为600秒 ==========
COMPILE_TIMEOUT = 600
# ASSEMBLE_TIMEOUT 是汇编器处理单个 .s 文件的超时。
ASSEMBLE_TIMEOUT = 60
# LINK_TIMEOUT 是链接单个可执行文件的超时。
LINK_TIMEOUT = 60
# RUN_TIMEOUT 是 qemu 运行单个用例的超时；放宽到 300s，用于排查真实错误而不是脚本误杀。
RUN_TIMEOUT = 300
# BUILD_TIMEOUT 是完整编译器构建超时。
BUILD_TIMEOUT = 900
# CXX_COMPILER 指定 CMake 配置完整编译器时使用的 C++ 编译器。
CXX_COMPILER = 'clang++-18'
# FAILURE_DETAIL_LIMIT 控制每个 WA 在报告里最多写多少行 diff。
FAILURE_DETAIL_LIMIT = 120

# results 分类记录每种测试结果。
results = {name: [] for name in ['AC', 'CE', 'AE', 'LE', 'RE', 'WA', 'TLE', 'BAD']}
# failure_details 保存失败用例的详细原因，最后写入 failure_report.txt。
failure_details = []
# total_time 累计运行时间，显示格式和 test.ipynb 一致。
total_time = 0.0

# 这个函数把文件名格式化成固定宽度，和 test.ipynb 的输出对齐。
def format_filename(path, max_len=25):
    # 只显示 basename，避免日志被长路径撑开。
    filename = Path(path).name
    # 文件名过长时截断并加省略号。
    if len(filename) > max_len:
        filename = filename[:max_len - 3] + '...'
    # 左对齐到固定宽度。
    return filename.ljust(max_len)

# 这个函数把测试状态打印成 test.ipynb 风格的一行日志。
def print_case(status, case, runtime_time, index, total):
    # 根据状态选择颜色。
    color = Color.GREEN if status == 'AC' else Color.YELLOW if status == 'TLE' else Color.RED
    # 两字符状态后面补两个空格，三字符状态后面补一个空格，贴近 test.ipynb 的排版。
    gap = ' ' if len(status) >= 3 else '  '
    # 格式化文件名。
    formatted_name = format_filename(case)
    # 输出一行紧凑结果。
    print(f'{color}[{status}]{Color.RESET}{gap}{formatted_name} | Runtime:{runtime_time:.6f}s | Total:{total_time:>6.2f}s | [{index}/{total}]')

# 这个函数执行外部命令，并返回 CompletedProcess。
def run_command(args, timeout, stdin_path=None):
    # 如果存在输入文件，就打开它作为 stdin。
    stdin_file = stdin_path.open('rb') if stdin_path and stdin_path.exists() else None
    # try/finally 保证 stdin 文件句柄一定会关闭。
    try:
        # stdout/stderr 都捕获，避免构建 warning 打乱测试日志。
        return subprocess.run(args, cwd=WORKSPACE, stdin=stdin_file, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    # finally 用于关闭 stdin 文件。
    finally:
        # 如果打开过输入文件，就关闭它。
        if stdin_file:
            stdin_file.close()

# 这个函数执行 setup 阶段命令，默认安静，只在失败时打印摘要。
def run_setup_command(args, stage, timeout):
    # 执行命令并捕获 stdout/stderr。
    ret = subprocess.run(args, cwd=WORKSPACE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
    # 如果命令成功且不要求展开日志，就直接返回。
    if ret.returncode == 0 and QUIET_SETUP:
        return ret
    # 如果命令成功但 QUIET_SETUP=False，就打印原始输出。
    if ret.returncode == 0:
        print(ret.stdout, end='')
        print(ret.stderr, end='')
        return ret
    # 命令失败时打印阶段名称和命令，方便定位。
    print(f'{Color.RED}[{stage} failed]{Color.RESET} ' + ' '.join(args))
    # 打印 stdout 的最后一段。
    if ret.stdout:
        print(ret.stdout[-3000:])
    # 打印 stderr 的最后一段。
    if ret.stderr:
        print(ret.stderr[-3000:])
    # 抛出异常停止测试。
    raise RuntimeError(f'{stage} failed with exit code {ret.returncode}')

# 这个函数自动探测 RISC-V 工具链前缀。
def detect_riscv_prefix():
    # 优先读取用户手动指定的环境变量。
    env_prefix = os.environ.get('RISCV_PREFIX')
    # 如果环境变量可用，就直接返回。
    if env_prefix and shutil.which(env_prefix + 'gcc'):
        return env_prefix
    # 常见工具链前缀列表，包含当前机器可用的 riscv64-linux-gnu-。
    candidates = ['riscv64-linux-gnu-', 'riscv64-unknown-linux-gnu-', '/home/r1c0/riscv/bin/riscv64-unknown-linux-gnu-']
    # 逐个检查候选前缀。
    for prefix in candidates:
        # gcc 存在就认为该工具链可用。
        if shutil.which(prefix + 'gcc'):
            return prefix
    # 没找到工具链时给出明确错误。
    raise RuntimeError('未找到 RISC-V 工具链，请安装 riscv64-linux-gnu-gcc 或设置 RISCV_PREFIX。')

# RISCV_PREFIX 是调用 gcc/as 的工具链前缀。
RISCV_PREFIX = detect_riscv_prefix()
# RISCV_GCC 是 RISC-V 交叉编译器。
RISCV_GCC = RISCV_PREFIX + 'gcc'
# RISCV_AS 是 RISC-V 汇编器。
RISCV_AS = RISCV_PREFIX + 'as'
# QEMU 是 qemu-riscv64 的路径。
QEMU = shutil.which('qemu-riscv64')
# 找不到 qemu 就无法执行后端产物。
if not QEMU:
    raise RuntimeError('未找到 qemu-riscv64，请先安装 qemu-user。')

# 这个函数构建完整编译器。
def build_full_compiler():
    # configure_cmd 关闭前端/中端专用开关，从而启用完整编译器和 RISC-V 后端。
    configure_cmd = ['cmake', '-B', str(BUILD_DIR), '-S', str(PROJECT), '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_FRONTEND_ONLY=OFF', '-DBUILD_MIDDLE_END=OFF', '-DBUILD_FRONTEND_RELEASE=OFF', f'-DCMAKE_CXX_COMPILER={CXX_COMPILER}']
    # build_cmd 构建 compiler 目标。
    build_cmd = ['cmake', '--build', str(BUILD_DIR), '--config', 'Debug', '--target', 'compiler', '-j', '4']
    # 打印简短 setup 状态，避免 CMake warning 淹没测试输出。
    print(f'{Color.YELLOW}[SETUP]{Color.RESET} building full compiler ...')
    # 执行 CMake 配置。
    run_setup_command(configure_cmd, 'configure', BUILD_TIMEOUT)
    # 执行 CMake 构建。
    run_setup_command(build_cmd, 'build', BUILD_TIMEOUT)
    # 构建成功后打印一行完成提示。
    print(f'{Color.YELLOW}[SETUP]{Color.RESET} compiler ready: {COMPILER}')

# 这个函数准备临时目录并编译运行库。
def prepare_runtime():
    # 删除旧的临时目录，避免历史产物影响本轮测试。
    shutil.rmtree(WORK_ROOT, ignore_errors=True)
    # 创建当前用例目录。
    CASE_DIR.mkdir(parents=True, exist_ok=True)
    # 创建失败详情目录。
    FAILURE_DIR.mkdir(parents=True, exist_ok=True)
    # runtime_cmd 把 sylib.c 编译成 RISC-V 目标文件；-w 隐藏 scanf 返回值 warning。
    runtime_cmd = [RISCV_GCC, '-O2', '-w', '-c', str(RUNTIME_C), '-o', str(SYLIB_OBJ)]
    # 打印简短 setup 状态。
    print(f'{Color.YELLOW}[SETUP]{Color.RESET} compiling runtime ...')
    # 执行运行库编译。
    run_setup_command(runtime_cmd, 'runtime', 120)

# 这个函数把用户填写的路径解析成实际测试文件。
def resolve_case_path(path_text):
    # raw 保存原始路径。
    raw = Path(path_text)
    # 绝对路径直接使用。
    if raw.is_absolute():
        return raw.resolve()
    # 先尝试相对 PROJECT 的路径。
    candidate_from_project = (PROJECT / raw).resolve()
    # 如果相对 PROJECT 存在，就使用它。
    if candidate_from_project.exists():
        return candidate_from_project
    # 再尝试相对 WORKSPACE 的路径。
    candidate_from_workspace = (WORKSPACE / raw).resolve()
    # 如果相对 WORKSPACE 存在，就使用它。
    if candidate_from_workspace.exists():
        return candidate_from_workspace
    # 都不存在时返回 PROJECT 解释结果，后续错误更贴近项目目录。
    return candidate_from_project

# 这个函数收集本轮测试用例（修改点2：同时加载 functional + performance）
def collect_cases():
    # SINGLE_CASES 非空时只运行指定用例，优先级最高
    if SINGLE_CASES:
        cases = [resolve_case_path(p) for p in SINGLE_CASES]
    elif TEST_ROOT is not None:
        # 外部指定 TEST_ROOT，仅扫描该目录
        found = {case.resolve() for pattern in CASE_PATTERNS for case in TEST_ROOT.glob(pattern)}
        cases = sorted(case for case in found if case.suffix == '.sy')
    else:
        # 默认自动加载两套测试集：riscv-functional + riscv-performance
        all_found = set()
        # 加载 functional
        if TEST_FUNC_DIR.exists():
            for pat in CASE_PATTERNS:
                all_found.update(TEST_FUNC_DIR.glob(pat))
        # 加载 performance
        if TEST_PERF_DIR.exists():
            for pat in CASE_PATTERNS:
                all_found.update(TEST_PERF_DIR.glob(pat))
        # 过滤 .sy 并排序
        cases = sorted(case.resolve() for case in all_found if case.suffix == '.sy')

    # 排除已知坏用例目录（见 TESTING_NOTES.md）：rv_bad_tests 里的 .in/.out 与源码不匹配，
    # 或 main 缺少 return（返回值未定义），即使用参考 clang 也无法 AC，正常回归应跳过。
    # 可用环境变量 EXCLUDE_DIRS（逗号分隔的子串）覆盖，默认排除 rv_bad_tests。
    exclude_subs = [s for s in os.environ.get('EXCLUDE_DIRS', 'rv_bad_tests').split(',') if s]
    if exclude_subs:
        cases = [c for c in cases if not any(sub in str(c) for sub in exclude_subs)]

    # 如果设置 MAX_CASES，就截断列表。
    return cases[:MAX_CASES] if MAX_CASES is not None else cases

# 这个函数把当前用例复制到固定临时目录。
def stage_case(case):
    # 清理上一个用例的 current 目录。
    shutil.rmtree(CASE_DIR, ignore_errors=True)
    # 创建 current 目录。
    CASE_DIR.mkdir(parents=True, exist_ok=True)
    # 把源文件复制成 test.sy，方便失败后复现。
    shutil.copy2(case, CASE_DIR / 'test.sy')
    # 同名 .in 是输入文件。
    input_file = case.with_suffix('.in')
    # 同名 .out 是标准答案。
    output_file = case.with_suffix('.out')
    # 输入文件存在时复制。
    if input_file.exists():
        shutil.copy2(input_file, CASE_DIR / 'test.in')
    # 输出文件存在时复制。
    if output_file.exists():
        shutil.copy2(output_file, CASE_DIR / 'test.out')
    # 返回临时源文件、输入文件和输出文件路径。
    return CASE_DIR / 'test.sy', CASE_DIR / 'test.in', CASE_DIR / 'test.out'

# 这个函数记录失败详情，最后统一写报告。
def record_failure(status, case, detail):
    # 计算相对路径用于报告。
    try:
        rel = str(case.relative_to(PROJECT))
    except ValueError:
        rel = str(case)
    # 保存状态、路径和详情。
    failure_details.append((status, rel, detail.strip()))

# 这个函数保存失败报告到 backend_test_work/failure_report.txt。
def write_failure_report():
    # 如果没有失败，写一个空报告说明。
    if not failure_details:
        REPORT_FILE.write_text('No failures.\n', encoding='utf-8')
        return
    # lines 收集报告内容。
    lines = []
    # 逐个失败用例写入状态和详情。
    for status, rel, detail in failure_details:
        lines.append(f'[{status}] {rel}')
        if detail:
            lines.append(detail)
        lines.append('')
    # 写入报告文件。
    REPORT_FILE.write_text('\n'.join(lines), encoding='utf-8')

# 这个函数从运行库 stderr 解析 TOTAL 时间。
def parse_runtime_seconds(stderr_bytes, fallback):
    # 把 stderr 解码成文本。
    stderr_text = stderr_bytes.decode('utf-8', errors='replace')
    # 匹配 sylib.c 打印的 TOTAL: 0H-0M-0S-0us。
    match = re.search(r'TOTAL:\s*(\d+)H-(\d+)M-(\d+)S-(\d+)us', stderr_text)
    # 如果没有匹配到，就用 qemu 墙钟时间兜底。
    if not match:
        return fallback
    # 解析小时、分钟、秒、微秒。
    hours, minutes, seconds, microseconds = map(int, match.groups())
    # 转换成秒。
    return hours * 3600 + minutes * 60 + seconds + microseconds / 1_000_000

# 这个函数编译单个用例到 RISC-V 汇编。
def compile_case(source_file):
    # asm_file 是汇编输出路径。
    asm_file = CASE_DIR / 'test.s'
    # compile_cmd 调用完整编译器，采用竞赛标准形式 compiler <输入> -S -o <输出>，
    # 与当前 main.cpp 的 inputPath=argv[1]、outputPath=argv[4] 读取方式一致。
    compile_cmd = [str(COMPILER), str(source_file), '-S', '-o', str(asm_file)]
    # 执行编译并返回结果。
    return run_command(compile_cmd, COMPILE_TIMEOUT), asm_file

# 这个函数把 RISC-V 汇编文件转成目标文件。
def assemble_case(asm_file):
    # obj_file 是汇编器输出目标文件。
    obj_file = CASE_DIR / 'test.o'
    # assemble_cmd 调用 RISC-V as。
    assemble_cmd = [RISCV_AS, str(asm_file), '-o', str(obj_file)]
    # 执行汇编并返回结果。
    return run_command(assemble_cmd, ASSEMBLE_TIMEOUT), obj_file

# 这个函数静态链接测试目标文件和 SysY 运行库。
def link_case(obj_file):
    # exe_file 是 qemu 要执行的 RISC-V 程序。
    exe_file = CASE_DIR / 'test'
    # link_cmd 使用 -static，避免 qemu 动态 sysroot 配置影响测试。
    link_cmd = [RISCV_GCC, '-static', str(obj_file), str(SYLIB_OBJ), '-o', str(exe_file)]
    # 执行链接并返回结果。
    return run_command(link_cmd, LINK_TIMEOUT), exe_file

# 这个函数运行 RISC-V 程序并和 .out 比较。
def run_and_compare(exe_file, input_file, expected_file):
    # 没有 .out 时无法判定正确性。
    if not expected_file.exists():
        return 'BAD', 0.0, 'missing expected .out file'
    # 记录 qemu 启动前时间。
    start = time.time()
    # 执行 qemu；如果 test.in 不存在，run_command 会自动不传 stdin。
    ret = run_command([QEMU, str(exe_file)], RUN_TIMEOUT, input_file)
    # 计算墙钟时间。
    elapsed = time.time() - start
    # 优先使用运行库 TOTAL 时间，没有 TOTAL 时使用墙钟时间。
    runtime_time = parse_runtime_seconds(ret.stderr, elapsed)
    # stdout 是程序正常输出。
    actual = ret.stdout.decode('utf-8', errors='replace')
    # 如果 stdout 非空且末尾没有换行，就补一个换行。
    if actual and not actual.endswith('\n'):
        actual += '\n'
    # SysY 评测规则：把 main 返回值作为最后一行加入输出。
    actual += str(ret.returncode) + '\n'
    # 读取标准答案。
    expected = expected_file.read_text(encoding='utf-8')
    # 生成统一 diff。
    diff = list(unified_diff(actual.splitlines(), expected.splitlines(), fromfile='actual', tofile='expected', lineterm=''))
    # diff 非空就是 WA。
    if diff:
        detail = '\n'.join(diff[:FAILURE_DETAIL_LIMIT])
        return 'WA', runtime_time, detail
    # 没有 diff 就是 AC。
    return 'AC', runtime_time, ''

# 这个函数测试一个用例，并返回状态和运行时间。
def test_one_case(case, index, total):
    # total_time 是全局累计时间。
    global total_time
    # 复制当前用例到临时目录。
    source_file, input_file, expected_file = stage_case(case)
    # 编译阶段。
    try:
        compile_ret, asm_file = compile_case(source_file)
    except subprocess.TimeoutExpired:
        total_time += COMPILE_TIMEOUT
        results['TLE'].append(case)
        record_failure('TLE', case, f'compile timeout after {COMPILE_TIMEOUT}s')
        print_case('TLE', case, 0.0, index, total)
        return 'TLE'
    # 编译器返回非零就是 CE。
    if compile_ret.returncode != 0:
        results['CE'].append(case)
        record_failure('CE', case, compile_ret.stderr.decode('utf-8', errors='replace')[-3000:])
        print_case('CE', case, 0.0, index, total)
        return 'CE'
    # 汇编阶段。
    try:
        assemble_ret, obj_file = assemble_case(asm_file)
    except subprocess.TimeoutExpired:
        total_time += ASSEMBLE_TIMEOUT
        results['TLE'].append(case)
        record_failure('TLE', case, f'assemble timeout after {ASSEMBLE_TIMEOUT}s')
        print_case('TLE', case, 0.0, index, total)
        return 'TLE'
    # 汇编器返回非零就是 AE。
    if assemble_ret.returncode != 0:
        results['AE'].append(case)
        record_failure('AE', case, assemble_ret.stderr.decode('utf-8', errors='replace')[-3000:])
        print_case('AE', case, 0.0, index, total)
        return 'AE'
    # 链接阶段。
    try:
        link_ret, exe_file = link_case(obj_file)
    except subprocess.TimeoutExpired:
        total_time += LINK_TIMEOUT
        results['TLE'].append(case)
        record_failure('TLE', case, f'link timeout after {LINK_TIMEOUT}s')
        print_case('TLE', case, 0.0, index, total)
        return 'TLE'
    # 链接器返回非零就是 LE。
    if link_ret.returncode != 0:
        results['LE'].append(case)
        record_failure('LE', case, link_ret.stderr.decode('utf-8', errors='replace')[-3000:])
        print_case('LE', case, 0.0, index, total)
        return 'LE'
    # qemu 运行和输出比较阶段。
    try:
        status, runtime_time, detail = run_and_compare(exe_file, input_file, expected_file)
    except subprocess.TimeoutExpired:
        total_time += RUN_TIMEOUT
        results['TLE'].append(case)
        record_failure('TLE', case, f'qemu timeout after {RUN_TIMEOUT}s')
        print_case('TLE', case, RUN_TIMEOUT, index, total)
        return 'TLE'
    # 累加真实运行时间。
    total_time += runtime_time
    # 记录状态。
    results[status].append(case)
    # 失败时记录详情。
    if status != 'AC':
        record_failure(status, case, detail)
    # 打印单行进度。
    print_case(status, case, runtime_time, index, total)
    # 返回状态。
    return status

# 这个函数打印最终统计，格式尽量贴近 test.ipynb。
def print_summary():
    # 先写失败报告。
    write_failure_report()
    # 输出空行分隔。
    print()
    # 输出分隔线。
    print('=' * 60)
    # 编译错误数量。
    print(f'Compiler Error:  {len(results["CE"]):>3}')
    # 汇编错误数量。
    print(f'Assembler Error: {len(results["AE"]):>3}')
    # 链接错误数量。
    print(f'Linker Error:    {len(results["LE"]):>3}')
    # 运行错误数量。
    print(f'Runtime Error:   {len(results["RE"]):>3}')
    # 超时数量。
    print(f'Timeout Error:   {len(results["TLE"]):>3}')
    # 答案错误数量。
    print(f'Wrong Answer:    {len(results["WA"]):>3}')
    # 测试本身缺少 .out 的数量。
    print(f'Bad Test:        {len(results["BAD"]):>3}')
    # AC 数量。
    print(f'{Color.GREEN}Accepted:        {len(results["AC"]):>3}{Color.RESET}')
    # 输出分隔线。
    print('=' * 60)
    # 输出累计运行时间。
    print(f'Total time: {total_time:.2f}s')
    # 如果存在失败，提示报告文件路径。
    if failure_details:
        print()
        print(f'{Color.YELLOW}失败详情已写入:{Color.RESET} {REPORT_FILE}')
        for name in ['CE', 'AE', 'LE', 'RE', 'WA', 'TLE', 'BAD']:
            if results[name]:
                print(f'{name}_LIST')
                for case in results[name]:
                    try:
                        print('  ' + str(case.relative_to(PROJECT)))
                    except ValueError:
                        print('  ' + str(case))

# 如果需要，就先构建完整编译器。
if BUILD_BEFORE_TEST:
    build_full_compiler()
# 确认完整编译器存在。
if not COMPILER.exists():
    raise FileNotFoundError(f'找不到完整编译器：{COMPILER}')
# 确认运行库存在。
if not RUNTIME_C.exists():
    raise FileNotFoundError(f'找不到运行库：{RUNTIME_C}')
# 准备运行库和临时目录。
prepare_runtime()
# 收集测试用例。
test_list = collect_cases()
# 没有测试用例就报错。
if not test_list:
    raise RuntimeError('没有找到任何 .sy 测试用例，请检查 TEST_ROOT/CASE_PATTERNS/SINGLE_CASES。')
# 打印简短环境信息。
print(f'{Color.YELLOW}[SETUP]{Color.RESET} cases: {len(test_list)}, compile_timeout={COMPILE_TIMEOUT}s, run_timeout={RUN_TIMEOUT}s')
# 逐个执行测试。
for test_count, test in enumerate(test_list, 1):
    # 测试当前用例。
    status = test_one_case(test, test_count, len(test_list))
    # 如果设置遇错停止，则遇到非 AC 就退出循环。
    if status != 'AC' and STOP_ON_FIRST_FAIL:
        break
# 打印汇总。
print_summary()
