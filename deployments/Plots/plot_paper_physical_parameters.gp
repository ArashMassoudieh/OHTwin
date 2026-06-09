# =============================================================================
# plot_paper_physical_parameters.gp
#
# Plots per-cycle calibrated trajectories of the six physical parameters
# in the Bioretention twin experiment, each with a horizontal dashed line
# at its synthetic-truth value. Six panels (3 rows x 2 columns) share the
# simulated time axis (date labels on the bottom row only).
#
# Inputs:
#   Bioretention_assimilation/outputs/calibration/parameter_history.csv
#
# Output:
#   paper_physical_parameters.png
#
# Run from the deployments/ directory:
#   gnuplot plot_paper_physical_parameters.gp
# =============================================================================

set datafile separator ","
set datafile missing "nan"

infile  = "Bioretention_assimilation/outputs/calibration/parameter_history.csv"
outfile = "paper_physical_parameters.png"

if (!exists("drift")) drift = 0
if (drift) {
    infile  = "Bioretention_assimilation_drift/outputs/calibration/parameter_history.csv"
    outfile = "paper_physical_parameters_drift.png"
}

# --- output canvas ----------------------------------------------------------
# Landscape for a 3x2 grid of panels.
set terminal pngcairo size 1600,1500 enhanced font "Helvetica,18"
set output outfile

# --- x-axis: simulated calendar date ---------------------------------------
# parameter_history.csv column 3 is t_now in OHQ day-serial (Excel epoch
# 1899-12-30 UTC). Convert to Unix epoch seconds for gnuplot's time axis:
#   unix = (t_now - 25569) * 86400
epoch(d) = (d - 25569.0) * 86400.0

set xdata time
set timefmt "%s"

# --- synthetic-truth values ------------------------------------------------
# These are the values used to generate the synthetic observation series.
true_alpha_eng = 1.0
true_Ksat_eng  = 10.0
true_n_eng     = 1.41
true_Ksat_nat  = 0.01
true_n_nat     = 1.41
true_runoff    = 0.8

# --- shared style ----------------------------------------------------------
set multiplot layout 3,2

set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3
unset key

c_est   = "#1f77b4"   # estimated trajectory
c_truth = "#d62728"   # truth reference line

ps = 1.0
lw_line = 2.2
lw_truth = 2.2

# Columns in parameter_history.csv:
#   1: cycle
#   2: timestamp
#   3: t_now
#   4: EngineeredSoilAlpha
#   5: EngineeredSoilKsat
#   6: EngineeredSoiln
#   7: NativeSoilKsat
#   8: NativeSoiln
#   9: RunoffCoeff
#  10: Std_PondWaterDepth
#  11: Std_SoilMoisture
#  12: Std_UnderdrainFlow

# Upper two rows: suppress x-tic labels and xlabel.
set format x ""
unset xlabel

# =============================================================================
# Panel (a): EngineeredSoilAlpha
# =============================================================================
set ylabel "{/Symbol a}_{eng} (1/m)" offset 1,0
set yrange [0:*]
set ytics autofreq
set title "(a) Engineered soil van Genuchten {/Symbol a}" font "Helvetica,18"

plot \
    true_alpha_eng with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
    infile using (epoch($3)):4 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps notitle

# =============================================================================
# Panel (b): EngineeredSoilKsat
# =============================================================================
set ylabel "K_{sat,eng} (m/day)" offset 1,0
set yrange [0:*]
set ytics autofreq
set title "(b) Engineered soil saturated conductivity" font "Helvetica,18"

plot \
    true_Ksat_eng with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
    infile using (epoch($3)):5 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps notitle

# =============================================================================
# Panel (c): EngineeredSoiln
# =============================================================================
set ylabel "n_{eng} (-)" offset 1,0
set yrange [*:*]
set ytics autofreq
set title "(c) Engineered soil van Genuchten n" font "Helvetica,18"

plot \
    true_n_eng with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
    infile using (epoch($3)):6 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps notitle

# =============================================================================
# Panel (d): NativeSoilKsat -- log scale (truth at GA lower bound)
# =============================================================================
set ylabel "K_{sat,nat} (m/day)" offset 1,0
set logscale y
set yrange [0.008:0.2]
set format y "10^{%L}"
set ytics autofreq
set title "(d) Native soil saturated conductivity" font "Helvetica,18"

plot \
    true_Ksat_nat with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
    infile using (epoch($3)):7 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps notitle

# Reset y formatting for the bottom row.
unset logscale y
set format y "%g"

# =============================================================================
# Bottom row (e,f): gets x-axis date labels and xlabel.
# =============================================================================
set format x "%Y-%m"
# Tic every 3 months (3 * 30.4375 days, in seconds) so labels don't collide.
set xtics 7889400 rotate by -30 offset 0,-0.3
set xlabel "Simulated date" offset 0,-0.8
set bmargin 5

# =============================================================================
# Panel (e): NativeSoiln
# =============================================================================
set ylabel "n_{nat} (-)" offset 1,0
set yrange [*:*]
set ytics autofreq
set title "(e) Native soil van Genuchten n" font "Helvetica,18"

plot \
    true_n_nat with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
    infile using (epoch($3)):8 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps notitle

# =============================================================================
# Panel (f): RunoffCoeff -- carries the single figure legend.
# =============================================================================
set ylabel "Runoff coefficient (-)" offset 1,0
set yrange [0:1]
set ytics autofreq
set title "(f) Runoff coefficient" font "Helvetica,18"

set key top right inside box opaque samplen 2 spacing 1.2 \
    font "Helvetica,16" width 0

plot \
    true_runoff with lines dt 2 lc rgb c_truth lw lw_truth title "Synthetic truth", \
    infile using (epoch($3)):9 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps title "Estimated"

unset multiplot
unset output
