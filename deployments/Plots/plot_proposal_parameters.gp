# =============================================================================
# plot_proposal_parameters.gp
#
# Condensed TWO-panel version of the Bioretention parameter-recovery figure,
# for the NSF proposal. Shows:
#   (a) Engineered-soil saturated conductivity -- estimate tracks the DRIFTING
#       true Ksat (clogging analog). The true curve is read from the drift file,
#       NOT drawn as a constant.
#   (b) Runoff coefficient -- clean stationary recovery (constant truth).
# laid out side by side (1 row x 2 cols), sharing the simulated date axis.
#
# Inputs:
#   ../Bioretention_assimilation_drift/outputs/calibration/parameter_history.csv
#   ../Bioretention_truth_drift/drift/ksat_drift.csv   (true drifting Ksat_eng)
#
# Output:
#   proposal_parameters.pdf
#
# Run from the deployments/<thisdir>/ directory (paths are relative, "../").
#
# EDIT the drift-file column mapping below to match ksat_drift.csv.
# =============================================================================

set datafile separator ","
set datafile missing "nan"

infile    = "../Bioretention_assimilation_drift/outputs/calibration/parameter_history.csv"
driftfile = "../Bioretention_truth_drift/drift/ksat_drift.csv"
outfile   = "proposal_parameters.pdf"

# --- drift-file column mapping (EDIT THESE TO MATCH ksat_drift.csv) ----------
t_col_drift = 1          # column holding the time value
k_col_drift = 2          # column holding the true Ksat_eng (m/day)
drift_time_is_serial = 1 # 1 = OHQ day-serial (convert w/ epoch); 0 = already Unix seconds
drift_has_header = 0     # ksat_drift.csv has NO header row

set terminal pdfcairo size 9.0in,3.2in enhanced font "Helvetica,13"
set output outfile

epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"
dtime(v) = drift_time_is_serial ? epoch(v) : (v)

true_runoff = 0.8

set multiplot layout 1,2
set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3

c_est   = "#1f77b4"
c_truth = "#d62728"
ps       = 0.7
lw_line  = 2.2
lw_truth = 2.2

set format x "%Y-%m"
set xtics 7889400 rotate by -30 offset 0,-0.3
set xlabel "Simulated date" offset 0,-0.6
set bmargin 4.5

skip = drift_has_header ? 1 : 0

# --- Panel (a): estimate vs DRIFTING truth -----------------------------------
set ylabel "K_{sat,eng} (m/day)" offset 1.5,0
set yrange [0:*]
set ytics autofreq
set title "(a) Engineered-soil conductivity: tracks imposed drift" font "Helvetica,13"
set key top right inside box opaque samplen 2 spacing 1.2 font "Helvetica,11" width 0

plot \
    driftfile every ::skip using (dtime(column(t_col_drift))):(column(k_col_drift)) \
        with lines dt 2 lc rgb c_truth lw lw_truth title "Synthetic truth (drift)", \
    infile using (epoch($3)):5 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps title "Estimated"

# --- Panel (b): stationary runoff coefficient --------------------------------
unset key
set ylabel "Runoff coefficient (-)" offset 1.5,0
set yrange [0:1]
set ytics autofreq
set title "(b) Runoff coefficient: held at stationary truth" font "Helvetica,13"

plot \
    true_runoff with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
    infile using (epoch($3)):9 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps notitle

unset multiplot
unset output
