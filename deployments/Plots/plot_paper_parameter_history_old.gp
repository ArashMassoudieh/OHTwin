#!/usr/bin/env gnuplot
#
# plot_paper_parameter_history.gp
#
# Paper figure: three stacked panels showing the trajectory of each
# calibrated parameter across assimilation cycles, with a horizontal
# reference line at the truth value (twin-experiment validation).
#
#   (a) EngineeredSoilKsat  (truth: 10.0)
#   (b) NativeSoilAlpha     (truth: 1.0)
#   (c) NativeSoilKsat      (truth: 0.01)
#
# Panel boxes are placed with `set lmargin/rmargin/tmargin/bmargin at
# screen` (the gnuplot-recommended way for stacked plots) so all three
# data panels are *exactly* the same size, regardless of which one carries
# the date labels and xlabel.
#
# Run from within the deployment folder:
#     gnuplot plot_paper_parameter_history.gp
# For the drift case (uses Bioretention_assimilation_drift, writes
# paper_parameter_history_drift.png):
#     gnuplot -e "drift=1" plot_paper_parameter_history.gp
#
# Output: paper_parameter_history[_drift].png

if (!exists("drift")) drift = 0
if (drift) {
    assim_dir = "Bioretention_assimilation_drift"
    suffix    = "_drift"
} else {
    assim_dir = "Bioretention_assimilation"
    suffix    = ""
}

# ---------- file path ----------
param_file = assim_dir . "/outputs/calibration/parameter_history.csv"

# ---------- truth values (edit these to match your synthetic-truth setup) ----------
truth_EngK   = 10.0
truth_NSAlph = 1.0
truth_NSKsat = 0.01

set datafile separator ","
set datafile commentschars "#c"   # skip header (starts with 'c' for "cycle")

# ---------- canvas ----------
set terminal pngcairo size 1800,1500 enhanced font "Helvetica,22"
set output "paper_parameter_history" . suffix . ".png"

set multiplot

# ---------- styling ----------
set grid lc rgb "#cccccc" lw 0.8
set border lw 1.5

set style line 1 lc rgb "#d62728" lw 2.6 dt solid pt 7 ps 1.0
set style line 2 lc rgb "#666666" lw 1.5 dt (8,4)

# ---------- screen-coordinate layout ----------
# Define the rectangle that all three panels share, then set explicit
# top/bottom screen positions for each panel so heights are exactly equal.
left_x   = 0.16
right_x  = 0.96

# The three panels collectively occupy [bottom_strip .. top_of_first].
# Reserve space below for the rotated date labels and "Date" xlabel.
panel_top_y    = 0.96    # top edge of panel (a)
panel_bottom_y = 0.18    # bottom edge of panel (c) — leaves room for date axis
gap            = 0.0     # gap between adjacent panels (0 = touching)

n_panels       = 3
total_height   = panel_top_y - panel_bottom_y - (n_panels - 1) * gap
panel_h        = total_height / n_panels

# Compute each panel's top and bottom screen y.
top_a = panel_top_y
bot_a = top_a - panel_h

top_b = bot_a - gap
bot_b = top_b - panel_h

top_c = bot_b - gap
bot_c = top_c - panel_h    # == panel_bottom_y by construction

# ---------- common panel setup ----------
set tics font "Helvetica,18" nomirror
unset key
set autoscale

set xdata time
set timefmt "%s"

set format x ""
unset xlabel

set ylabel font "Helvetica,24" offset -1.5,0

# Horizontal margins are the same on all panels.
set lmargin at screen left_x
set rmargin at screen right_x

# ---------- Panel (a): EngineeredSoilKsat ----------
set tmargin at screen top_a
set bmargin at screen bot_a
unset logscale y
set format y "%g"
set yrange [0:*]
set ylabel "Engineered\nSoil K_{sat} [m/day]"
set label 1 "(a)" at graph 0.015, 0.86 font "Helvetica,22" front
plot truth_EngK with lines ls 2 notitle, \
     param_file using (($3-25569)*86400):4 with linespoints ls 1 notitle

# ---------- Panel (b): NativeSoilAlpha ----------
set tmargin at screen top_b
set bmargin at screen bot_b
unset logscale y
set format y "%g"
set yrange [0:*]
set ylabel "Native\nSoil α [1/m]"
set label 1 "(b)" at graph 0.015, 0.86 font "Helvetica,22" front
plot truth_NSAlph with lines ls 2 notitle, \
     param_file using (($3-25569)*86400):5 with linespoints ls 1 notitle

# ---------- Panel (c, bottom): NativeSoilKsat ----------
# Log scale; truth (0.01) is the GA's lower bound.
set tmargin at screen top_c
set bmargin at screen bot_c
set ylabel "Native\nSoil K_{sat} [m/day]"
set label 1 "(c)" at graph 0.015, 0.86 font "Helvetica,22" front
set logscale y
set yrange [0.008:0.2]
set format y "10^{%L}"
set format x "%Y-%m-%d"
set xtics rotate by -30 offset 0,-0.3 font "Helvetica,18"
set xlabel "Date" font "Helvetica,24" offset 0,-1.5
plot truth_NSKsat with lines ls 2 notitle, \
     param_file using (($3-25569)*86400):6 with linespoints ls 1 notitle

unset multiplot
unset output

print "Wrote paper_parameter_history" . suffix . ".png"
