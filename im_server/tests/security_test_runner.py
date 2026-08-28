#!/usr/bin/env python3
"""Build and run the IM server security regression suite with sanitizers.

Outputs an HTML report containing test status, sanitizer findings and source
locations. The security build is isolated from the normal build directory.
"""

from __future__ import annotations

import argparse
import datetime as dt
import html
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
SERVER_DIR = ROOT / "im_server"
DEFAULT_BUILD_DIR = SERVER_DIR / "build-security"
DEFAULT_LEAK_BUILD_DIR = SERVER_DIR / "build-leaks"
DEFAULT_REPORT_DIR = SERVER_DIR / "security-reports"

SANITIZER_PATTERNS = {
    "悬垂指针/释放后使用": re.compile(r"heap-use-after-free|stack-use-after-return|stack-use-after-scope", re.I),
    "内存泄漏": re.compile(
        r"LeakSanitizer|detected memory leaks|Direct leak|Indirect leak|(?:[1-9]\d*) leaks? for",
        re.I,
    ),
    "越界访问": re.compile(r"buffer-overflow|out of bounds|container-overflow", re.I),
    "重复释放/非法释放": re.compile(r"double-free|attempting free on address|invalid free", re.I),
    "空指针/非法地址": re.compile(r"SEGV|DEADLYSIGNAL|null pointer|misaligned address", re.I),
    "未定义行为": re.compile(r"runtime error:|UndefinedBehaviorSanitizer", re.I),
    "数据竞争": re.compile(r"ThreadSanitizer|data race", re.I),
}

SOURCE_LOCATION = re.compile(
    r"(?P<path>(?:/[^\s:()]+)+\.(?:cpp|cc|cxx|c|h|hpp|kt)):(?P<line>\d+)(?::\d+)?"
)


class Result:
    def __init__(self, name: str, purpose: str, command: list[str], code: int,
                 output: str, seconds: float) -> None:
        self.name = name
        self.purpose = purpose
        self.command = command
        self.code = code
        self.output = output
        self.seconds = seconds
        self.findings = [name for name, pattern in SANITIZER_PATTERNS.items() if pattern.search(output)]
        self.locations = []
        seen = set()
        for match in SOURCE_LOCATION.finditer(output):
            item = (match.group("path"), int(match.group("line")))
            if item not in seen:
                seen.add(item)
                self.locations.append(item)

    @property
    def passed(self) -> bool:
        return self.code == 0 and not self.findings


def run(name: str, purpose: str, command: list[str], cwd: Path,
        env: dict[str, str]) -> Result:
    print(f"\n[安全测试] {name}\n  {' '.join(command)}", flush=True)
    started = time.monotonic()
    process = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    elapsed = time.monotonic() - started
    print(process.stdout, end="")
    status = "通过" if process.returncode == 0 else f"失败({process.returncode})"
    print(f"[安全测试] {name}: {status}, {elapsed:.2f}s", flush=True)
    return Result(name, purpose, command, process.returncode, process.stdout, elapsed)


def render_report(results: list[Result], report_path: Path, build_output: str,
                  leak_method: str) -> None:
    passed = sum(result.passed for result in results)
    overall = passed == len(results)
    cards = []
    for result in results:
        status_class = "pass" if result.passed else "fail"
        status_text = "通过" if result.passed else "发现问题"
        findings = "、".join(result.findings) if result.findings else "未发现 Sanitizer 异常"
        locations = "".join(
            f'<li><code>{html.escape(path)}:{line}</code></li>'
            for path, line in result.locations[:20]
        ) or "<li>没有错误源码位置</li>"
        cards.append(f"""
        <section class="card {status_class}">
          <div class="title"><h2>{html.escape(result.name)}</h2><span>{status_text}</span></div>
          <p>{html.escape(result.purpose)}</p>
          <div class="meta">耗时 {result.seconds:.2f}s · 退出码 {result.code}</div>
          <p><strong>检测结果：</strong>{html.escape(findings)}</p>
          <details><summary>源码定位</summary><ul>{locations}</ul></details>
          <details><summary>完整日志</summary><pre>{html.escape(result.output)}</pre></details>
        </section>""")

    leak_note = leak_method
    document = f"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>即通 IM 安全回归报告</title>
<style>
body{{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#f4f7fb;color:#172033;margin:0}}
main{{max-width:1100px;margin:36px auto;padding:0 20px}}h1{{margin-bottom:6px}}.sub{{color:#657087}}
.summary{{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;margin:24px 0}}
.metric,.card{{background:white;border-radius:14px;padding:18px;box-shadow:0 5px 20px #20305012}}
.metric b{{display:block;font-size:28px;color:#1268d5}}.card{{margin:14px 0;border-left:6px solid #19a463}}
.card.fail{{border-left-color:#e5484d}}.title{{display:flex;align-items:center;justify-content:space-between;gap:12px}}
.title h2{{font-size:18px;margin:0}}.title span{{border-radius:20px;padding:5px 11px;background:#dcf7e8;color:#08783e}}
.fail .title span{{background:#ffe4e5;color:#b4232a}}.meta{{color:#657087;font-size:13px}}details{{margin-top:10px}}
pre{{white-space:pre-wrap;word-break:break-word;background:#101827;color:#dce7ff;padding:14px;border-radius:9px;max-height:500px;overflow:auto}}
code{{word-break:break-all}}@media(max-width:700px){{.summary{{grid-template-columns:1fr}}}}
</style></head><body><main>
<h1>即通 IM 安全回归报告</h1>
<div class="sub">生成时间：{dt.datetime.now().astimezone().isoformat(timespec='seconds')}</div>
<div class="summary">
 <div class="metric"><b>{'安全通过' if overall else '需要修复'}</b>总体状态</div>
 <div class="metric"><b>{passed}/{len(results)}</b>通过测试</div>
 <div class="metric"><b>{sum(len(r.findings) for r in results)}</b>Sanitizer 告警</div>
</div>
<p>{html.escape(leak_note)}</p>
{''.join(cards)}
<details><summary>安全构建日志</summary><pre>{html.escape(build_output)}</pre></details>
</main></body></html>"""
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(document, encoding="utf-8")
    shutil.copyfile(report_path, report_path.parent / "latest.html")


def main() -> int:
    parser = argparse.ArgumentParser(description="运行 IM 服务端内存与安全回归测试并生成 HTML 报告")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--report-dir", type=Path, default=DEFAULT_REPORT_DIR)
    parser.add_argument("--clean", action="store_true", help="删除并重新生成安全构建目录")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    leak_build_dir = DEFAULT_LEAK_BUILD_DIR.resolve()
    report_dir = args.report_dir.resolve()
    if args.clean and build_dir.exists():
        shutil.rmtree(build_dir)

    sanitizer_flags = "-fsanitize=address,undefined -fno-omit-frame-pointer -g"
    configure = [
        "cmake", "-S", str(SERVER_DIR), "-B", str(build_dir),
        "-DIM_SERVER_BUILD_TESTS=ON", "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_CXX_FLAGS={sanitizer_flags}",
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined",
    ]
    if sys.platform == "darwin":
        configure.append("-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3")

    env = os.environ.copy()
    leak_sanitizer_supported = sys.platform != "darwin"
    detect_leaks = "1" if leak_sanitizer_supported else "0"
    env["ASAN_OPTIONS"] = f"detect_leaks={detect_leaks}:halt_on_error=0:abort_on_error=0:symbolize=1"
    env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=0"

    config_result = run("Sanitizer 构建配置", "启用 ASan 与 UBSan", configure, ROOT, env)
    build_command = [
        "cmake", "--build", str(build_dir), "--target",
        "test_db_write_queue", "test_conversation_migration", "test_token_service",
        "test_database_concurrency", "test_e2e", "-j4",
    ]
    build_result = run("安全测试编译", "生成带源码行号的测试程序", build_command, ROOT, env)
    build_output = config_result.output + "\n" + build_result.output

    results: list[Result] = []
    if config_result.code == 0 and build_result.code == 0:
        test_specs = [
            ("数据库写队列生命周期", "未启动拒绝、异常传播、移动捕获、嵌套提交防死锁及并发严格串行",
             build_dir / "test_db_write_queue"),
            ("会话 ID 数据迁移", "旧碰撞公式迁移到 32+32 位打包、双向查询及重复启动幂等性",
             build_dir / "test_conversation_migration"),
            ("密码与 Token 安全", "Argon2id、旧密码迁移、刷新幂等及重放撤销",
             build_dir / "test_token_service"),
            ("数据库并发安全", "并发读写、事务及 SQLite 连接池稳定性",
             build_dir / "test_database_concurrency"),
            ("TLS 登录与内存安全 E2E", "真实 TLS 登录、连续异步写、消息漫游及大文件分片；可捕获悬垂 buffer",
             build_dir / "test_e2e"),
        ]
        for name, purpose, executable in test_specs:
            results.append(run(name, purpose, [str(executable)], ROOT, env))
        if sys.platform == "darwin" and shutil.which("leaks"):
            # leaks 无法分析 ASan 的 malloc 替代器，因此使用独立、无 Sanitizer 的 Debug 构建。
            leak_configure = [
                "cmake", "-S", str(SERVER_DIR), "-B", str(leak_build_dir),
                "-DIM_SERVER_BUILD_TESTS=ON", "-DCMAKE_BUILD_TYPE=Debug",
                "-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3",
            ]
            leak_config = run("macOS 泄漏构建配置", "准备与 leaks 兼容的普通 Debug 构建", leak_configure, ROOT, env)
            leak_build_command = [
                "cmake", "--build", str(leak_build_dir), "--target",
                "test_db_write_queue", "test_conversation_migration", "test_token_service",
                "test_database_concurrency", "test_e2e", "-j4",
            ]
            leak_build = run("macOS 泄漏测试编译", "编译不注入 ASan 的泄漏测试程序", leak_build_command, ROOT, env)
            build_output += "\n" + leak_config.output + "\n" + leak_build.output
            if leak_config.code == 0 and leak_build.code == 0:
                for name, purpose, executable in test_specs:
                    leak_executable = leak_build_dir / executable.name
                    results.append(run(
                        f"{name}（macOS 泄漏扫描）",
                        f"使用 /usr/bin/leaks 检查：{purpose}",
                        ["/usr/bin/leaks", "--atExit", "--", str(leak_executable)],
                        ROOT,
                        env,
                    ))
            else:
                results.append(Result(
                    "macOS 泄漏构建", "泄漏扫描程序必须先成功编译", leak_build_command,
                    leak_build.code or leak_config.code,
                    leak_config.output + "\n" + leak_build.output,
                    0.0,
                ))
    else:
        results.append(Result("安全构建", "安全测试必须先成功编译", build_command,
                              build_result.code or config_result.code, build_output, 0.0))

    if leak_sanitizer_supported:
        leak_method = "内存泄漏由 LeakSanitizer 检测；悬垂指针、越界和非法释放由 AddressSanitizer 检测。"
    elif shutil.which("leaks"):
        leak_method = "Apple ASan 不提供 LeakSanitizer，本报告改用 macOS leaks 检查泄漏；其他内存错误由 AddressSanitizer 检测。"
    else:
        leak_method = "当前平台没有可用的泄漏扫描器；悬垂指针、越界和非法释放仍由 AddressSanitizer 检测。"
    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    report_path = report_dir / f"security-report-{timestamp}.html"
    render_report(results, report_path, build_output, leak_method)
    print(f"\n[安全测试] HTML 报告: {report_path}")
    print(f"[安全测试] 最新报告: {report_dir / 'latest.html'}")
    return 0 if all(result.passed for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
