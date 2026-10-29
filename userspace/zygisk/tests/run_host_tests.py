import os
from pathlib import Path
import subprocess
import tempfile


def run(*args):
    subprocess.run(args, check=True)


if os.geteuid() != 0:
    raise SystemExit('Run these filesystem tests as root in a disposable Linux container.')

root = Path(__file__).resolve().parents[3]
compiler = os.environ.get('CXX', 'g++')
flags = ['-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-I' + str(root), '-I' + str(root / 'kernel')]
daemon = (root / 'userspace/zygisk/daemon/zygiskd.cpp').read_text()
begin = daemon.index('bool nl_receive_one(int fd) {')
end = daemon.index('uint64_t resolve_linker_sym(', begin)
netlink = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <vector>
#include "uapi/yukizygisk.h"

struct Packet { std::vector<char> data; uint32_t sender = 0; int error = 0; };
static std::deque<Packet> packets;
static std::vector<uint32_t> generations;
static ssize_t fake_recvfrom(int, void *buffer, size_t capacity, int flags,
                             sockaddr *address, socklen_t *length) {
    assert((flags & MSG_DONTWAIT) != 0);
    if (packets.empty()) { errno = EAGAIN; return -1; }
    const auto packet = packets.front();
    packets.pop_front();
    if (packet.error) { errno = packet.error; return -1; }
    assert(packet.data.size() <= capacity && *length >= sizeof(sockaddr_nl));
    auto *sender = reinterpret_cast<sockaddr_nl *>(address);
    sender->nl_family = AF_NETLINK;
    sender->nl_pid = packet.sender;
    *length = sizeof(*sender);
    memcpy(buffer, packet.data.data(), packet.data.size());
    return static_cast<ssize_t>(packet.data.size());
}
static void enqueue(uint32_t type, uint32_t generation, uint32_t sender = 0) {
    Packet packet;
    packet.sender = sender;
    packet.data.resize(NLMSG_SPACE(sizeof(yz_zygote_exit_event)));
    nlmsghdr header{};
    header.nlmsg_type = YZ_NL_MSG_EVENT;
    header.nlmsg_len = NLMSG_LENGTH(sizeof(yz_zygote_exit_event));
    yz_zygote_exit_event event{};
    event.event.type = type;
    event.generation = generation;
    memcpy(packet.data.data(), &header, sizeof(header));
    memcpy(packet.data.data() + NLMSG_HDRLEN, &event, sizeof(event));
    packets.push_back(packet);
}
struct Monitor {
    void on_exit(const yz_zygote_exit_event &event) { generations.push_back(event.generation); }
} g_crash_monitor;
struct History { bool enabled() const { return false; } } g_exit_history;
static std::vector<yz_target_exit_event> g_legacy_native_exits;
static void flush_legacy_native_exits() { g_legacy_native_exits.clear(); }
static void read_yzconfig() {}
static bool rescan_modules_for_reload() { return true; }
namespace yzpolicy {
static bool refresh(bool) { return true; }
static void handle_refresh_request(uint32_t) {}
}
#define DLOGI(...) ((void)0)
#define recvfrom fake_recvfrom
''' + daemon[begin:end] + r'''
int main() {
    enqueue(YZ_EV_SPECIALIZE, 0);
    enqueue(YZ_EV_ZYGOTE_EXIT, 99, 1234);
    enqueue(YZ_EV_ZYGOTE_EXIT, 7);
    packets.push_back({{}, 0, EINTR});
    enqueue(YZ_EV_ZYGOTE_EXIT, 8);
    nl_drain(1);
    assert(packets.empty());
    assert((generations == std::vector<uint32_t>{7, 8}));
    nl_drain(1);
    std::cout << "PASS: kernel event backlog, sender validation and interrupted receive\n";
}
'''
with tempfile.TemporaryDirectory(prefix='yz-host-tests-') as temporary:
    build = Path(temporary)
    replay = build / 'crash_protection_test'
    run(compiler, *flags, str(root / 'userspace/zygisk/tests/crash_protection_test.cpp'),
        str(root / 'userspace/zygisk/daemon/crash_monitor.cpp'),
        '-Wl,--wrap=flock', '-Wl,--wrap=fsync', '-o', str(replay))
    run(str(replay))
    fixture = os.environ.get('YZ_TOMBSTONE_FIXTURE')
    if fixture:
        run(str(replay), fixture)
    source = build / 'netlink_test.cpp'
    source.write_text(netlink)
    binary = build / 'netlink_test'
    run(compiler, *flags, str(source), '-o', str(binary))
    run(str(binary))
    endpoint = root / 'userspace/zygisk/endpoint.cpp'
    binary = build / 'endpoint_test'
    run(compiler, *flags, '-pthread',
        str(root / 'userspace/zygisk/tests/endpoint_test.cpp'), str(endpoint),
        '-Wl,--wrap=connect', '-o', str(binary))
    run(str(binary))
    load_config = root / 'userspace/zygisk/tests/load_config_test.cpp'
    if load_config.is_file():
        binary = build / 'load_config_test'
        run(compiler, *flags, '-pthread', str(load_config), str(endpoint), '-o', str(binary))
        run(str(binary))
