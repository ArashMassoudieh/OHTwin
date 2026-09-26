# OHTwin
OHTwin is an open-source stormwater digital twin framework built around the OpenHydroQual simulation engine.
It provides a continuously running, sensor-coupled environment for real-time hydrologic simulation, state estimation, forecasting, visualization, and data assimilation of distributed stormwater infrastructure systems.

The framework was developed primarily for stormwater infiltration systems such as:

dry wells,
bioretention systems,
bioswales,
stormwater ponds,
engineered infiltration galleries,
and related green infrastructure assets.

OHTwin combines:

physically based hydrologic simulation,
online weather ingestion,
live observations,
data assimilation,
calibration/optimization,
state snapshots,
and browser-based visualization

into a unified digital twin architecture.

## OpenHydroQual resources

Clone `OHTwin` and `OpenHydroQual` as sibling directories. The `resources`
entry in OHTwin is a relative symlink to `../OpenHydroQual/resources`; the
build already uses source files from that sibling checkout. For example:

```sh
git clone https://github.com/ArashMassoudieh/OpenHydroQual.git
git clone https://github.com/ArashMassoudieh/OHTwin.git
```

Pull OpenHydroQual to use its latest resource templates locally:

```sh
git -C ../OpenHydroQual pull
```

Run this from the OHTwin directory. Check the OpenHydroQual changes before
deploying: `deploy.sh` and `deploy_jm.sh` copy the current JSON and list
templates to the server, so deployed copies update on the next deployment.
The former OHTwin-only `Pond_Plugin.json.bak_depth` is retained in
`resource_backups/`.
