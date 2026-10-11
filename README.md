# Across

Across is a terminal-free deployment manager for Linux virtual machines. It has:

- a Raylib desktop application (`across-desktop`),
- a local automation CLI (`across`), and
- a small remote CLI (`across-server`) installed over SSH.

It is cloud-provider neutral. A Lightsail, EC2, GCP, Azure, DigitalOcean, or
on-premises VM works the same way when it runs a supported Linux distribution,
is reachable with OpenSSH, and the SSH account has passwordless `sudo` (or is
root). Ubuntu and Debian are detected automatically and use `apt`.

Across's desktop, deployment client, and server backend are written in C++20.
They orchestrate OpenSSH, rsync, Nginx, PM2, Certbot, UFW, and systemd on the
host. The backend runs on demand over SSH; no extra public API port is needed.

The desktop has dedicated Projects, Websites, Domains, Files, Pipelines,
Resources, Firewall, Packages, Connection, Security, and Monitoring screens.
Deploy Node.js, static, and reverse proxy websites, manage multiple VPS profiles and websites, change Nginx settings
or edit its config, point Cloudflare DNS, issue and renew certificates, inspect
VPS resources, set service limits, and manage host firewall rules.

Across restores the selected server, page, and recent local project manifests
when reopened. Selecting a server automatically reloads its deployed websites,
domain and HTTPS settings, resources, and package versions. **Refresh all** in
the header repeats these live checks; partial failures appear in Activity.
The Projects screen's **Recent projects** menu is saved separately for each server.

## Contents

- [Install and launch the desktop](#install-and-launch-the-desktop)
- [Prepare the VPS and permissions](#prepare-the-vps-and-permissions)
- [Connect and install the server backend](#connect-and-install-the-server-backend)
- [Deploy your first website](#deploy-your-first-website)
- [App navigation](#app-navigation)
- [VPS packages](#vps-packages)
- [Files and folders](#desktop-file-and-folder-sharing)
- [Project manifest and build location](#project-manifest)
- [Domains, HTTPS, and website administration](#website-administration)
- [Security and monitoring](#security-and-monitoring)
- [Persistent storage and backups](#persistent-storage-and-backups)
- [Troubleshooting](#troubleshooting)
- [CLI operations](#operations)
- [CI/CD](#cicd)

## Install and launch the desktop

Run these commands on your **local machine**, from the Across source directory.
On macOS, install the Xcode Command Line Tools if a C++ compiler is not available
(`xcode-select --install`), and make sure CMake and rsync are on your PATH.
The first build needs Internet access to fetch dependencies.

Requirements: CMake 3.20+, a C++20 compiler, OpenSSH, and rsync. Raylib and the
JSON library are fetched at pinned versions and verified by SHA-256.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Launch the desktop application:

```sh
open ./build/Across.app
```

On Linux, launch `./build/across-desktop` instead. The native folder picker is
currently implemented on macOS; Linux builds use the available path fields.
You can copy `build/Across.app` into `/Applications` on macOS. Updating the bundle
preserves the settings stored in your user profile.

The macOS bundle includes the remote CLI source needed for server bootstrap, so
the `.app` can be moved independently of the source checkout. The desktop UI
bundles Inter Regular and Semibold rather than relying on Raylib's default font
or a system-installed typeface. Inter's license is included at
[assets/fonts/INTER-LICENSE.txt](assets/fonts/INTER-LICENSE.txt).

## Prepare the VPS and permissions

Use an Ubuntu or Debian VPS with systemd, OpenSSH, and outbound Internet access.
Ubuntu 22.04+ or Debian 12+ provides the recommended C++20 build toolchain.
Bootstrap installs its build tools automatically; you do not need to compile
Across manually on the VPS. Containers without systemd and other Linux
distributions are not supported by the automatic setup flow.

### Network access

Configure the provider's firewall/security group before connecting:

| Port | Who needs access | Purpose |
|---|---|---|
| SSH port, usually TCP 22 | Your administrator IP and any CI deployment runner | App connection, setup, and file transfers |
| TCP 80 | Public Internet for a public website | HTTP and Let's Encrypt validation |
| TCP 443 | Public Internet for a public website | HTTPS |

Across's **Firewall** tab manages UFW on the VPS. It cannot change your cloud
provider's firewall. No Across API port is required. Node application ports,
such as 4100, should remain private; Nginx forwards website traffic to them.
Bind the application to `127.0.0.1` where its framework supports this.
If DNS has an AAAA record, IPv6 must also reach the correct VPS and allow 80/443.

### SSH account

Use an existing administrator account such as `ubuntu`, `debian`, or `root`, or
create a dedicated account such as `across-admin`. Across authenticates with an
SSH private key or your SSH agent. Password prompts and interactive sudo prompts
are not supported.

For a new account, run the following in an existing administrator session on the
**VPS**. Replace the public-key placeholder with your actual public key. Keep
your existing session open until the new login has been tested.

```sh
sudo adduser --disabled-password --gecos '' across-admin
sudo install -d -m 0700 -o across-admin -g across-admin /home/across-admin/.ssh
printf '%s\n' 'YOUR_SSH_PUBLIC_KEY' | sudo tee /home/across-admin/.ssh/authorized_keys > /dev/null
sudo chown across-admin:across-admin /home/across-admin/.ssh/authorized_keys
sudo chmod 0600 /home/across-admin/.ssh/authorized_keys
```

Across needs administrator privileges to install packages, write Nginx and
systemd configuration, issue certificates, and manage the firewall. For a
non-root account, its current automatic setup requires **passwordless sudo**.
Run `sudo visudo -f /etc/sudoers.d/across-admin` and add:

```sudoers
across-admin ALL=(ALL:ALL) NOPASSWD: ALL
```

Then validate and secure the file:

```sh
sudo chmod 0440 /etc/sudoers.d/across-admin
sudo visudo -cf /etc/sudoers.d/across-admin
```

This grants full administrator access to anyone holding that account's SSH key.
Use a dedicated key and protect it. The current installer invokes multiple
privileged tools and shell scripts; a sudo rule allowing only `across-server`
is insufficient for bootstrap. A root SSH account does not need this sudo rule.
The separate `across` service account is created automatically for builds and
Node processes; it has no login shell and should not be used as the SSH account.

On your **local machine**, protect the private key and test the account:

```sh
chmod 0600 /path/to/private-key
ssh -i /path/to/private-key -p 22 across-admin@203.0.113.10
```

Check the host fingerprint against your provider's console when accepting the
first SSH connection. Inside that new VPS session, verify:

```sh
id
sudo -n true
```

`sudo -n true` must succeed without a password for a non-root account.
Do not make project or server directories world-writable with `chmod 777`.

### Files, application permissions, and secrets

- Local project files must be readable by the user running Across. Saving a
  manifest or editing a local file also requires write permission there.
- Uploads, downloads, and VPS file edits use the **SSH account's filesystem
  permissions**, without automatic sudo. A successful server connection does
  not make every file writable in the Files tab.
- Deployment setup manages `/var/lib/across` ownership itself. Application
  installs, builds, and Node processes run as the unprivileged `across` user;
  Nginx and system administration remain privileged operations.
- `.env`, `.env.*`, private keys, and Git metadata are excluded from deployments.
  Across does not provide a secrets editor or automatically load an external
  environment file. If your app needs secrets, configure an explicit server-side
  loading mechanism in its start command or application, with files readable
  only by the required service account. Do not put secrets in `across.json`.

## Connect and install the server backend

In the desktop app:

1. Click **New server** and enter a profile name, VPS address, SSH username,
   SSH port, and private-key path. Leave the key path empty to use your SSH agent.
2. Choose rsync, SCP, or SFTP for Files transfers and save the profile.
   Deployment and backend setup still require rsync on both machines.
3. In **Packages**, select the required software. A Node website normally needs
   Nginx, Node.js, npm, and PM2; a static website needs Nginx.
4. Open **Connection** and click **Install backend**. Across connects over SSH,
   installs build prerequisites, uploads its bundled server source, builds the
   backend, and installs `/usr/local/bin/across-server` and selected packages.
5. Check **Activity** for success or errors. Use the Connection checks and
   **Refresh all** to load the live server inventory.

Initial setup may take several minutes. It is an installation step for each VPS,
not a command you need to run before every deployment. Run **Install backend**
again after upgrading Across when new server commands are needed.

Equivalent commands, run on your **local machine**:

```sh
./build/across server add production \
  --host 203.0.113.10 --user across-admin --port 22 \
  --identity /path/to/private-key
./build/across bootstrap production
./build/across doctor production
```

Across never stores passwords or private-key contents. Profiles store the path
to the optional private key. Its own known-hosts file accepts a new host key on
first use and rejects later changes. The initial terminal SSH check above lets
you verify the host independently; it does not populate Across's known-hosts file.

## Deploy your first website

1. Select your server and open **Projects**.
2. Click **Select folder** and choose your local project directory. Across fills
   its path and loads `across.json` if it exists. For a new project, enter the
   settings and click **Save** to create that file. Choosing the folder alone
   does not create a manifest.
3. Choose a unique application ID and website type: Node, static, or proxy.
   For Node, configure install/build/start commands, an unused application port
   between 1024 and 65535, and a working health-check path. For example, use port
   4100 and `/` if your application has no `/health` endpoint.
4. Add the actual domain names and choose **Build on: VPS** or **Local machine**.
   Save the manifest, then click **Deploy**. Watch Activity for build output and
   the final health-check result.
5. Open **Websites** and select the deployed app. Confirm its hostnames and
   application settings. Point your DNS A record to the VPS IPv4 address using
   your DNS provider or the Cloudflare controls in **Domains**.
6. Allow TCP 80 and 443 in both the provider firewall and UFW. Once every
   configured hostname resolves and reaches this VPS, open **Domains**, provide
   your certificate-account email, and click **Add HTTPS**. Issuance accepts
   Let's Encrypt's subscriber terms. Open the domain using HTTPS to verify it.
7. In Domains, optionally use **Block IP access** to reject the default IP page
   for the selected server. This applies to all its websites; see the details
   under [website administration](#website-administration).
8. Open **Monitoring**, select the project, enable website logging, and refresh
   after making a few website requests. Traffic history begins when logging is
   enabled.

For a CLI deployment after saving the manifest:

```sh
./build/across deploy production /path/to/project/across.json
```

Your start command must keep the server process running, listen on the configured
port, and return a successful response at the health-check path. For VPS builds,
the required runtime and build tools must be installed on the VPS. For local
builds, they must also be installed locally. Node versions supplied by the
operating system may differ from your application's required version; check
**Packages** before deploying.

## App navigation

| Tab or control | What to do there |
|---|---|
| Server sidebar | Switch VPS profiles; each has its own connection settings and recent projects |
| Projects | Select a folder, save its manifest, deploy, or watch local changes |
| Websites | Select deployed apps and manage their Nginx settings |
| Domains | Configure DNS, issue/renew HTTPS, and control default IP access |
| Files | Browse local/VPS folders, transfer files, and edit text files |
| Pipelines | Connect GitHub, choose repositories, add environment keys, and publish VPS deployment workflows |
| Resources | Read VPS health and configure service CPU/memory limits |
| Firewall | View and manage UFW ports and rules |
| Connection | Edit SSH details, check connectivity, and install/update the backend |
| Packages | Select server packages, install them, and inspect versions |
| Security | Inspect SSH activity, accounts, processes, and server protection settings |
| Monitoring | Inspect traffic, visitors, pages, response times, and host metrics |
| Refresh all | Reload the selected server's inventory and settings |
| Activity | See operation output and individual refresh failures |

Across restores the selected profile, tab, and recent project paths when reopened,
then fetches the server's live data. Local project source is not downloaded from
the server automatically. Use **Recent projects** to reopen a saved manifest;
use **Refresh all** if deployed websites or domain settings appear out of date.
Security and Monitoring have their own refresh controls.

## VPS packages

The **Packages** tab saves selections per server. Select Nginx, PM2, Node.js,
npm, or Docker, and add other Ubuntu/Debian package names with **Add**. Use
**Install selected** to apply the selection to an existing server and **Refresh**
to inspect installed versions. Backend setup also installs the saved selection.
Deselecting skips installation; it does not uninstall software.

Node.js maps to `nodejs`, Docker to `docker.io`, and PM2 installs through npm
with Node.js/npm dependencies. Versions come from the VPS distribution's
repositories. Build and transfer prerequisites are installed automatically
before uploading the backend. Ubuntu 22.04+ and Debian 12+ are recommended for
the C++20 toolchain. Unsupported distributions fail before package installation.
For servers with an older Across backend, run **Install backend** once to enable
the package commands.

## Desktop file and folder sharing

Select a server and open the **Files** tab. Across shows the local project
directory on the left and the server directory on
the right. No local or remote path needs to be typed.

- Double-click folders to navigate and use **Up** to return to their parent.
- Select a local item and click **Upload selected**, or select a server item and
  click **Download selected**.
- Drag a file or folder from one pane to the other. Dropping onto a folder sends
  the item directly into that folder.
- Files and folders dragged from Finder or another desktop file manager can be
  dropped directly onto the server pane.
- Select a text file and use **Edit local file** or **Edit VPS file**, then
  **Save file**. Text files are limited to 64 KiB. Saving keeps a timestamped
  backup and rejects a file changed since it was opened.

Transfers use rsync, SCP, or SFTP over the existing authenticated SSH connection.
Choose the protocol in Connection and save the server profile. Deployment
synchronization always uses rsync to preserve exclusions and delta transfer.
The browser also creates local and remote folders, renames entries, and archives items
as timestamped `.across-trash-*` entries in the same directory. Rename an
archived item to restore it. These operations use the SSH account permissions. Names unsafe for
a remote path are normalized, duplicate names in the same drop receive a numeric
suffix, and no shell command is created from a local filename. Dropping an
`across.json` file, or a project folder containing one, onto the main window's
manifest field still selects it for deployment.

## Project manifest

In Projects, click **Select folder** to choose the project using the native
macOS folder picker. Across fills the path and loads an existing `across.json`.
For a new project, fill its settings and click **Save** to create `across.json`
inside that folder. Selecting a folder alone does not write a manifest.
For CLI setup, copy [examples/node.across.json](examples/node.across.json) and
adjust its values. `project_root` is resolved relative to the manifest.

- `type`: `node`, `static`, or `proxy`.
- `domains`: the main hostname and any subdomains served by the app.
- `upstreams`: one or more `host:port` targets; multiple targets create an
  Nginx least-connections load balancer.
- `rate_limit_rps` and `rate_limit_burst`: per-client request limiting; set RPS
  to `0` to disable it.
- `tls_certificate` and `tls_certificate_key`: existing certificate paths on
  the server. Set `redirect_http_to_https` after both paths are valid.
- `.env`, private keys, Git data, dependencies, and build directories are
  excluded from transfer by default. Put secrets on the server using a secrets
  manager or a separately permissioned environment file.

Static applications should set `install_command` or `build_command` to an empty
string when that step is not needed. Proxy applications require `upstreams` and
do not run build or PM2 steps.

## Choose where applications build

Projects has a **Build on** selector for **Local machine** or **VPS**. Load a
manifest, choose the location, and Save before Deploy or Watch. Existing
manifests default to VPS builds.

With `"build_location": "local"`, Across runs `install_command` and
`build_command` in the local project, then uploads `deployment_dir` (relative to
`project_root`). The VPS skips dependency installation and application building.
A failed local command aborts before connecting to the server. Runtime bundles
include their dependencies and build output; secrets and Git files remain excluded.

For Next.js, enable `output: "standalone"` and use
`"deployment_dir": ".next/standalone"`. The local build command must also copy
`public` and `.next/static` into that bundle, and the start command should run
`HOSTNAME=127.0.0.1 node server.js`. Include native runtime libraries matching the
VPS OS/architecture when building on a different platform. The VPS still needs a
compatible Node runtime. With `"build_location": "vps"`, the usual source upload,
remote install, and remote build run as before; `deployment_dir` is unused.

## Operations

```sh
./build/across watch production ./across.json
./build/across filewatch /path/to/project
./build/across apps production
./build/across rollback production my-app
./build/across exec production -- uname -a
./build/across upload production ./asset.zip /tmp/asset.zip
./build/across download production /var/log/nginx/error.log ./error.log
./build/across remove-app production my-app --yes
```

Each deployment is uploaded into a private staging directory, copied without
unsafe symlinks, built as the unprivileged `across` user, and stored as a
versioned release. The `current` symlink changes atomically. Nginx configuration
is validated before reload, health checks gate success, application deployments
are locked against concurrency, and failed deployments restore the prior
release. File-watch output identifies added, modified, and removed paths before
triggering rsync's delta transfer. Conflicting Nginx domains and occupied ports
are rejected. The most recent releases are retained for rollback.

## Website administration

Refresh **Websites** and select an application to load its server settings. Change
domains/subdomains, request limits, burst limits, and upload size, then apply.
The Nginx editor supports multiline typing, paste, arrow navigation, Home/End,
and scrolling; copying copies the full config. Every save runs `nginx -t` before
reload and restores the old file on validation/reload failure. A global lock
prevents concurrent Across Nginx operations. Custom raw edits are replaced by
future deployment or settings generation; structured website settings persist
across redeployments and rollbacks.

In **Domains & HTTPS**, save the website hostnames first. DNS pointing supports
Cloudflare A/AAAA records using a zone ID and a local token file containing only
the API token. The token needs DNS edit permission for that zone. Credentials
are passed to curl through a private temporary header file and removed afterward.
Pointing DNS creates a record or updates the single matching record; ambiguous
multiple records are rejected. Other DNS providers require their own control
panel. DNS changes do not change registrar nameservers or cloud security groups.

**Add HTTPS** obtains a Let's Encrypt certificate for all configured hostnames
using Certbot's Nginx authenticator. Issuance accepts Let's Encrypt's terms using
the supplied account email. Public DNS and port 80 must reach the VPS. Wildcard
certificate issuance needs DNS challenges and is not supported here. Certificates
are saved into website settings, HTTP redirects to HTTPS, and a systemd timer
runs renewal twice daily with a Nginx reload hook. **Renew HTTPS** renews when due,
rather than forcing unnecessary issuance. Existing certificate paths remain
supported in manifests. Certbot installation is automated for apt/dnf systems.

**Resources** reads uptime, memory, disk, and systemd service state. CPU quota and
memory limits apply to `nginx.service` or `across-pm2.service` (all managed Node
apps together); they do not resize the cloud VPS. **Firewall** supports UFW status,
installation on apt systems, enabling with the SSH port preserved, allow/deny
rules and deletion of allow rules. Allow ports 80/443 as needed before enabling.
Blocking or deleting the selected SSH port is rejected. Cloud security groups
must also allow the necessary ports separately.

## CI/CD

Use **Projects → Deploy from GitHub** or open **Pipelines**:

1. Install GitHub CLI (`brew install gh` on macOS; [other platforms](https://cli.github.com/)).
   Click **Connect GitHub** to open GitHub in your default browser. The one-time
   device code is copied to your clipboard; paste it into GitHub and authorize.
   The code is also shown in Activity if clipboard access is unavailable. Across requests repository and workflow access;
   GitHub CLI manages authentication credentials. **Refresh repos** also works with
   an existing GitHub CLI login that has these scopes.
   The button changes to **Connected** and the repository chooser opens when
   authorization completes. Pipelines also checks your existing login automatically
   after reopening Across. Repository lookup failures show an error without losing
   your connected account.
2. Click **Choose repo** to select a writable repository (including organization
   repositories), or enter `owner/repository`. Choose the deployment branch.
3. Click **Clone project**. In Projects, configure your build/start commands,
   domains, and other deployment settings, then **Save**. For an existing local
   checkout, enter its repository root in Pipelines instead. The manifest path is
   relative to that root, for example `frontend/across.json`.
4. Click **Environment keys** to add application names and values (for example
   `DATABASE_URL` or `NEXT_PUBLIC_API_URL`). Values are masked; each repository has
   its own in-memory list. Add or update a key, or remove it before setup. Values
   are not saved in desktop settings, so unfinished entries must be re-entered
   after closing Across. Up to 95 application keys are supported, with values up
   to 48 KB each. Deployment tool names such as `PATH`, `PORT`, and `ACROSS_*`
   are reserved. `PORT` comes from project settings.
5. Select the destination VPS. Install its backend in Connection first, then
   click **Deploy with GitHub**. This button becomes available after choosing a
   repository folder containing the saved manifest. This publishes a commit to the selected branch,
   containing the saved manifest, `.github/workflows/across-deploy.yml`, and the
   C++ deployment client in `.across/client`. The manifest's project path is made
   relative so it works on the runner. Imported manifests with machine-specific
   absolute paths are rebased to their containing project folder; repository-relative
   subproject paths are preserved. Empty install commands use `npm ci` when a
   package lock exists, or `npm install` for projects with only `package.json`. Local source edits are not published by
   this action; commit and push application changes through your normal Git flow.
6. Across checks VPS access, generates and verifies a dedicated CI SSH key,
   installs its public key in the SSH account's `authorized_keys`, and configures
   encrypted repository secrets: `ACROSS_HOST`, `ACROSS_USER`, `ACROSS_PORT`,
   `ACROSS_SSH_KEY`, and `ACROSS_KNOWN_HOSTS`. Private key material is staged in
   a private temporary directory and removed after setup; your personal private
   key is not uploaded. Repository administration permission is required.
7. The workflow deploys that commit and subsequent pushes over SSH. **View runs**
   shows the latest workflow statuses in Activity; open your repository's Actions
   page for full logs. A published workflow is not proof of a successful deploy.

Application values are stored as encrypted repository secrets named
`ACROSS_ENV_<KEY>`. The generated deployment step maps these to the original
names for application builds. A private JSON file transfers the keys over SSH
for VPS builds and Node/PM2 runtime; it is stored outside publicly served release
files with owner-only permissions. Rollback uses that release's environment.
An older VPS backend is updated during setup when keys are configured. Secret
values never enter the workflow, manifest, source commit, or setup logs. Frontend
frameworks can embed public environment values into browser bundles; use their
public prefixes only for values intended for visitors. After setup, update an
existing value in the repository's Actions secrets settings. Adding a new name
to an existing pipeline also requires adding its env mapping and Python key list
in the workflow. ([GitHub secrets reference](https://docs.github.com/en/actions/reference/security/secrets))

The runner compiles the client and deploys with a verified VPS host key. Allow
runner access to the VPS SSH port in your cloud firewall. The workflow uses
GitHub's `production` environment; its approval and branch rules still apply.
Protected branches can reject the setup commit. Existing Across pipelines are
preserved; configure them manually or use their existing Actions workflow.
Setup may leave the dedicated authorized key and some repository secrets if a
later step fails; inspect Activity before retrying. Remove the public key with
comment `across-github-owner/repository` from the VPS to revoke CI access.
The CI key has the same SSH account permissions as the selected administrator.

**Create local workflow** still generates CI files without publishing or setting
secrets. **Projects → Deploy** continues to deploy directly from your local
folder, and **Watch** continues its local automatic deployment behavior.
`.github` and `.across` are excluded from deployment uploads.

## Server commands

The same operations are available through `across manage SERVER -- COMMAND`:

```sh
./build/across manage production -- website --app my-app
./build/across manage production -- website-settings --app my-app --domains 'example.com www.example.com' --body-mb 40
./build/across manage production -- tls-issue --app my-app --email owner@example.com
./build/across manage production -- tls-renew --app my-app
./build/across manage production -- metrics
./build/across manage production -- resource-limits --service nginx.service --cpu 100 --memory-mb 512
./build/across manage production -- firewall --action status
./build/across manage production -- firewall --action allow --port 443 --protocol tcp --ssh-port 22
```

The server must run Linux with systemd and a C++20 compiler/CMake 3.20+ for
bootstrap. Passwordless sudo is needed for privileged operations. File watch
uses polling; project rsync sends changed files after debounce. SCP/SFTP copies
depend on the corresponding OpenSSH server support. Raw file transfers do not
apply project deployment exclusions.

Tests cover validation, process timeouts, workflow generation, GitHub setup/authentication/secret failures
with simulated GitHub and SSH plus real Git publishing and key generation, transfer command
construction with fake executables, website settings persistence, Nginx rollback
on rejection/reload failure, and SSH-port protection. Server integration tests
use an isolated filesystem and simulated system commands; they do not prove live
VPS behavior. A real Linux deployment and DNS/certificate credentials are needed
for end-to-end verification.

See [docs/architecture.md](docs/architecture.md) for the deployment transaction.

In **Domains**, **Block IP access** enables domain-only access for the entire
selected server. Unknown HTTP hostnames receive 403 and unknown HTTPS names
are rejected. Across backs up the default welcome site, tests and reloads Nginx,
and restores the previous configuration if validation or reload fails.
**Restore IP access** restores the previous default site. Websites configured
with an IP hostname must first be switched to a domain. This is hostname
routing, not a firewall that prevents connecting to the origin with a valid hostname.

## Security and Monitoring

**Security** reads actual server accounts, processes, listening ports, active TCP
connections, SSH sessions and journal events, UFW rules, Fail2ban jails, package
updates, and deployed website certificates. It provides process TERM/KILL,
website restart, port allow/block/removal, server IP and website visitor blocking,
SSH-key-only authentication, and persistent IPv4/IPv6 ping controls. PID 1,
SSH/system managers, the operation's ancestors, and the current administrator IP
are protected. Process actions require the selected process's start identity.

Fail2ban's SSH jail uses the system journal. You can install it and edit failed
attempt count, detection window, and ban duration. The current SSH administrator
is added to the jail's trusted IPs before activation. A server firewall IP block
works on the connecting peer; website visitor blocks work on the restored visitor
IP behind Cloudflare. ICMP ping is per IP, and Cloudflare's public proxy answers
are controlled by Cloudflare, not the origin server.

**Monitoring** provides server CPU samples, RAM/disk/interface counters and
network throughput, plus requests, visitor IPs, response bytes, error counts,
average/p95 latency, status codes, per-project and per-page statistics, and recent
request details. Tables have aligned columns, a five-row viewport, and a visible
scrollbar; scroll within a table to read more rows. Request and grouped traffic
views place the latest activity first. **Updated** shows when the app received
the report, while **Last seen** describes the traffic event. Timestamps are shown
in the desktop machine's local time. The Security SSH table places current TCP
connections before its newest journal events. Device/browser labels are inferred
from the reported user-agent.
Enable logging for the selected website first. Only published Cloudflare peer
ranges may supply the visitor-IP header. Direct clients cannot spoof it.

Traffic covers requests reaching the origin, so Cloudflare cache hits are not
counted. Historical traffic before logging was enabled is unavailable. Reports
scan current and previous daily logs (up to 8 MiB each), flag partial scans, and
support 1–48 hour windows. Log rotation retains 30 daily archives. Export saves
the complete captured JSON report locally; optional auto-refresh runs every 30
seconds while the Monitoring tab is open. Network throughput and CPU utilization
need two successive samples.

## Persistent storage and backups

Across currently uses JSON files, deployment directories, and native system
configuration/logs. It does not use SQLite or another database.

| Location | Contents |
|---|---|
| macOS: `~/Library/Application Support/Across/servers.json` | Server profiles, key paths, transfer preferences, and package selections |
| macOS: `~/Library/Application Support/Across/desktop.json` | Selected server/tab and recent project manifest paths per server |
| Same configuration folder: `known_hosts` | Across's trusted SSH host keys |
| Linux: `~/.config/across/` (or `$XDG_CONFIG_HOME/across/`) | Equivalent client configuration files |
| Project: `across.json` | Deployment commands, paths, domains, and website settings |
| VPS: `/var/lib/across/apps/<app-id>/` | Saved manifest, website overrides, releases, current release link, and app settings |
| VPS: `/var/lib/across/.pm2/` | Managed Node process state |
| VPS: `/var/log/nginx/across-<app-id>.access.jsonl` | Website request history when logging is enabled |
| VPS: system journal | SSH and service activity, subject to the system's retention policy |
| VPS: `/etc/nginx/`, `/etc/letsencrypt/`, `/etc/ufw/`, `/etc/fail2ban/`, `/etc/systemd/system/` | Native server configuration and certificates |

`ACROSS_CONFIG_DIR` overrides the local configuration directory. To open the
macOS directory in Finder, press **Command+Shift+G** and paste
`~/Library/Application Support/Across/`.

Back up the local configuration folder, your project manifests, and your SSH
keys through your normal protected backup process. A provider snapshot or a
server backup should also cover deployed app state, native configuration, and
any database/uploads used by your website. Across does not automatically back up
your website's database. Close the desktop before restoring its local JSON files.
Removing a local profile does not uninstall websites from the VPS.

Traffic summaries are reconstructed from access logs when refreshed. CPU and
network rates use successive live samples; Across does not store historical host
metrics. SSH history depends on the VPS journal, including whether that journal
persists across reboots. JSON report export saves a snapshot, not a background
monitoring database.

## Troubleshooting

| Symptom | Check or action |
|---|---|
| SSH permission denied | Check the username, selected private key, server `authorized_keys`, and key permissions; test the same account with `ssh` |
| Connection times out | Check the VPS address/SSH port, provider firewall, UFW, and your current administrator IP |
| `sudo: a password is required` | Test `sudo -n true`; configure passwordless sudo for the selected administrator account or use an authorized root account |
| Server host key changed | Verify the replacement VPS fingerprint through the provider console before updating Across's `known_hosts`; do not blindly bypass the mismatch |
| Install backend fails | Inspect Activity for the failing step; confirm Ubuntu/Debian, systemd, outbound package/download access, free disk space, and sufficient build memory |
| Backend command is unknown | Install/update the backend from Connection, then refresh |
| npm/build command fails | Check required Node version, lockfile, command, dependencies, and whether you selected local or VPS building |
| Deployment health check fails | Check that the start command stays running, listens on the manifest port, and serves the configured health path successfully |
| Domain shows another website or Nginx welcome page | Check DNS, the selected website's saved domains, and which server answers; use Block IP access only after domain routing works |
| HTTPS issuance fails | Check all configured A/AAAA records, public TCP 80, Nginx validity, and the exact Certbot error in Activity |
| Reopened app has missing/outdated websites | Select the correct server and click Refresh all; inspect Activity for SSH/backend failures and reopen local manifests through Recent projects |
| Files upload/save permission denied | Check ownership and permissions for the SSH account; file operations do not automatically use sudo |
| Files editor refuses to save | The file may have changed since opening, or exceed the 64 KiB text limit; reload it before editing again |
| Monitoring is empty | Enable logging for the selected website, generate origin requests, and refresh; cached Cloudflare responses do not reach origin logs |
| CPU says Sampling or network rate is zero | Take a second sample using Refresh traffic or enable auto-refresh |
| Only five table rows appear | Scroll inside the table or use its right-hand scrollbar; the footer shows the visible range and total |

For a read-only connection/backend check from the local CLI:

```sh
./build/across doctor production
./build/across apps production
```

See [docs/architecture.md](docs/architecture.md) for implementation details.
