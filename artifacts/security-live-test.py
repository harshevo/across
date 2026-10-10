import json
import os
from pathlib import Path
import subprocess
import time

checks = []

def run(*args, ok=True):
    result = subprocess.run(args, text=True, capture_output=True, timeout=90)
    if ok and result.returncode:
        raise RuntimeError(f'{args}: {result.stdout} {result.stderr}')
    return result

def action(name, *args):
    return run('across-server', 'security-action', '--action', name, *args)

def passed(name):
    checks.append(name)
    print('PASS ' + name, flush=True)

def firewall(name):
    return run('across-server', 'firewall', '--action', name, '--port', '22443', '--protocol', 'tcp', '--ssh-port', '22')

def get_jail(name):
    return run('fail2ban-client', 'get', 'sshd', name).stdout.strip()

ip = '203.0.113.99'
try:
    assert get_jail('maxretry') in ['5', '6'], 'Retry limit unavailable'
    passed('Native Fail2ban retry limit update')
    action('fail2ban-limits', '--maxretry', '5', '--findtime', '600', '--bantime', '3600', '--ssh-port', '22')
    assert get_jail('maxretry') == '5' and get_jail('findtime') == '600' and get_jail('bantime') == '3600'
    passed('Fail2ban limits restored and active')
    action('ban-ip', '--ip', ip)
    assert ip in run('fail2ban-client', 'status', 'sshd').stdout
    passed('Fail2ban IP ban')
finally:
    action('unban-ip', '--ip', ip)
assert ip not in run('fail2ban-client', 'status', 'sshd').stdout
passed('Fail2ban IP unban')

try:
    firewall('allow')
    assert '22443/tcp' in run('ufw', 'show', 'added').stdout
    passed('Add port allow rule')
    firewall('deny')
    assert 'deny 22443/tcp' in run('ufw', 'show', 'added').stdout
    passed('Add port deny rule')
    action('block-ip', '--ip', ip)
    assert ip in run('ufw', 'show', 'added').stdout
    passed('Server IP block')
finally:
    firewall('delete')
    firewall('unblock')
    action('unblock-ip', '--ip', ip)
rules = run('ufw', 'show', 'added').stdout
assert '22443' not in rules and ip not in rules
passed('Remove port rules and unblock server IP')
assert run('across-server', 'firewall', '--action', 'deny', '--port', '22', '--protocol', 'tcp', '--ssh-port', '22', ok=False).returncode != 0
passed('Active SSH port block rejected')

def site_request():
    return run('curl', '--silent', '--noproxy', '*', '--interface', '127.0.0.2', '--resolve', 'tradelyzer.harshevo.dev:443:127.0.0.1', '-o', '/dev/null', '-w', '%{http_code}', 'https://tradelyzer.harshevo.dev/').stdout

def expect_site_status(expected):
    for attempt in range(20):
        status = site_request()
        if status == expected:
            return
        time.sleep(0.1)
    raise AssertionError(f'Expected {expected}, got {status}')

try:
    action('website-block-ip', '--app', 'tradelyzer', '--ip', '127.0.0.2')
    expect_site_status('403')
    passed('Website visitor block returns HTTP 403')
finally:
    action('website-unblock-ip', '--app', 'tradelyzer', '--ip', '127.0.0.2')
expect_site_status('200')
passed('Website visitor unblock returns HTTP 200')

before = [Path('/proc/sys/net/ipv4/icmp_echo_ignore_all').read_text().strip(), Path('/proc/sys/net/ipv6/icmp/echo_ignore_all').read_text().strip()]
assert before == ['0', '0']
try:
    action('ping', '--mode', 'block')
    assert Path('/proc/sys/net/ipv4/icmp_echo_ignore_all').read_text().strip() == '1'
    assert Path('/proc/sys/net/ipv6/icmp/echo_ignore_all').read_text().strip() == '1'
    passed('Disable IPv4 and IPv6 ping')
finally:
    action('ping', '--mode', 'allow')
assert Path('/proc/sys/net/ipv4/icmp_echo_ignore_all').read_text().strip() == '0'
passed('Restore ping policy')

ssh_config = Path('/etc/ssh/sshd_config.d/00-across-keys-only.conf')
previous = ssh_config.read_bytes() if ssh_config.exists() else None
try:
    action('ssh-keys-only')
    effective = run('sshd', '-T').stdout
    assert 'passwordauthentication no' in effective and 'pubkeyauthentication yes' in effective
    passed('SSH keys-only validated and reloaded')
finally:
    if previous is None:
        ssh_config.unlink(missing_ok=True)
    else:
        ssh_config.write_bytes(previous)
    run('sshd', '-t')
    run('systemctl', 'reload', 'ssh')
passed('Original SSH settings restored')

for signal in ['TERM', 'KILL']:
    child = subprocess.Popen(['/bin/sleep', '600'])
    try:
        stat = Path(f'/proc/{child.pid}/stat').read_text().rsplit(')', 1)[1].split()
        action('kill-process', '--pid', str(child.pid), '--start', stat[19], '--signal', signal)
        assert child.wait(timeout=5) < 0
        passed('Test process ' + signal)
    finally:
        if child.poll() is None:
            child.terminate()
            child.wait(timeout=5)
assert run('across-server', 'security-action', '--action', 'kill-process', '--pid', '1', '--start', '0', '--signal', 'KILL', ok=False).returncode != 0
passed('PID 1 termination rejected')

snapshot = json.loads(run('across-server', 'security', '--json').stdout)
assert snapshot['users'] and snapshot['processes'] and snapshot['ssh_activity']['available'] and snapshot['ports']['available']
Path('/tmp/across-security-live-evidence.json').write_text(json.dumps({'checks': checks, 'security': snapshot}, indent=2))
passed('SSH, user, process, port and certificate snapshot')
print(f'{len(checks)} live checks passed', flush=True)
