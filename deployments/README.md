# Bioretention Truth + Forward Two-Deployment Run

This documents the procedure for running the Truth Twin and the Forward
Twin in tandem on the same machine, with the Forward Twin polling the
Truth Twin's observations over HTTP.

## Architecture

```
   ┌────────────────────────────────────┐         ┌────────────────────────────────────┐
   │ Bioretention_truth                 │         │ Bioretention_forward               │
   │   - generates noisy observations   │         │   - same model, will be perturbed  │
   │   - writes selected_output.csv     │         │     for the actual experiment      │
   │   - writes selected_output_meta.   │         │   - polls truth's CSV every 10s    │
   │     json (sigma, tau, etc)         │         │   - holds it in DTObservationBuffer│
   │   - nginx serves on :8084          │         │   - nginx serves on :8085          │
   └────────────────────────────────────┘         └────────────────────────────────────┘
                  │                                                │
                  └────────► HTTP GET (every 10 s) ◄───────────────┘
```

## One-time setup

```bash
# 1. Run the setup script (creates dir, copies model/viz, writes config.json)
bash setup_forward.sh

# 2. Install the nginx site config
sudo cp bioretention_forward.nginx /etc/nginx/sites-available/bioretention_forward
sudo ln -sf /etc/nginx/sites-available/bioretention_forward \
            /etc/nginx/sites-enabled/bioretention_forward

# 3. Test and reload nginx
sudo nginx -t && sudo systemctl reload nginx

# 4. Verify port 8085 is up (will 404 until forward writes outputs, but headers should be OK)
curl -I http://localhost:8085/outputs/
```

## Running the two twins

You'll need two terminals.

**Terminal 1 — Truth Twin** (start this first; let it accumulate some
observations before launching forward):

```bash
cd /home/arash/Projects/OpenHydroTwin
./build-qmake/bin/OHTwin --deployment deployments/Bioretention_truth
```

Wait until you see at least 2-3 cycles complete (each cycle at 1000x
acceleration takes ~86 seconds of wall time for a simulated day).

**Terminal 2 — Forward Twin:**

```bash
./build-qmake/bin/OHTwin --deployment deployments/Bioretention_forward
```

## What to look for in the forward twin's logs

```
[Config] assim.csv_url     : http://localhost:8084/outputs/selected_output.csv
[Config] assim.meta_url    : http://localhost:8084/outputs/selected_output_meta.json
[Config] assim.poll_int    : 10000 ms
...
[Assim] initial refresh OK — N points across M variables
[Assim] poll timer started — interval 10000 ms
...
[Assim] poll OK — N points buffered      (every 10s)
```

If the URLs are unreachable, you'll see:

```
[ObsBuf] CSV fetch failed: ...
[Assim] initial refresh failed: ...   (will retry on poll timer)
[Assim] poll failed: ...
```

The forward twin should keep running its forward simulation regardless
of polling outcomes — assimilation failures are non-fatal.

## Sanity checks

```bash
# How many points are in truth's CSV right now?
curl -s http://localhost:8084/outputs/selected_output.csv | wc -l

# After ~10s of polling, the forward twin should report a similar count.
# Check its stdout for "[Assim] poll OK — N points buffered" lines.

# Verify forward twin is also writing its own outputs:
curl -I http://localhost:8085/outputs/selected_output.csv

# (Won't exist until forward twin completes its first cycle.)
```

## Stopping

Ctrl+C in each terminal. Both twins write a state snapshot on shutdown.

## When to revert poll_interval

`10s` is for verification only — it's noisy in the logs and triggers
unnecessary HTTP traffic. For real assimilation experiments, bump it to
something matching the desired calibration cadence (e.g. `5min` or
`1hr`). Don't run with 10s overnight; it'll fill journals quickly.
