import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import zipfile
import shlex


HOST = Path(sys.argv[1]).resolve()
ROOT = HOST.parent
PLUGINS = ROOT / "plugins"
LOCKS = ROOT / "locks"
created = []


def command(*args, code=0, timeout=15, env=None):
    result = subprocess.run([str(HOST), *args], capture_output=True, text=True, timeout=timeout,
                            env=env)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return result


def install(name, script="return {}", *, wrapped=True, expected=0, files=None, **fields):
    manifest = dict(id=name, **fields)
    prefix = "wrapped/" if wrapped else ""
    with tempfile.NamedTemporaryFile(dir=ROOT, suffix=".zip") as archive:
        with zipfile.ZipFile(archive.name, "w") as package:
            package.writestr(prefix + "plugin.json", json.dumps(manifest))
            package.writestr(prefix + "main.lua", script)
            for path, (content, mode) in (files or {}).items():
                member = zipfile.ZipInfo(prefix + path)
                member.external_attr = (0o100000 | mode) << 16
                package.writestr(member, content)
        command("plugin", "install", archive.name, code=expected)
    if expected == 0 and name not in created:
        created.append(name)
    return PLUGINS / name


def wait_file(path, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return path.read_text()
        time.sleep(0.02)
    raise AssertionError(f"missing completion file: {path}")


def run_api_cases():
    script = r'''
return { action = function()
    assert(get_config("count") == "")
    assert(set_config("count", 3))
    assert(get_config("count") == "3")
    assert(set_config(" 配置: key ", 4))
    assert(get_config(" 配置: key ") == "4")
    assert(set_config(string.rep("a", 64), "boundary"))
    assert(set_config(string.rep("中", 21) .. "a", "boundary"))
    assert(not pcall(set_config, string.rep("中", 22), "x"))
    assert(not pcall(set_config, string.rep("a", 65), "x"))
    assert(not pcall(set_config, "bad/key", "x"))
    assert(not pcall(set_config, "bad\\key", "x"))
    assert(not pcall(set_config, "key", true))
    assert(not pcall(set_config, "key", {}))
    info(123)
    warn(456)
    assert(mkdir(PLUGIN_DIR .. "/one"))
    assert(mkdir(PLUGIN_DIR .. "/one", nil))
    assert(mkdir(PLUGIN_DIR .. "/nested/two", "truthy"))
    assert(mkdir(PLUGIN_DIR .. "/zero/child", 0))
    assert(mkdir(PLUGIN_DIR .. "/table/child", {}))
    assert(not mkdir(PLUGIN_DIR .. "/absent/child", false))
    assert(write_file(PLUGIN_DIR .. "/file", 42))
    assert(read_file(PLUGIN_DIR .. "/file") == "42")
    assert(chmod(PLUGIN_DIR .. "/file", " 0755\n"))
    assert(write_file(PLUGIN_DIR .. "/file", "\255\000binary"))
    assert(read_file(PLUGIN_DIR .. "/file") == "\255\000binary")
    local mode = exec("stat", "-c", "%a", PLUGIN_DIR .. "/file")
    assert(mode.stdout == "755\n")
    assert(json_encode({}) == "{}")
    local mixed = json_decode(json_encode({[1] = "a", extra = "b"}))
    assert(mixed["1"] == "a" and mixed.extra == "b")
    local cyclic = {}; cyclic.self = cyclic
    assert(not pcall(json_encode, cyclic))
    assert(not pcall(json_encode, 0/0))
    assert(not pcall(json_encode, {[1] = "a", ["1"] = "b"}))
    assert(json_decode("[1,null,3]")[3] == 3)
    assert(not pcall(exec, PLUGIN_DIR .. "/missing"))
    assert(write_file(PLUGIN_DIR .. "/no-exec", "#!/bin/sh\nexit 0\n"))
    assert(not pcall(exec, PLUGIN_DIR .. "/no-exec"))
    local exit127 = exec("sh", "-c", "exit 127")
    assert(not exit127.ok and exit127.code == 127)
    assert(exec("sh", "-c", "exit 5").code == 5)
    assert(exec("sh", "-c", "kill -TERM $$").code == 143)
    assert(exec("cat").stdout == "")
    assert(exec("printf", "%s", 99).stdout == "99")
    local pipe = assert(io.popen("pwd"))
    local cwd = pipe:read("*a"); pipe:close()
    assert(exec("pwd").stdout == cwd)
    assert(not pcall(exec, true))
    assert(not pcall(exec, "sh\000ignored"))
    local output = exec("python3", "-c", "import os; os.write(1,b'a'*5000000); os.write(2,b'b'*5000000)")
    assert(output.ok and #output.stdout > 4194304 and #output.stderr > 4194304)
    assert(output.stdout:find("[output truncated]", 1, true))
    assert(output.stderr:find("[output truncated]", 1, true))
    local dual = exec("python3", "-c", [[
import os, threading
def emit(fd, value):
    for _ in range(512): os.write(fd, value * 16384)
writer = threading.Thread(target=emit, args=(1, b'a'))
writer.start()
emit(2, b'b')
writer.join()
]])
    assert(dual.ok and dual.stdout:find("[output truncated]", 1, true))
    assert(dual.stderr:find("[output truncated]", 1, true))
    assert(exec("python3", "-c", "import os; os.write(1,b'\\xff')").stdout == "\255")
    local before = #list_dir("/proc/self/fd")
    for i = 1, 40 do assert(not pcall(exec, PLUGIN_DIR .. "/missing")) end
    assert(#list_dir("/proc/self/fd") == before)
    for _, interval in ipairs({0/0, math.huge, -math.huge, "no", 2^63}) do
        assert(not pcall(start_daemon, "tick", interval))
    end
    assert(write_file(PLUGIN_DIR .. "/config.json", "broken"))
    assert(not set_config("count", "new"))
    assert(not pcall(get_config, "count"))
    assert(read_file(PLUGIN_DIR .. "/config.json") == "broken")
end }
'''
    directory = install("api", script, min_version=4294967295, entry=None,
                        config=[dict(key=" 配置: key ", default=8), dict(key="count", default=99)])
    command("plugin", "run", "api", "action")
    (directory / "config.json").unlink()
    command("plugin", "config", "--id", "api", "set", " key:中文 ", "value")
    assert command("plugin", "config", "--id", "api", "get", " key:中文 ").stdout == "value\n"
    command("plugin", "config", "--id", "api", "delete", " key:中文 ")
    assert command("plugin", "config", "--id", "api", "get", " key:中文 ").stdout == "\n"
    command("plugin", "run", "api", "missing", code=1)
    command("plugin", "disable", "api")
    command("plugin", "enable", "api")
    for value in (-1, 1.5, "123", 4294967296):
        install("bad-manifest", min_version=value, expected=1)
    for key in ("", "bad/key", "bad\\key", "中" * 22):
        install("bad-manifest", config=[dict(key=key)], expected=1)
    assert not (PLUGINS / "bad-manifest").exists()


def run_stage_cases():
    script = '''return {
      service = function() write_file(PLUGIN_DIR .. "/service", "done") end,
      post_mount = function() write_file(PLUGIN_DIR .. "/mounted", "done") end,
      main = function()
        assert(_G.previous_round == nil); _G.previous_round = true
        local n = tonumber(get_config("round")) or 0
        set_config("round", n + 1)
        write_file(PLUGIN_DIR .. "/tick", tostring(n + 1))
      end
    }'''
    legacy = install("legacy", script, min_version=None)
    empty = install("empty", script, callbacks=[])
    declared = install("declared", script, callbacks=["main", "main"])
    no_main = install("no-main", script, callbacks=["service"])
    install("explicit-no-action", has_action=False)
    listing = json.loads(command("plugin", "list").stdout)
    items = {item["id"]: item for item in listing}
    assert items["legacy"]["action_unknown"]
    assert not items["empty"]["action_unknown"]
    assert not items["explicit-no-action"]["action_unknown"]
    assert items["declared"]["callbacks"] == ["main"]
    command("stage", "service")
    assert wait_file(legacy / "tick")
    assert wait_file(declared / "tick")
    assert (empty / "service").exists() and not (empty / "tick").exists()
    assert (no_main / "service").exists() and not (no_main / "tick").exists()
    command("stage", "post-mount")
    assert (empty / "mounted").exists()
    deadline = time.monotonic() + 4
    while int((legacy / "tick").read_text() or "0") < 2 and time.monotonic() < deadline:
        time.sleep(0.05)
    assert int((legacy / "tick").read_text()) >= 2
    for name in ("legacy", "declared"):
        command("plugin", "disable", name)
    probe = install("no-probe", 'write_file(PLUGIN_DIR .. "/evaluated", "yes"); return {}')
    command("plugin", "list")
    assert not (probe / "evaluated").exists()
    command("plugin", "action", "no-probe", code=1)
    assert (probe / "evaluated").exists()
    for name, action in (("string-action", "worker:1"),
                         ("object-action", {"function": "worker:1", "label": "Run"})):
        directory = install(name, 'return {["worker:1"] = function() '
                            'write_file(PLUGIN_DIR .. "/ran", "yes") end}', quick_action=action)
        item = next(x for x in json.loads(command("plugin", "list").stdout) if x["id"] == name)
        assert not item["has_action"] and not item["action_unknown"]
        assert item["quick_action"]["function"] == "worker:1"
        command("plugin", "run", name, "worker:1")
        assert (directory / "ran").exists()
        command("plugin", "action", name, code=1)


def lock_path(name, callback):
    return LOCKS / "daemons" / name.encode().hex() / (callback.encode().hex() + ".lock")


def assert_unlocked(path):
    with path.open("r+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)


def run_option_callback_cases():
    names = ("-tick", "--help", "--ready-fd", "--")
    script = '''local callbacks = {}
    for _, name in ipairs({"-tick", "--help", "--ready-fd", "--"}) do
        callbacks[name] = function()
            assert(write_file(PLUGIN_DIR .. "/" .. name, "called"))
        end
    end
    callbacks.action = function()
        for _, name in ipairs({"-tick", "--help", "--ready-fd", "--"}) do
            assert(start_daemon(name, 9223372036854775807))
        end
    end
    return callbacks'''
    directory = install("option-callbacks", script, quick_action="--help", callbacks=list(names))
    for name in names:
        command("plugin", "run", "--", "option-callbacks", name)
        assert (directory / name).read_text() == "called"
        (directory / name).unlink()
        command("plugin", "daemon", "--", "option-callbacks", name, "9223372036854775807")
        assert wait_file(directory / name) == "called"
    command("plugin", "disable", "option-callbacks")
    for name in names:
        assert_unlocked(lock_path("option-callbacks", name))
        (directory / name).unlink()
    command("plugin", "enable", "option-callbacks")
    command("plugin", "action", "option-callbacks")
    for name in names:
        assert wait_file(directory / name) == "called"
    command("plugin", "disable", "option-callbacks")


def run_daemon_cases():
    script = '''return { ["b.tick"] = function() write_file(PLUGIN_DIR .. "/tick", "ok") end,
      tick = function() write_file(PLUGIN_DIR .. "/tick", "ok") end,
      action = function()
        assert(start_daemon("negative", -2))
        assert(start_daemon("fraction", "1.9"))
        assert(start_daemon("huge", 9223372036854775807))
      end,
      negative = function() end, fraction = function() end, huge = function() end }'''
    a = install("a", script)
    b = install("a.b", script)
    command("plugin", "daemon", "a", "b.tick", "0")
    command("plugin", "daemon", "a.b", "tick", "9223372036854775807")
    wait_file(a / "tick"); wait_file(b / "tick")
    identity = lock_path("a.b", "tick").read_bytes()
    command("plugin", "daemon", "a.b", "tick", "1")
    assert lock_path("a.b", "tick").read_bytes() == identity
    command("plugin", "run", "a", "action")
    command("plugin", "disable", "a")
    assert_unlocked(lock_path("a", "b.tick"))
    with lock_path("a.b", "tick").open("r+b") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            raise AssertionError("stopped the wrong plugin")
    command("plugin", "uninstall", "a.b")
    created.remove("a.b")
    assert_unlocked(lock_path("a.b", "tick"))
    command("plugin", "enable", "a")
    command("plugin", "daemon", "a", "tick", "9223372036854775807")
    install("a", script, wrapped=False, files={"helper": ("#!/bin/sh\nexit 0\n", 0o755)})
    assert_unlocked(lock_path("a", "tick"))
    assert_unlocked(lock_path("a", "negative"))
    assert (a / "helper").stat().st_mode & 0o777 == 0o755
    command("plugin", "daemon", "a", "missing", "1", code=1)
    command("plugin", "daemon", "a", "bad/name", "1", code=1)
    command("plugin", "daemon", "a", "tick", "9223372036854775808", code=2)
    command("plugin", "daemon", "a", "tick", "1", "--ready-fd", "2", code=2)
    command("plugin", "daemon", "a", "tick", "--", "-2")
    command("plugin", "disable", "a")
    slow = install("slow", 'os.execute("sleep 20"); return { tick = function() end }')
    command("plugin", "daemon", "slow", "tick", "1", code=1, timeout=14)
    assert_unlocked(lock_path("slow", "tick"))


def run_legacy_lock_cases():
    directory = install("old.lock", 'return { tick = function() '
                        'write_file(PLUGIN_DIR .. "/ran", "yes") end }')
    path = LOCKS / "old.lock.tick.daemon.lock"
    helper = r'''
import fcntl, os, signal, struct, sys
with open(sys.argv[1], "w+b", buffering=0) as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    lock.write(struct.pack("=IiiB3x", 0x4b504c47, os.getpid(), os.getpgrp(), 1))
    print("ready", flush=True)
    while True: signal.pause()
'''
    process = subprocess.Popen([sys.executable, "-c", helper, str(path)],
                               stdout=subprocess.PIPE, text=True, start_new_session=True)
    try:
        assert process.stdout.readline().strip() == "ready"
        command("plugin", "daemon", "old.lock", "tick", "1")
        assert process.poll() is None and not (directory / "ran").exists()
        install("old.lock", 'return { tick = function() '
                'write_file(PLUGIN_DIR .. "/ran", "updated") end }')
        process.wait(timeout=3)
        assert_unlocked(path)
        process.stdout.close()
        process = subprocess.Popen([sys.executable, "-c", helper, str(path)],
                                   stdout=subprocess.PIPE, text=True, start_new_session=True)
        assert process.stdout.readline().strip() == "ready"
        command("plugin", "disable", "old.lock")
        process.wait(timeout=3)
        assert_unlocked(path)
        command("plugin", "enable", "old.lock")
        command("plugin", "daemon", "old.lock", "tick", "1")
        wait_file(directory / "ran")
        command("plugin", "uninstall", "old.lock")
        created.remove("old.lock")
        assert_unlocked(path)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=3)
        process.stdout.close()


def run_install_regressions():
    directory = install("transaction", 'return {action=function() info("saved log") end}',
                        wrapped=False, files={"tool": ("#!/bin/sh\nexit 0\n", 0o755)})
    command("plugin", "config", "--id", "transaction", "set", "saved", "kept")
    command("plugin", "action", "transaction")
    log = (directory / "last_output.log").read_bytes()
    command("plugin", "disable", "transaction")
    install("transaction", 'return {}', files={"tool": ("new", 0o755)})
    assert command("plugin", "config", "--id", "transaction", "get", "saved").stdout == "kept\n"
    assert (directory / "last_output.log").read_bytes() == log
    assert (directory / "disable").exists()
    assert (directory / "tool").stat().st_mode & 0o777 == 0o755
    for path in ("../escape", "nested/../../escape", "/escape"):
        install("transaction", files={path: ("bad", 0o644)}, expected=1)
        assert (directory / "tool").read_text() == "new"
    install("missing-dependency", depends=["does-not-exist"], expected=1)
    install("dependency")
    install("dependent", depends=["dependency"])
    command("plugin", "disable", "dependent")
    command("plugin", "disable", "dependency")
    command("plugin", "enable", "dependent", code=1)
    command("plugin", "run", "dependent", "action", code=1)


def run_upstream_examples():
    fixtures = Path(__file__).parent / "fixtures"
    for project in ("folkpatch", "folkpatch-cache", "finasu"):
        fixture = fixtures / project
        manifest = json.loads((fixture / "plugin.json").read_text())
        name = manifest["id"]
        with tempfile.NamedTemporaryFile(dir=ROOT, suffix=".zip") as archive:
            with zipfile.ZipFile(archive.name, "w") as package:
                for path in ("main.lua", "plugin.json"):
                    package.write(fixture / path, path)
                package.writestr("data/", "")
            command("plugin", "install", archive.name)
        created.append(name)
        directory = PLUGINS / name
        if project == "folkpatch":
            result = command("plugin", "run", name, "boot_completed", code=1)
            assert "attempt to assign to const variable 'line'" in result.stderr
        elif project == "folkpatch-cache":
            with tempfile.TemporaryDirectory(dir=ROOT) as commands:
                output = directory / "mock-result"
                mock = Path(commands) / "sh"
                mock.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" >> " +
                                shlex.quote(str(output)) + "\nprintf '0\\n'\n")
                mock.chmod(0o755)
                environment = dict(os.environ, PATH=commands + os.pathsep + os.environ["PATH"])
                command("plugin", "action", name, env=environment)
                assert output.read_text().count("rm -rf") == 3
                command("plugin", "config", "--id", name, "set", "clear_all", "true")
                output.unlink()
                command("plugin", "run", name, "clean_now", env=environment)
                assert output.read_text().count("rm -rf") == 4
                command("plugin", "config", "--id", name, "set", "clean_hour",
                        str(time.localtime().tm_hour))
                command("plugin", "daemon", name, "main", "1", env=environment)
                wait_file(directory / "last_run")
                command("plugin", "disable", name)
        else:
            command("plugin", "run", name, "service")
            assert (directory / "data/greeting.txt").read_text() == "Hello, FinaSU!\n"
            command("plugin", "config", "--id", name, "set", "name", "YukiSU")
            command("plugin", "action", name)
            assert (directory / "data/greeting.txt").read_text() == "Hello, YukiSU!\n"


try:
    run_api_cases()
    run_stage_cases()
    run_option_callback_cases()
    run_daemon_cases()
    run_legacy_lock_cases()
    run_install_regressions()
    run_upstream_examples()
    print("Plugin API, manifest, configuration, stage, CLI and daemon compatibility passed")
finally:
    for name in reversed(created):
        subprocess.run([str(HOST), "plugin", "uninstall", name], capture_output=True, timeout=10)
