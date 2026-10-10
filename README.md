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

The desktop has dedicated Projects, Websites, Domains & HTTPS, Files, Pipelines,
Resources, Firewall, Packages, and Connection screens. Deploy Node.js, static, and reverse
proxy websites, manage multiple VPS profiles and websites, change Nginx settings
or edit its config, point Cloudflare DNS, issue and renew certificates, inspect
VPS resources, set service limits, and manage host firewall rules.

Across restores the selected server, page, and recent local project manifests
when reopened. Selecting a server automatically reloads its deployed websites,
domain and HTTPS settings, resources, and package versions. **Refresh all** in
the header repeats these live checks; partial failures appear in Activity.
The Projects screen's **Recent projects** menu is saved separately for each server.

## Build

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

The macOS bundle includes the remote CLI source needed for server bootstrap, so
the `.app` can be moved independently of the source checkout. The desktop UI
bundles Inter Regular and Semibold rather than relying on Raylib's default font
or a system-installed typeface. Inter's license is included at
[assets/fonts/INTER-LICENSE.txt](assets/fonts/INTER-LICENSE.txt).

## First server

The desktop flow is: save a server in **Connection**, choose packages in
**Packages**, click **Install backend** in Connection, select an `across.json`,
and click **Deploy**. The equivalent local CLI flow is:

```sh
./build/across server add production \
  --host 203.0.113.10 --user ubuntu --identity /path/to/key.pem
./build/across bootstrap production
./build/across doctor production
./build/across deploy production /path/to/project/across.json
```

Across never stores passwords or private-key contents. Profiles only store the
path to an optional private key. SSH agent authentication also works. A private
known-hosts file uses trust-on-first-use and rejects later host-key changes.

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

In **Pipelines**, select a project directory, branch, and manifest path. Create
workflow writes `.github/workflows/across-deploy.yml` and includes the C++ client
source under `.across/client`. Commit both directories, add the secrets listed in
the UI, and configure GitHub's `production` environment as appropriate. Existing
pipelines are preserved rather than overwritten. The runner compiles the C++
client and deploys each push through SSH with a verified known-hosts entry.
Bootstrap the VPS before the first workflow run. Workflow creation is local;
repository publishing, secret configuration, and a successful CI run are separate
steps. `.github` and `.across` are excluded from deployment uploads.

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

Tests cover validation, process timeouts, workflow generation, transfer command
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
