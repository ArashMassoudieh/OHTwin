# SSH access to the AWS servers — notes for Behzad

Two EC2 instances, both `us-west-2`. **Same key, same user for both.**

| | Host | User | What runs there |
|---|---|---|---|
| **Twin server** | `52.42.223.42` (`openhydrotwin.com`) | `ubuntu` | All OHTwin services **and** the Qt WASM viewers |
| **Landing server** | `54.213.147.59` | `ubuntu` | AnacostiaIQ public page + `SensorDashboard.html`. No twin services. |

`52.42.223.42` is an **Elastic IP** — stable across instance stop/start.

Note: the WASM viewer is *not* on a separate machine. It is served by nginx from
`/var/www/drywelldt/` on the same host that runs the simulations.

## 1. Get the key in place

Arash will send you `ArashLinux.pem` directly. Do not commit it to git, put it in
Dropbox, or paste it into chat/email threads.

```bash
mkdir -p ~/.ssh
mv ~/Downloads/ArashLinux.pem ~/.ssh/
chmod 400 ~/.ssh/ArashLinux.pem       # SSH refuses keys readable by others
```

## 2. Connect

```bash
ssh -i ~/.ssh/ArashLinux.pem ubuntu@52.42.223.42      # twin server
ssh -i ~/.ssh/ArashLinux.pem ubuntu@54.213.147.59     # landing server
```

First connection asks to accept the host fingerprint — type `yes`.

## 3. Optional: shorter commands

Add to `~/.ssh/config`:

```
Host ohtwin
    HostName 52.42.223.42
    User ubuntu
    IdentityFile ~/.ssh/ArashLinux.pem
    ServerAliveInterval 60

Host anacostia
    HostName 54.213.147.59
    User ubuntu
    IdentityFile ~/.ssh/ArashLinux.pem
    ServerAliveInterval 60
```

Then just `ssh ohtwin` / `ssh anacostia`. Also enables `scp file ohtwin:~/`.

## 4. Layout on the twin server

```
/home/ubuntu/drywelldt/deployments/Bioretention_truth/          port 8084
/home/ubuntu/drywelldt/deployments/Bioretention_assimilation/   port 8085
/home/ubuntu/jm_twin/deployments/JM_forecast/
/var/www/drywelldt/                    Qt WASM viewers (.wasm/.js/.html)
/etc/nginx/sites-enabled/              routing
/etc/nginx/ohtwin-locations/           per-deployment location blocks
```

Each deployment has `config.json`, `model/`, `outputs/`, `state/`.

Services:

```
drywelldt@Bioretention_truth.service
drywelldt@Bioretention_assimilation.service
drywelldt-jm@JM_forecast.service
```

## 5. Read-only health checks

```bash
systemctl status drywelldt@Bioretention_assimilation
journalctl -u drywelldt@Bioretention_assimilation -n 50 --no-pager
journalctl -u drywelldt@Bioretention_assimilation -f          # live tail

# Is it actually producing output? (see the warning below)
ls -l --time-style=long-iso \
  /home/ubuntu/drywelldt/deployments/Bioretention_assimilation/outputs/

tail -5 /home/ubuntu/drywelldt/deployments/Bioretention_assimilation/outputs/run_log.csv
df -h /
```

## ⚠ Two things to know before touching anything

**"active" does not mean "working."** These services have two independent loops —
a forward loop that writes `selected_output.csv`, `viz.svg` and `forecast_viz.svg`
(this is what the web pages render) and a calibration loop that writes
`run_log.csv` and `calibration/`. The forward loop can be stalled for days while
systemd still reports `active` and `run_log.csv` keeps updating. **Always judge
health by the file timestamps in `outputs/`, not by `systemctl status`.**

**Do not casually restart the Bioretention services.** A plain `systemctl restart`
or a reboot resets the simulation clock to `start_datetime` but does **not** clear
`outputs/`. For a few seconds the truth then serves data from the previous run at a
much later simulated time; the assimilation reads that stale frontier on startup,
jumps ahead of the truth, and freezes its forward loop for days. This has already
happened seven times. If a restart is genuinely needed, start the **truth first**,
wait for it to rewrite its outputs, then start the assimilation — and check the
`outputs/` timestamps afterwards.

`JM_forecast` is not affected by this and is safe to restart on its own.

## 6. Deploying (only when asked)

`deploy.sh` (Bioretention) and `deploy_jm.sh` (JM) run **from Arash's machine**,
not on the server. They rsync the deployment, install the systemd unit, update
nginx and restart the service. Unlike a manual restart they wipe
`outputs/ state/ snapshots/`, so they do not hit the trap above.

Known wrinkle: `deploy.sh` has a stale `PEM_FILE` path (a Dropbox
"Selective Sync Conflict" folder that no longer exists). `deploy_jm.sh` points at
the correct location. Check before running either.
