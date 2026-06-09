# =============================================================================
# plot_paper_fitness_history.gp
#
# Plots per-cycle calibration fitness measures for the three calibrated
# observation channels of the Bioretention twin experiment, as three
# stacked panels sharing a single time axis:
#
#   (a) Nash-Sutcliffe efficiency
#   (b) Mean squared error (log scale)
#   (c) Per-observation negative log-likelihood
#
# Inputs:
#   Bioretention_assimilation/outputs/calibration/fitness_history.csv
#
# Output:
#   paper_fitness_history.png
#
# Run from the deployments/ directory:
#   gnuplot plot_paper_fitness_history.gp
# =============================================================================

set datafile separator ","
set datafile missing "nan"

infile  = "Bioretention_assimilation/outputs/calibration/fitness_history.csv"
outfile = "paper_fitness_history.png"

if (!exists("drift")) drift = 0
if (drift) {
    infile  = "Bioretention_assimilation_drift/outputs/calibration/fitness_history.csv"
    outfile = "paper_fitness_history_drift.png"
}

# --- output canvas ----------------------------------------------------------
# Tall portrait for three stacked panels; larger font for paper rendering.
set terminal pngcairo size 1100,1400 enhanced font "Helvetica,18"
set output outfile

# --- x-axis: simulated calendar date ---------------------------------------
# fitness_history.csv column 3 is t_now in OHQ day-serial (Excel epoch
# 1899-12-30 UTC). Convert to Unix epoch seconds for gnuplot's time axis:
#   unix = (t_now - 25569) * 86400
# where 25569 = days from 1899-12-30 to 1970-01-01.
epoch(d) = (d - 25569.0) * 86400.0

set xdata time
set timefmt "%s"
set format x "%Y-%m"

# Channel colors (colorblind-friendly)
c_pond  = "#1f77b4"
c_soil  = "#2ca02c"
c_under = "#d62728"

ps = 1.0
lw_line = 2.2

# --- shared layout ----------------------------------------------------------
# Three stacked panels, single shared time axis. Suppress x tic labels and
# x-axis label on the upper two; only the bottom panel shows them.
set multiplot layout 3,1

set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3

# Columns in fitness_history.csv:
#   1: cycle
#   2: timestamp
#   3: t_now
#   4: likelihood
#   5,6,7:    Pond MSE, R2, NSE
#   8,9,10:   Soil MSE, R2, NSE
#   11,12,13: Underdrain MSE, R2, NSE
#  14: buffer_obs_per_channel
#  15: sum_w_per_channel
#  16: sum_w_total
#  17: likelihood_per_w

# =============================================================================
# Panel (a): NSE -- legend lives here in the bottom-right corner
# =============================================================================
set format x ""
unset xlabel

set key bottom right inside box opaque samplen 2 spacing 1.2 \
    font "Helvetica,16" width 0
set ylabel "NSE" offset 1,0
set yrange [-1.05:1.05]
set ytics 0.5
set title "(a) Nash-Sutcliffe efficiency" font "Helvetica,18"

# NSE can be strongly negative early in calibration; clip at -1 for display.
clip(y) = (y < -1.0 ? -1.0 : y)

plot \
    infile using (epoch($3)):(clip($7))  with linespoints \
        lc rgb c_pond  lw lw_line pt 7 ps ps title "Pond water depth", \
    infile using (epoch($3)):(clip($10)) with linespoints \
        lc rgb c_soil  lw lw_line pt 5 ps ps title "Soil moisture", \
    infile using (epoch($3)):(clip($13)) with linespoints \
        lc rgb c_under lw lw_line pt 9 ps ps title "Underdrain flow"

unset key

# =============================================================================
# Panel (b): MSE over time (log scale)
# =============================================================================
set ylabel "MSE (channel-native units^2)" offset 1,0
set logscale y
set yrange [*:*]
set format y "10^{%T}"
set ytics autofreq
set title "(b) Mean squared error" font "Helvetica,18"

plot \
    infile using (epoch($3)):5  with linespoints \
        lc rgb c_pond  lw lw_line pt 7 ps ps notitle, \
    infile using (epoch($3)):8  with linespoints \
        lc rgb c_soil  lw lw_line pt 5 ps ps notitle, \
    infile using (epoch($3)):11 with linespoints \
        lc rgb c_under lw lw_line pt 9 ps ps notitle

unset logscale y
set format y "%g"

# =============================================================================
# Panel (c): Per-observation negative log-likelihood
# =============================================================================
# Raw `likelihood` (col 4) grows in magnitude as the unbounded WLS buffer
# accumulates observations: sum_w increases monotonically with simulated
# time, so even a perfectly-calibrated model produces an ever-more-negative
# likelihood. Dividing by the total effective sample size (col 16,
# sum_w_total) yields a per-effective-observation NLL that stabilises once
# calibration has converged and only moves in response to genuine fit
# changes. Column 17 is `likelihood / sum_w_total`. The first cycle has
# sum_w ~ 1 and lik/w is meaningless; suppress it via a using-filter.
set format x "%Y-%m"
# Place a tic every 3 months (3 * 30.4375 days, in seconds) so labels
# don't collide on the narrow bottom panel.
set xtics 7889400
set xlabel "Simulated date" offset 0,-0.5
# Add bottom margin so x labels and xlabel have room.
set bmargin 5
set ylabel "Likelihood / effective N" offset 1,0
set yrange [*:*]
set ytics autofreq
set title "(c) Per-observation negative log-likelihood" font "Helvetica,18"

plot \
    infile using (epoch($3)):($16 > 10 ? $17 : NaN) with linespoints \
        lc rgb "#444444" lw lw_line pt 7 ps ps notitle

unset multiplot
unset output
