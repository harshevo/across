# Across live validation

Validated the rebuilt native Across app and installed Linux backend on digitalocean, 178.128.144.213.

- All three CTest suites passed, including desktop persistence, editing conflict detection, monitoring counter reset handling, input validation, and traffic aggregation.
- Nineteen server security checks passed in security-live-test.py. Two additional external TCP checks verified that port 22443 became reachable when allowed and unreachable when denied. Temporary rules and test listeners were removed.
- Native app checks passed for Fail2ban installation, retry limit changes, process termination, website restart, logging activation, traffic refresh, export, auto-refresh, restart recovery, and Refresh all.
- Browser visits appeared under project and page traffic. An untrusted CF-Connecting-IP header was ignored.
- Native folder selection, local and VPS editing, upload, and download passed. Automated native drag-and-drop did not trigger a transfer; its manual behavior remains unverified.
- Public and origin domain HTTPS returned 200. Bare IP HTTP returned 403. Earlier checks confirmed bare IP HTTPS handshake rejection and unknown-host rejection.

Final server state: domain-only enabled; Let’s Encrypt renewal timer active; UFW enabled with ports 22, 80, and 443 allowed; Fail2ban sshd jail active with maxretry 5, findtime 600, bantime 3600; traffic logging enabled. Temporary test bans were removed. Existing real attack bans remain. Ping and SSH authentication settings were restored after their tests.

Traffic reports cover current and previous daily origin logs with an 8 MiB scan cap per file. Cloudflare cache hits are excluded. Browser and operating-system labels are inferred from client-reported User-Agent values. Ping settings affect the VPS, not Cloudflare edge addresses.
