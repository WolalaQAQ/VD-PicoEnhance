"""Own loopback peer, actual Winsock transport/client. Does not contact a headset."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import threading


def discovery_test(args):
    """Two different loopback addresses stand in for successive headset IPs."""
    args.output.mkdir(parents=True, exist_ok=True)
    stopping = threading.Event()
    valid_sent = threading.Event()
    connected = threading.Event()
    errors, sessions, commands, discoveries = [], [], [], []
    servers, replies = [], []
    first = socket.socket()
    first.bind(('127.0.0.2', 0))
    port = first.getsockname()[1]
    first.listen(1); first.settimeout(.1); servers.append(first)
    for address in ('127.0.0.2', '127.0.0.3'):
        reply = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reply.bind((address, port)); replies.append(reply)
    if args.scenario == 'reconnect':
        second = socket.socket(); second.bind(('127.0.0.3', port))
        second.listen(1); second.settimeout(.1); servers.append(second)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(('127.0.0.1', port)); udp.settimeout(.1)

    def discover():
        try:
            while not stopping.is_set():
                try:
                    request, sender = udp.recvfrom(256)
                except socket.timeout:
                    continue
                assert len(request) == 102 and request[:70] == b'PKD1 ' + b'a'*64 + b' '
                discoveries.append(request)
                if args.scenario == 'stop-discovery':
                    continue
                target = 1 if args.scenario == 'reconnect' and len(sessions) >= 1 else 0
                reply = b'PKA1 ' + request[5:]
                if len(discoveries) == 1 and args.scenario != 'expired-discovery':
                    # Neither another pairing nor a reply to an old query may
                    # start a TCP connection, even when the address is plausible.
                    udp.sendto(reply[:5] + b'c'*64 + reply[69:], sender)
                    nonce = b'0'*32 if request[70:] != b'0'*32 else b'1'*32
                    replies[target].sendto(reply[:70] + nonce, sender)
                    assert not connected.wait(.15), 'invalid discovery response started TCP'
                valid_sent.set()
                replies[target].sendto(reply, sender)
        except Exception as error:
            errors.append(repr(error))

    def serve():
        try:
            for index, server in enumerate(servers):
                connection = None
                while not stopping.is_set() and connection is None:
                    try:
                        connection, _ = server.accept()
                    except socket.timeout:
                        pass
                if connection is None:
                    break
                connected.set()
                assert valid_sent.is_set(), 'TCP connected before the valid paired response'
                assert args.scenario != 'stop-discovery'
                session, previous, count, mode = None, 0, 0, 0
                with connection, connection.makefile('rb') as stream:
                    connection.settimeout(10)
                    while line := stream.readline(161):
                        words = line.decode('ascii').split()
                        assert len(words) == 6 and words[:2] == ['VDM1', 'a'*64] and words[-1] == '3000'
                        assert len(words[2]) == 32 and all(ch in '0123456789abcdef' for ch in words[2])
                        if session is None:
                            session = words[2]; sessions.append(session)
                        assert words[2] == session and int(words[3]) > previous
                        previous = int(words[3]); count += 1
                        command = words[4]
                        assert command in ('status', 'index', 'hands', 'restore')
                        commands.append(command)
                        mode = dict(index=1, hands=2, restore=0).get(command, mode)
                        reply_sequence = previous + 1 if args.scenario == 'bad-ack' and count == 2 else previous
                        outcome = 'error' if args.scenario == 'helper-error' and count == 2 else 'ok'
                        connection.sendall(f'VDR1 {reply_sequence} {mode} {0 if mode == 1 else 1} unobserved {outcome}\n'.encode())
                        if args.scenario == 'reconnect' and index == 0 and count == 2:
                            break
        except Exception as error:
            errors.append(repr(error))

    threads = [threading.Thread(target=discover), threading.Thread(target=serve)]
    for thread in threads: thread.start()
    environment = dict(os.environ)
    if args.driver:
        profile = args.output / 'owned-profile'
        config = profile / 'PicoMixedVR'; config.mkdir(parents=True)
        (config / 'endpoint.txt').write_text(f'127.0.0.1:{port}\n', encoding='ascii')
        (config / 'headset.token').write_text('a'*64 + '\n', encoding='ascii')
        (config / 'phase.txt').write_text('1 auto\n', encoding='ascii')
        environment['LOCALAPPDATA'] = str(profile)
    command = [str(args.fixture), str(args.driver) if args.driver else f'127.0.0.1:{port}', args.scenario]
    try:
        result = subprocess.run(command, env=environment, capture_output=True, timeout=20)
    finally:
        stopping.set()
        for thread in threads: thread.join(11)
        for sock in [udp, *servers, *replies]: sock.close()
    (args.output / 'stdout.log').write_bytes(result.stdout)
    (args.output / 'stderr.log').write_bytes(result.stderr)
    report = dict(passed=result.returncode == 0 and not errors and all(not t.is_alive() for t in threads),
                  scenario=args.scenario, fixture_returncode=result.returncode, errors=errors,
                  discovery_requests=len(discoveries), commands=commands, tcp_sessions=len(sessions),
                  distinct_sessions=len(set(sessions)), loopback_only=True, device_operations=False)
    if args.scenario in ('stop-discovery', 'expired-discovery'):
        report['passed'] &= bool(discoveries) and not sessions and not commands
    elif args.scenario in ('helper-error', 'bad-ack'):
        report['passed'] &= len(sessions) == 1 and len(discoveries) == 1 and commands == ['status', 'status']
    elif args.scenario == 'reconnect':
        report['passed'] &= len(set(sessions)) == 2 and commands[-1:] == ['restore']
    else:
        report['passed'] &= len(sessions) == 1 and bool(discoveries) and commands[-1:] == ['restore']
    if not args.driver and args.scenario != 'expired-discovery':
        report['passed'] &= commands == ['status', 'index', 'hands', 'restore']
    (args.output / 'discovery-results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    assert report['passed'], report

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('fixture', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--driver', type=Path)
    p.add_argument('--scenario', choices=('legacy', 'discovery', 'reconnect', 'stop-discovery',
                                        'expired-discovery', 'helper-error', 'bad-ack'), default='legacy')
    args = p.parse_args()
    if args.scenario != 'legacy':
        return discovery_test(args)
    commands, errors = [], []
    server = socket.socket()
    server.bind(('127.0.0.1', 0))
    server.listen(1)
    port = server.getsockname()[1]
    def peer():
        try:
            server.settimeout(10)
            connection, _ = server.accept()
            with connection, connection.makefile('rb') as stream:
                connection.settimeout(5)
                mode, previous, session = 0, 0, None
                while line := stream.readline(161):
                    words = line.decode('ascii').split()
                    assert len(words) == 6 and words[0] == 'VDM1' and words[1] == 'a' * 64 and words[-1] == '3000'
                    assert len(words[2]) == 32 and all(ch in '0123456789abcdef' for ch in words[2])
                    assert session is None or session == words[2]
                    session = words[2]
                    sequence = int(words[3])
                    assert sequence > previous
                    previous = sequence
                    command = words[4]
                    assert command in ('index', 'hands', 'restore', 'status')
                    commands.append(command)
                    mode = dict(index=1, hands=2, restore=0).get(command, mode)
                    connection.sendall(f'VDR1 {sequence} {mode} {0 if mode == 1 else 1} unobserved ok\n'.encode())
        except Exception as error:
            errors.append(repr(error))
        finally:
            server.close()
    thread = threading.Thread(target=peer)
    thread.start()
    environment = dict(os.environ)
    if args.driver:
        profile = args.output / 'owned-profile'
        config = profile / 'PicoMixedVR'
        config.mkdir(parents=True)
        (config / 'endpoint.txt').write_text(f'127.0.0.1:{port}\n', encoding='ascii')
        (config / 'headset.token').write_text('a' * 64 + '\n', encoding='ascii')
        (config / 'phase.txt').write_text('1 auto\n', encoding='ascii')
        environment['LOCALAPPDATA'] = str(profile)
    result = subprocess.run([str(args.fixture), str(args.driver) if args.driver else f'127.0.0.1:{port}'],
                            env=environment, capture_output=True, timeout=15)
    thread.join(10)
    args.output.mkdir(exist_ok=True)
    (args.output / 'lan.stdout.log').write_bytes(result.stdout)
    (args.output / 'lan.stderr.log').write_bytes(result.stderr)
    assert result.returncode == 0 and not errors and not thread.is_alive(), (result.returncode, errors)
    # A confirmed Restore already discharged ownership; shutdown need not send
    # another Restore before closing this connection.
    if args.driver:
        assert commands.count('status') >= 3 and commands[-1] == 'restore' and not {'index', 'hands'} & set(commands), commands
    else:
        assert commands == ['status', 'index', 'hands', 'restore'], commands
    report = dict(passed=True, actual_transport=json.loads(result.stdout.decode()),
        commands=commands, protocol_session_and_sequence_verified=True,
        real_headset_backend=False, lan_device_tested=False, device_operations=False)
    (args.output / ('provider-results.json' if args.driver else 'lan-results.json')).write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))

if __name__ == '__main__': main()
