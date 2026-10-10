# Across architecture and security model

## Components

1. `across-desktop` is the Raylib interface. It manages multiple SSH profiles,
   provisioning, deployments, rollback/removal, remote commands, transfers, and
   file watching without opening a terminal. File sharing runs in a separate
   two-pane Across window: local directory enumeration stays on the desktop,
   while server directory enumeration and transfers use the selected SSH
   profile.
2. `across` exposes the same local engine for automation and troubleshooting.
3. `across-server` is invoked through SSH for privileged, deterministic server
   operations. It has no listening port or long-lived network API.
4. OpenSSH supplies authentication, encryption, host-key verification, agent
   support, and connection liveness. rsync over that SSH channel supplies
   delta-aware deployment transfer; SCP and SFTP are also available for file copies.

This keeps the externally exposed attack surface at the server's existing SSH
port. There is no proprietary authentication protocol and no deployment token.

## Trust boundaries

- A saved server contains an identifier, display name, host, SSH port, user, and
  optional key *path*. It never contains a password or private-key bytes.
- New host keys use trust-on-first-use in an Across-owned `known_hosts` file.
  A changed key is refused. A future release should add an out-of-band
  fingerprint confirmation screen for high-assurance first connections.
- The SSH principal is trusted to request deployment. Passwordless `sudo` is
  required only for the small remote CLI; application install/build/start
  commands run as the unprivileged system account `across`.
- Local `.env`, `*.pem`, and `*.key` files are excluded. Runtime secrets are not
  part of a release artifact.
- Server/app/domain identifiers, remote paths, upstreams, ports, health paths,
  artifact paths, and release identifiers are validated before use.
- Local commands use argument vectors rather than a shell. The SSH boundary is
  encoded with single-quote escaping. User-configured build/start commands are
  intentionally shell commands but execute without root privileges.

## Deployment transaction

1. Validate the local profile and manifest.
2. Create a private `/tmp/across-upload-<app>-<release>` staging directory.
3. rsync the project through SSH with default and project-specific exclusions.
4. Lock `/var/lock/across/<app>.lock` to prevent overlapping mutations.
5. Copy staged files into `/var/lib/across/apps/<app>/releases/<release>` using
   safe-link handling and ownership for the `across` account.
6. Install dependencies and build as `across`.
7. Atomically repoint `current` to the new release.
8. Start/reload Node processes with PM2, or configure a static/proxy site.
9. Generate an isolated `/etc/nginx/conf.d/across-<app>.conf`, run `nginx -t`,
   and reload only if validation succeeds.
10. Poll the declared health endpoint. On failure, restore the prior release and
    restart it. On success, save PM2 state and prune only releases beyond the
    retention limit.

## Reliability properties

- Upload/build directories are never served directly.
- Release switching is atomic on a single filesystem.
- Nginx's complete configuration is checked before every reload.
- Domain collisions with existing Nginx sites and new-app port collisions are
  rejected before a release is accepted.
- Health checks are bounded and failed releases do not become successful state.
- Per-application file locks serialize deploy, rollback, removal, and settings.
- A global Nginx lock serializes validation/reload transactions across websites.
- Structured website settings are stored separately and applied to subsequent releases.
- Certbot uses a systemd renewal timer and validated Nginx reload hook.
- PM2 state is saved for systemd resurrection after restart.
- PM2 runs under systemd with a minimal environment; sudo/session variables are
  not inherited by deployed applications.
- Existing Nginx sites are not rewritten; Across owns only `across-*.conf`.
- Remote operations have connection keepalives and bounded timeouts.

## Provider portability

Across talks to the guest operating system, not a cloud control plane. The same
transport and deployment protocol therefore works across VM providers. Provider
tasks—creating instances, cloud security groups, static IP allocation, and IAM—
remain outside the guest. A local Cloudflare adapter creates/updates A/AAAA DNS
records. Host firewall rules use UFW; guest service resource limits use systemd.
